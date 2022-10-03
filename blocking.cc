#include "peer.h"

#include "graph.h"
#include "leveldb/db.h"
#include "smart_contracts.h"

INITIALIZE_EASYLOGGINGPP

Document peer_config;
leveldb::DB *db;
leveldb::Options options;
Queue<TransactionProposal> proposal_queue;
Queue<string> ordering_queue;
Queue<TransactionProposal> execution_queue;
shared_ptr<grpc::Channel> leader_channel;
bool is_leader = false;
atomic<long> total_ops = 0;
atomic<long> readn = 0;
atomic<long> writen = 0;
atomic<long long> last_block_id = 0;
extern deque<atomic<unsigned long>> match_index;
extern atomic<unsigned long> commit_index;

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
    uint64_t blockid = 0;
    
    //for every transaction we check whether the version-number of the read value still matches
    //the one in the current state.
    for (int read_id = 0; read_id < transaction->read_set_size(); read_id++) {
        struct RecordVersion r_record_version;
        kv_get(transaction->read_set(read_id).read_key(), nullptr, &r_record_version, blockid);

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
    //LOG(INFO) << "Block formation thread is running.";

    ifstream log("./log/raft.log", ios::in);
    assert(log.is_open());
    ofstream block_store("./log/blockchain.log", ios::out | ios::binary);
    assert(block_store.is_open());

    unsigned long last_applied = 0;
    int majority = (peer_config["sysconfig"]["followers"].Size() / 2) + 1;
    uint64_t block_index = 0;
    uint64_t trans_index = 0;
    size_t max_block_size = peer_config["arch"]["blocksize"].GetInt();  // number of transactions
    bool is_xov = peer_config["arch"]["early_execution"].GetBool();
    bool reorder = peer_config["arch"]["reorder"].GetBool();
    size_t block_pipe_num = peer_config["arch"]["block_pipe_num"].GetInt();  // number of blocks reordered together before the block cutting 

    Block block;
    string prev_block_hash = sha256(block.SerializeAsString());
    string serialized_block;
    queue<string> request_queue;

    while (true) {
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

            //LOG(DEBUG) << "[block_id = " << block_index << ", trans_id = " << trans_index << "]: added transaction to block.";
            request_queue.push(serialized_request);
            trans_index++;

            /* cut the block */
            //Block Pipelining - Wait for block B2 before reordering

            //Waiting happens by waiting till request queue has transactions from 2 blocks
            /* TODO: change 2 to a parameter in config file*/ 
            
            //block_pipe_num
            if (trans_index >= max_block_size*block_pipe_num) {
            //if (trans_index >= max_block_size*2) {
                //arch - xov, reorder 
                if (reorder) {
                    if (is_xov) {
                        xov_reorder(request_queue, block);
                        //iterating over all the transactions in the request_queue with B1, B2      
                        for (uint64_t i = 0; i < block.transactions_size(); i++) {
                            //Only recording the 1st block with Block size = max_block_size
                            //Hence cutting B1 out
                            if(i<max_block_size) {
                                struct RecordVersion record_version = {
                                    .version_blockid = block_index,
                                    .version_transid = i,
                                    };
                                if ((!block.mutable_transactions(i)->aborted()) && validate_transaction(record_version, block.mutable_transactions(i)))
                                {
                                    total_ops++;
                                    //counts the reads and writes in every transaction(i) in each block
                                    if(( block.mutable_transactions(i)->write_set_size()) != 0) 
                                    {
                                        //transaction is a write transaction
                                        writen++;
                                    }
                                    else
                                    {
                                        //transaction is a read only transaction
                                        readn++;
                                    }
                                    block.mutable_transactions(i)->set_aborted(false);
                                }
                                else
                                {
                                    block.mutable_transactions(i)->set_aborted(true);
                                }
                            //push the remaining transactions back into request_queue 
                            } else {
                                request_queue.push(block.transactions(i).SerializeAsString());
                            }
                        }

                        //Clearing the second half content of block 
                        //Using block.mutable_transactions()->DeleteSubrange(i, j)
                        /*
                        void RepeatedPtrField::DeleteSubrange(
                            int start,
                            int num)

                            Delete elements with indices in the range [[]start .
                                     . start+num-1]. 
                        Caution: implementation moves all elements with indices [[]start+num .. ]. Calling this routine inside a loop can cause quadratic behavior.
                        */

                        //start:max_block_size , j: request_queue.size()
                        if(block_pipe_num>1){block.mutable_transactions()->DeleteSubrange(max_block_size, request_queue.size());}
                            
                    } else {
                    }
                } 
                else {
                    uint64_t trans_index_ = 0;
                    while (request_queue.size()) {
                        Endorsement *endorsement = block.add_transactions();
                        struct RecordVersion record_version = {
                            .version_blockid = block_index,
                            .version_transid = trans_index_,
                        };
                        //arch - xov, no reorder
                        if (is_xov) {
                            /* validate */
                             if (!endorsement->ParseFromString(request_queue.front()) ||
                                    !endorsement->GetReflection()->GetUnknownFields(*endorsement).empty()) {
                                    LOG(WARNING) << "block formation thread: error in deserialising endorsement.";
                                    block.mutable_transactions()->RemoveLast();
                                    } else {
                                        if ((!endorsement->aborted()) && (validate_transaction(record_version, endorsement))) {
                                            total_ops++;
                                            if(( endorsement->write_set_size()) != 0) 
                                            {
                                                //transaction is a write transaction
                                                writen++;
                                            }
                                            else
                                            {
                                                //transaction is a read only transaction
                                                readn++;
                                            }
                                            endorsement->set_aborted(false);
                                        } else {
                                            endorsement->set_aborted(true);
                                        }
                                    } 

                        } 
                        //arch - ox, no reorder
                        else {
                            /* execute */
                            TransactionProposal proposal;
                            if (!proposal.ParseFromString(request_queue.front())) {
                                LOG(ERROR) << "block formation thread: error in deserialising transaction proposal.";
                            }

                            if (proposal.type() == TransactionProposal::Type::TransactionProposal_Type_Get) {
                                ycsb_get(proposal.keys(), endorsement);
                            } else if (proposal.type() == TransactionProposal::Type::TransactionProposal_Type_Put) {
                                ycsb_put(proposal.keys(), proposal.values(), record_version, true, endorsement);
                            } else {
                                smallbank(proposal.keys(), proposal.type(), proposal.execution_delay(), true, record_version, endorsement);
                            }
                            total_ops++;
                            endorsement->set_aborted(false);
                              //counts the reads and writes in every transaction(i) in each block
                                    if(( endorsement->write_set_size()) != 0) 
                                    {
                                        //transaction is a write transaction
                                        writen++;
                                    }
                                    else
                                    {
                                        //transaction is a read only transaction
                                        readn++;
                                    }
                        }
                        trans_index_++;
                        request_queue.pop();
                    }
                }

                /* write the block to stable storage */
                //print the block id version number 
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
                last_block_id = block_index;

                block_index++;
                //tran_index will start from request_queue.size() since the request_queue is filled
                //till there with transactions from Block B2
                trans_index = request_queue.size();
                
                //LOG(DEBUG) << "[block_id = " << block_index << ", trans_id = " << trans_index << "]: added transaction to block.";
                block.clear_block_id();
                block.clear_transactions();
            }
        }
    }

    return nullptr;
}

//exploit the available version-numbers to implement a lock-free concurrency control mechanism 
//protecting the current state. To do so, in Fabric++
//1) we first remove the read-write lock, that was  sequentializing simulation and validation phase - already been done in our program where pthreads run concurrently in run_peer
//2) we have to inspect the version-number of every read value and test whether it is still up-to-date
//3) if a read value is not up-to-date, we have to abort the transaction and inform client to send the proposal again


//simulation_handler executes transactions from transaction proposals 

//At the start of the simulation phase, we first identify the block-ID of the last block that made it into the ledger
//This is stored as a global variable last_block_id and changes(atomically) everytime a new block is added to the ledger
void *simulation_handler(void *arg) {

    bool early_abort = peer_config["arch"]["early_abort"].GetBool();
    LOG(DEBUG) << "simulation handler thread started.";
    struct ExecThreadContext ctx = *(struct ExecThreadContext *)arg;
    int thread_index = ctx.thread_index;

    unique_ptr<PeerComm::Stub> stub;
    if (!is_leader) {
        stub = PeerComm::NewStub(leader_channel);
    }

    while (true) {
        TransactionProposal proposal = execution_queue.pop();
        Request req;
        Endorsement *endorsement = req.mutable_endorsement();
        bool checking_condition;
        LOG(DEBUG) << "simulation handler thread: received transaction proposal.";
        //print last_block_id
        if (proposal.type() == TransactionProposal::Type::TransactionProposal_Type_Get) {
            //apply condition if ycsb_get returns false
            checking_condition = ycsb_get(proposal.keys(), endorsement, last_block_id);
            if (!checking_condition && early_abort) {
                endorsement->set_aborted(true);
            } else {
                endorsement->set_aborted(false);
            }
        } else if (proposal.type() == TransactionProposal::Type::TransactionProposal_Type_Put) {
            ycsb_put(proposal.keys(), proposal.values(), RecordVersion(), false, endorsement);
            endorsement->set_aborted(false);

        } else {
            checking_condition =  smallbank(proposal.keys(), proposal.type(), proposal.execution_delay(), false, RecordVersion(), endorsement, last_block_id);
            if(!checking_condition && early_abort) {
                LOG(INFO) << "aborted in simulation handler";
                endorsement->set_aborted(true);
            } else {
                endorsement->set_aborted(false);
            }
        }
        if (is_leader) {
            //ask chenyuan
            ordering_queue.add(endorsement->SerializeAsString());
        } else {
            ClientContext context;
            google::protobuf::Empty rsp;
            Status status = stub->send_to_peer(&context, req, &rsp);
            if (!status.ok()) {
                LOG(ERROR) << "grpc failed in simulation handler.";
            }
        }
    }
    return nullptr;
}

void run_peer(const string &server_address) {
    /* start the gRPC server to accept requests */
    PeerCommImpl service;
    ServerBuilder builder;
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);
    std::unique_ptr<Server> server(builder.BuildAndStart());
    //LOG(INFO) << "RPC server listening on " << server_address << ".";

    /* spawn the raft threads on leader, block formation thread, and execution threads */
    unique_ptr<PeerComm::Stub> stub;
    if (is_leader) {
        spawn_raft_threads(peer_config["sysconfig"]["followers"], peer_config["arch"]["blocksize"].GetInt());
    } else {
        string leader_addr = peer_config["sysconfig"]["leader"].GetString();
        leader_channel = grpc::CreateChannel(leader_addr, grpc::InsecureChannelCredentials());
        stub = PeerComm::NewStub(leader_channel);
    }

    pthread_t block_form_tid;
    //this - block_formation_thread
    pthread_create(&block_form_tid, NULL, block_formation_thread, NULL);
    pthread_detach(block_form_tid);

    int num_exec_threads = peer_config["sysconfig"]["num_execution_threads"].GetInt();
    pthread_t exec_tids[num_exec_threads];
    struct ExecThreadContext *ctxs = (struct ExecThreadContext *)calloc(num_exec_threads, sizeof(struct ExecThreadContext));
    for (int i = 0; i < num_exec_threads; i++) {
        ctxs[i].thread_index = i;
        //this - simulation_handler
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

    /* process transaction proposals from queue */
    bool is_xov = peer_config["arch"]["early_execution"].GetBool();
    while (true) {
        TransactionProposal proposal = proposal_queue.pop();
        if (is_xov) {
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
    }
}

int main(int argc, char *argv[]) {
    int opt;
    string configfile = "config/peer_config.json";
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

    el::Configurations conf("./config/logger.conf");
    el::Loggers::reconfigureLogger("default", conf);

    std::filesystem::remove_all("./testdb");
    options.create_if_missing = true;
    options.error_if_exists = true;
    leveldb::Status s = leveldb::DB::Open(options, "./testdb", &db);
    assert(s.ok());

    std::filesystem::remove_all("./log");
    std::filesystem::create_directory("./log");

    run_peer(server_address);

    return 0;
}
