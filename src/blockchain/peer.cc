#include "peer.h"

#include "common.h"
#include "graph.h"
#include "leveldb/db.h"
#include "smart_contracts.h"

INITIALIZE_EASYLOGGINGPP

Architecture arch;
Episode ep;
Document peer_config;
leveldb::DB *db;
leveldb::Options options;
Queue<TransactionProposal> proposal_queue;
Queue<string> ordering_queue;
Queue<TransactionProposal> execution_queue;
OXIIHelper oxii_helper;
shared_ptr<grpc::Channel> leader_channel;
bool is_leader = false;
uint64_t block_index = 0;
uint64_t transaction_count = 0;

string sha256(const string str) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256_CTX sha256;
    SHA256_Init(&sha256);
    SHA256_Update(&sha256, str.c_str(), str.size());
    SHA256_Final(hash, &sha256);
    stringstream ss;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        ss << hex << setw(2) << setfill('0') << (int)hash[i];
    }
    return ss.str();
}

bool validate_transaction(struct RecordVersion w_record_version, const Endorsement *transaction) {
    // logger->debug("******validating transaction[block_id = %v, trans_id = %v]******",
    //           w_record_version.version_blockid, w_record_version.version_transid);
    bool is_valid = true;

    for (int read_id = 0; read_id < transaction->read_set_size(); read_id++) {
        struct RecordVersion r_record_version;
        kv_get(transaction->read_set(read_id).read_key(), nullptr, &r_record_version);

        // logger->debug("read_key = %v\nstored_read_version = [block_id = %v, trans_id = %v]\n"
        //           "current_key_version = [block_id = %v, trans_id = %v]",
        //           transaction->read_set(read_id).read_key().c_str(),
        //           transaction->read_set(read_id).block_seq_num(),
        //           transaction->read_set(read_id).trans_seq_num(),
        //           r_record_version.version_blockid, r_record_version.version_transid);

        if (r_record_version.version_blockid != transaction->read_set(read_id).block_seq_num() ||
            r_record_version.version_transid != transaction->read_set(read_id).trans_seq_num()) {
            is_valid = false;
            break;
        }
    }

    if (is_valid) {
        for (int write_id = 0; write_id < transaction->write_set_size(); write_id++) {
            kv_put(transaction->write_set(write_id).write_key(), transaction->write_set(write_id).write_value(),
                   w_record_version, true, nullptr);

            // logger->debug("write_key = %v\nwrite_value = %v",
            //           transaction->write_set(write_id).write_key().c_str(),
            //           transaction->write_set(write_id).write_value().c_str());
        }
        // logger->debug("transaction is committed.\n");
    }
    return is_valid;
}

void *block_formation_thread(void *arg) {
    LOG(INFO) << "Block formation thread is running.";

    ifstream log(string(peer_config["sysconfig"]["log_dir"].GetString()) + "/raft.log", ios::in);
    assert(log.is_open());
    ofstream block_store(string(peer_config["sysconfig"]["log_dir"].GetString()) + "/blockchain.log", ios::out | ios::binary);
    assert(block_store.is_open());

    unsigned long last_applied = 0;
    int majority = (peer_config["sysconfig"]["followers"].Size() / 2) + 1;
    uint64_t trans_index = 0;

    Block block;
    string prev_block_hash = sha256(block.SerializeAsString());
    string serialized_block;
    queue<string> request_queue;

    while (true) {
        if (block_index < ep.B_n) {
            if (is_leader) {
                int N = commit_index + 1;
                int count = 0;
                for (int i = 0; i < match_index.size(); i++) {
                    if (match_index[i] >= N) {
                        count++;
                    }
                }
                if (count >= majority) {
                    commit_index = N;
                    // logger->debug("commit_index is updated to %v.", commit_index.load());
                }
            }

            if (commit_index > last_applied) {
                last_applied++;

                uint32_t size;
                log.read((char *)&size, sizeof(uint32_t));
                char *entry_ptr = (char *)malloc(size);
                log.read(entry_ptr, size);
                string serialized_request(entry_ptr, size);
                free(entry_ptr);

                LOG(DEBUG) << "[block_id = " << block_index << ", trans_id = " << trans_index << "]: added transaction to block.";
                request_queue.push(serialized_request);
                trans_index++;
                transaction_count++;

                if (trans_index >= arch.max_block_size) {
                    /* cut the block */
                    if (arch.reorder) {
                        if (arch.is_xov) {
                            xov_reorder(request_queue, block);
                            for (uint64_t i = 0; i < block.transactions_size(); i++) {
                                struct RecordVersion record_version = {
                                    .version_blockid = block_index,
                                    .version_transid = i,
                                };
                                if ((!block.mutable_transactions(i)->aborted()) &&
                                    validate_transaction(record_version, block.mutable_transactions(i))) {
                                    ep.total_ops++;
                                    block.mutable_transactions(i)->set_aborted(false);
                                } else {
                                    block.mutable_transactions(i)->set_aborted(true);
                                }
                            }
                        } else {
                            vector<TransactionProposal> proposals;
                            Graph conflict_graph;
                            build_conflict_graph_oxii(request_queue, proposals, conflict_graph);

                            oxii_helper.W.clear();
                            oxii_helper.C_clear();
                            oxii_helper.endorsements.clear();
                            for (int i = 0; i < proposals.size(); i++) {
                                oxii_helper.W.insert(i);
                                oxii_helper.endorsements.emplace_back();
                            }

                            while (!oxii_helper.W.empty()) {
                                for (auto it = oxii_helper.W.begin(); it != oxii_helper.W.end();) {
                                    uint64_t proposal_id = *it;
                                    bool all_pred_in_c = true;

                                    for (auto pred_it = conflict_graph[proposal_id].in_edges.begin();
                                         pred_it != conflict_graph[proposal_id].in_edges.end(); pred_it++) {
                                        if (!oxii_helper.C_find(*pred_it)) {
                                            all_pred_in_c = false;
                                            break;
                                        }
                                    }

                                    if (all_pred_in_c) {
                                        it = oxii_helper.W.erase(it);
                                        execution_queue.add(proposals[proposal_id]);
                                    } else {
                                        it++;
                                    }
                                }
                            }

                            while (oxii_helper.C_size() != proposals.size())
                                ;

                            for (int i = 0; i < oxii_helper.endorsements.size(); i++) {
                                Endorsement *endorsement = block.add_transactions();
                                (*endorsement) = oxii_helper.endorsements[i];
                            }
                        }
                    } else {
                        uint64_t trans_index_ = 0;
                        while (request_queue.size()) {
                            Endorsement *endorsement = block.add_transactions();
                            struct RecordVersion record_version = {
                                .version_blockid = block_index,
                                .version_transid = trans_index_,
                            };

                            if (arch.is_xov) {
                                /* validate */
                                if (!endorsement->ParseFromString(request_queue.front()) ||
                                    !endorsement->GetReflection()->GetUnknownFields(*endorsement).empty()) {
                                    LOG(WARNING) << "block formation thread: error in deserialising endorsement.";
                                    block.mutable_transactions()->RemoveLast();
                                } else {
                                    if (validate_transaction(record_version, endorsement)) {
                                        ep.total_ops++;
                                        endorsement->set_aborted(false);
                                    } else {
                                        endorsement->set_aborted(true);
                                    }
                                }
                            } else {
                                /* execute */
                                TransactionProposal proposal;
                                if (!proposal.ParseFromString(request_queue.front()) ||
                                    !proposal.GetReflection()->GetUnknownFields(proposal).empty()) {
                                    LOG(WARNING) << "block formation thread: error in deserialising transaction proposal.";
                                    block.mutable_transactions()->RemoveLast();
                                } else {
                                    *(endorsement->mutable_received_ts()) = proposal.received_ts();
                                    if (proposal.type() == TransactionProposal::Type::TransactionProposal_Type_Get) {
                                        ycsb_get(proposal.keys(), endorsement);
                                    } else if (proposal.type() == TransactionProposal::Type::TransactionProposal_Type_Put) {
                                        ycsb_put(proposal.keys(), proposal.values(), record_version, true, endorsement);
                                    } else {
                                        smallbank(proposal.keys(), proposal.type(), proposal.execution_delay(), true, record_version, endorsement);
                                    }
                                    ep.total_ops++;
                                    endorsement->set_aborted(false);
                                }
                            }
                            trans_index_++;
                            request_queue.pop();
                        }
                    }

                    /* write the block to stable storage */
                    block.set_block_id(block_index);
                    block.set_prev_block_hash(prev_block_hash);  // write to disk and hash the block
                    serialized_block.clear();
                    if (!block.SerializeToString(&serialized_block)) {
                        LOG(ERROR) << "block formation thread: failed to serialize block.";
                    }
                    uint32_t size = serialized_block.size();
                    block_store.write((char *)&size, sizeof(uint32_t));
                    block_store.write(serialized_block.c_str(), size);
                    block_store.flush();
                    prev_block_hash = sha256(block.SerializeAsString());

                    block_index++;
                    trans_index = 0;

                    block.clear_block_id();
                    block.clear_transactions();
                }
            }
        }
    }

    return nullptr;
}

void *simulation_handler(void *arg) {
    struct ExecThreadContext ctx = *(struct ExecThreadContext *)arg;
    int thread_index = ctx.thread_index;

    unique_ptr<PeerComm::Stub> stub;
    if (!is_leader) {
        stub = PeerComm::NewStub(leader_channel);
    }

    while (true) {
        TransactionProposal proposal = execution_queue.pop();
        if (arch.is_xov) {
            Request req;
            Endorsement *endorsement = req.mutable_endorsement();
            assert(proposal.has_received_ts());
            *(endorsement->mutable_received_ts()) = proposal.received_ts();

            if (proposal.type() == TransactionProposal::Type::TransactionProposal_Type_Get) {
                ycsb_get(proposal.keys(), endorsement);
            } else if (proposal.type() == TransactionProposal::Type::TransactionProposal_Type_Put) {
                ycsb_put(proposal.keys(), proposal.values(), RecordVersion(), false, endorsement);
            } else {
                smallbank(proposal.keys(), proposal.type(), proposal.execution_delay(), false, RecordVersion(), endorsement);
            }

            if (is_leader) {
                ordering_queue.add(endorsement->SerializeAsString());
            } else {
                ClientContext context;
                google::protobuf::Empty rsp;
                Status status = stub->send_to_peer(&context, req, &rsp);
                if (!status.ok()) {
                    LOG(ERROR) << "grpc failed in simulation handler.";
                }
            }
        } else if (arch.reorder) {
            uint64_t proposal_id = proposal.id();
            Endorsement endorsement;
            struct RecordVersion record_version = {
                .version_blockid = block_index,
                .version_transid = proposal_id,
            };

            *(endorsement.mutable_received_ts()) = proposal.received_ts();
            if (proposal.type() == TransactionProposal::Type::TransactionProposal_Type_Get) {
                ycsb_get(proposal.keys(), &endorsement);
            } else if (proposal.type() == TransactionProposal::Type::TransactionProposal_Type_Put) {
                ycsb_put(proposal.keys(), proposal.values(), record_version, true, &endorsement);
            } else {
                smallbank(proposal.keys(), proposal.type(), proposal.execution_delay(), true, record_version, &endorsement);
            }
            ep.total_ops++;
            endorsement.set_aborted(false);

            oxii_helper.C_add(proposal_id, endorsement);
        }
    }

    return nullptr;
}

void run_peer(const string &server_address) {
    /* start the gRPC server to accept requests */
    PeerCommImpl service(peer_config["sysconfig"]["log_dir"].GetString());
    ServerBuilder builder;
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);
    std::unique_ptr<Server> server(builder.BuildAndStart());
    LOG(INFO) << "RPC server listening on " << server_address << ".";

    /* spawn the raft threads on leader, block formation thread, and execution threads */
    ep.B_n = peer_config["sysconfig"]["trans_water_mark"].GetInt() / arch.max_block_size;
    ep.T_n = ep.B_n.load() * arch.max_block_size;

    unique_ptr<PeerComm::Stub> stub;
    if (is_leader) {
        spawn_raft_threads(peer_config["sysconfig"]["followers"], peer_config["arch"]["blocksize"].GetInt());
    } else {
        string leader_addr = peer_config["sysconfig"]["leader"].GetString();
        leader_channel = grpc::CreateChannel(leader_addr, grpc::InsecureChannelCredentials());
        stub = PeerComm::NewStub(leader_channel);
    }

    pthread_t block_form_tid;
    pthread_create(&block_form_tid, NULL, block_formation_thread, NULL);
    pthread_detach(block_form_tid);

    int num_exec_threads = peer_config["sysconfig"]["num_execution_threads"].GetInt();
    pthread_t exec_tids[num_exec_threads];
    struct ExecThreadContext *ctxs = (struct ExecThreadContext *)calloc(num_exec_threads, sizeof(struct ExecThreadContext));
    for (int i = 0; i < num_exec_threads; i++) {
        ctxs[i].thread_index = i;
        pthread_create(&exec_tids[i], NULL, simulation_handler, &ctxs[i]);

        /* stick thread to a core for better performance */
        int num_cores = sysconf(_SC_NPROCESSORS_ONLN);
        int core_id = i % num_cores;
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(core_id, &cpuset);
        int ret = pthread_setaffinity_np(exec_tids[i], sizeof(cpu_set_t), &cpuset);
        if (ret) {
            LOG(ERROR) << "pthread_setaffinity_np failed with '" << strerror(ret) << "'.";
        }
        pthread_detach(exec_tids[i]);
    }

    /* setup the grpc client for learning agent */
    unique_ptr<AgentComm::Stub> agent_stub;
    agent_stub = AgentComm::NewStub(grpc::CreateChannel(peer_config["sysconfig"]["agent"].GetString(),
                                                        grpc::InsecureChannelCredentials()));

    /* process transaction proposals from queue */
    bool is_cleaned = false;
    while (true) {
        if (block_index < ep.B_n) {
            TransactionProposal proposal = proposal_queue.pop();
            if (arch.is_xov) {
                execution_queue.add(proposal);
            } else {
                if (is_leader) {
                    ordering_queue.add(proposal.SerializeAsString());
                } else {
                    ClientContext context;
                    Request req;
                    google::protobuf::Empty rsp;
                    TransactionProposal *proposal_ = req.mutable_proposal();
                    *proposal_ = proposal;
                    Status status = stub->send_to_peer(&context, req, &rsp);
                    if (!status.ok()) {
                        LOG(ERROR) << "grpc failed in run_peer.";
                    }
                }
            }
            if (is_cleaned) {
                is_cleaned = false;
            }
        } else {
            if (!is_cleaned) {
                ep.end = chrono::duration_cast<chrono::milliseconds>(chrono::system_clock::now().time_since_epoch());
                uint64_t time = (ep.end - ep.start).count();
                double throughput = ((double)ep.total_ops.load() / time) * 1000;

                LOG(INFO) << "Episode " << ep.episode << " ends: duration = " << time / 1000.0 << "s, throughput = " << throughput << "tps, "
                          << "trans_count(T_n) = " << transaction_count << ", last_log_index = " << last_log_index << ".";

                ep.freeze = true;
                proposal_queue.clear();
                ordering_queue.clear();
                execution_queue.clear();

                ClientContext context;  // notify learning agent the end of current episode
                Reward reward;
                google::protobuf::Empty rsp;
                reward.set_is_leader(is_leader);
                reward.set_throughput(throughput);
                Status status = agent_stub->end_current_episode(&context, reward, &rsp);
                if (!status.ok()) {
                    LOG(ERROR) << "grpc failed in run_peer.";
                    exit(1);
                }

                is_cleaned = true;
            }
        }
    }
}

int main(int argc, char *argv[]) {
    int opt;
    string configfile = "../../config/peer_config.json";
    string server_address;
    while ((opt = getopt(argc, argv, "hla:c:")) != -1) {
        switch (opt) {
            case 'h':
                fprintf(stderr, "peer node usage:\n");
                fprintf(stderr, "\t-h: print this help message\n");
                fprintf(stderr, "\t-l: only if specified, act as the leader node\n");
                fprintf(stderr, "\t-a <server_ip:server_port>: the listening addr of grpc server\n");
                fprintf(stderr, "\t-c <path_to_config_file>: path to the configuration file\n");
                exit(0);
            case 'l':
                is_leader = true;
                break;
            case 'a':
                server_address = std::string(optarg);
                break;
            case 'c':
                configfile = string(optarg);
                break;
            default:
                fprintf(stderr, "Invalid option -%c\n", opt);
                exit(1);
                break;
        }
    }

    ifstream ifs(configfile);
    IStreamWrapper isw(ifs);
    peer_config.ParseStream(isw);
    arch.max_block_size = peer_config["arch"]["blocksize"].GetInt();  // number of transactions
    arch.is_xov = peer_config["arch"]["early_execution"].GetBool();
    arch.reorder = peer_config["arch"]["reorder"].GetBool();

    el::Configurations conf("../../config/logger.conf");
    el::Loggers::reconfigureLogger("default", conf);

    std::filesystem::remove_all(peer_config["sysconfig"]["leveldb_dir"].GetString());
    options.create_if_missing = true;
    options.error_if_exists = true;
    leveldb::Status s = leveldb::DB::Open(options, peer_config["sysconfig"]["leveldb_dir"].GetString(), &db);
    assert(s.ok());

    std::filesystem::remove_all(peer_config["sysconfig"]["log_dir"].GetString());
    std::filesystem::create_directory(peer_config["sysconfig"]["log_dir"].GetString());

    run_peer(server_address);

    return 0;
}