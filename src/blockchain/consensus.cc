#include "consensus.h"

#include "common.h"

atomic<unsigned long> commit_index(0);
atomic<unsigned long> last_log_index(0);
deque<atomic<unsigned long>> next_index;
deque<atomic<unsigned long>> match_index;

void *log_replication_thread(void *arg) {
    struct RaftThreadContext ctx = *(struct RaftThreadContext *)arg;
    LOG(INFO) << "[server_index = " << ctx.server_index << "]log replication thread is running for follower " << ctx.grpc_endpoint << ".";
    shared_ptr<grpc::Channel> channel = grpc::CreateChannel(ctx.grpc_endpoint, grpc::InsecureChannelCredentials());
    unique_ptr<PeerComm::Stub> stub(PeerComm::NewStub(channel));

    ifstream log(string(peer_config["sysconfig"]["log_dir"].GetString()) + "/raft.log", ios::in);
    assert(log.is_open());

    while (true) {
        if (last_log_index >= next_index[ctx.server_index]) {
            /* send AppendEntries RPC */
            ClientContext context;
            AppendRequest app_req;
            AppendResponse app_rsp;

            app_req.set_leader_commit(commit_index);
            int index = 0;
            for (; index < arch.max_block_size && next_index[ctx.server_index] + index <= last_log_index; index++) {
                uint32_t size;
                log.read((char *)&size, sizeof(uint32_t));
                char *entry_ptr = (char *)malloc(size);
                log.read(entry_ptr, size);
                app_req.add_log_entries(entry_ptr, size);
                free(entry_ptr);
            }

            Status status = stub->append_entries(&context, app_req, &app_rsp);
            if (!status.ok()) {
                LOG(ERROR) << "[server_index = " << ctx.server_index << "]gRPC failed with error message: " << status.error_message() << ".";
                continue;
            } else {
                LOG(DEBUG) << "[server_index = " << ctx.server_index << "]send append_entries RPC. last_log_index = "
                           << last_log_index.load() << ". next_index = " << next_index[ctx.server_index].load()
                           << ". commit_index = " << commit_index.load() << ".";
            }

            next_index[ctx.server_index] += index;
            match_index[ctx.server_index] = next_index[ctx.server_index] - 1;
            // LOG(DEBUG) << "[server_index = " << ctx.server_index << "]match_index is " << match_index[ctx.server_index].load() << ".";
        } else if (last_log_index) {
            usleep(1000);
            ClientContext context;
            AppendRequest app_req;
            AppendResponse app_rsp;

            app_req.set_leader_commit(commit_index);

            Status status = stub->append_entries(&context, app_req, &app_rsp);  // heartbeat
        }
    }
}

void *leader_main_thread(void *arg) {
    struct RaftThreadContext ctx = *(struct RaftThreadContext *)arg;
    ofstream log(string(peer_config["sysconfig"]["log_dir"].GetString()) + "/raft.log", ios::out | ios::binary);
    string tagged_entry_str;
    while (true) {
        if (last_log_index < ep.T_h && !ep.consensus_paused) {
            int i = 0;
            for (; i < arch.max_block_size; i++) {
                TaggedEntry tagged_entry;
                string req = ordering_queue.pop();
                tagged_entry.set_entry(req);
                tagged_entry.set_tag(ep.episode);
                tagged_entry.SerializeToString(&tagged_entry_str);
                uint32_t size = tagged_entry_str.size();
                log.write((char *)&size, sizeof(uint32_t));
                log.write(tagged_entry_str.c_str(), size);
            }
            log.flush();
            last_log_index += i;
            if (ep.timeout) {
                ep.consensus_paused = true;
            }
        }
    }
}

/* implementation of AppendEntriesRPC */
Status PeerCommImpl::append_entries(ServerContext *context, const AppendRequest *request, AppendResponse *response) {
    int i = 0;
    for (; i < request->log_entries_size(); i++) {
        uint32_t size = request->log_entries(i).size();
        log.write((char *)&size, sizeof(uint32_t));
        log.write(request->log_entries(i).c_str(), size);
        last_log_index++;
    }
    if (i != 0) {
        log.flush();
    }

    uint64_t leader_commit = request->leader_commit();
    if (leader_commit > commit_index) {
        if (leader_commit > last_log_index) {
            commit_index = last_log_index.load();
        } else {
            commit_index = leader_commit;
        }
    }

    LOG(DEBUG) << "AppendEntriesRPC finished: last_log_index = " << last_log_index.load() << ", commit_index = " << commit_index.load() << ".";

    return Status::OK;
}

Status PeerCommImpl::send_to_peer(ServerContext *context, const Request *request, google::protobuf::Empty *response) {
    if (request->has_endorsement()) {
        ordering_queue.add(request->endorsement().SerializeAsString());
    } else if (request->has_proposal()) {
        if (!request->proposal().has_received_ts()) {
            TransactionProposal proposal = request->proposal();
            set_timestamp(proposal.mutable_received_ts());
            proposal_queue.add(proposal);
        } else {
            proposal_queue.add(request->proposal());
        }
    }

    return Status::OK;
}

Status PeerCommImpl::send_to_peer_stream(ServerContext *context, ServerReader<Request> *reader, google::protobuf::Empty *response) {
    Request request;

    while (reader->Read(&request)) {
        if (request.has_endorsement()) {
            ordering_queue.add(request.endorsement().SerializeAsString());
        } else if (request.has_proposal()) {
            if (!request.proposal().has_received_ts()) {
                TransactionProposal proposal = request.proposal();
                set_timestamp(proposal.mutable_received_ts());
                proposal_queue.add(proposal);
            } else {
                proposal_queue.add(request.proposal());
            }
        }
    }

    return Status::OK;
}

Status PeerCommImpl::prepopulate(ServerContext *context, const TransactionProposal *proposal, PrepopulateResponse *response) {
    LOG(DEBUG) << "prepopulate key " << proposal->keys(0) << ".";
    struct RecordVersion record_version = {
        .version_blockid = 0,
        .version_transid = 0,
    };

    kv_put(proposal->keys(0), proposal->values(0), record_version, true, nullptr);

    return Status::OK;
}

Status PeerCommImpl::start_benchmarking(ServerContext *context, const google::protobuf::Empty *request, google::protobuf::Empty *response) {
    LOG(INFO) << "starts benchmarking.";
    start = chrono::duration_cast<chrono::milliseconds>(chrono::system_clock::now().time_since_epoch());
    ep.start = chrono::duration_cast<chrono::milliseconds>(chrono::system_clock::now().time_since_epoch());

    return Status::OK;
}

Status PeerCommImpl::end_benchmarking(ServerContext *context, const google::protobuf::Empty *request, google::protobuf::Empty *response) {
    end = chrono::duration_cast<chrono::milliseconds>(chrono::system_clock::now().time_since_epoch());
    uint64_t time = (end - start).count();
    double throughput = ((double)ep.total_ops.load() / time) * 1000;
    LOG(INFO) << "throughput = " << throughput << "tps.";

    return Status::OK;
}

Status PeerCommImpl::new_episode_info(ServerContext *context, const Action *action, google::protobuf::Empty *response) {
    ep.next_action = (*action);

    return Status::OK;
}

Status PeerCommImpl::timeout(ServerContext *context, const google::protobuf::Empty *request, google::protobuf::Empty *response) {
    LOG(INFO) << "Episode " << ep.episode << " timeout: initiated by the leader.";
    ep.timeout = true;

    return Status::OK;
}

Status PeerCommImpl::exchange_block_index(ServerContext *context, const PeerExchange *exchange, google::protobuf::Empty *response) {
    LOG(INFO) << "Episode " << ep.episode << ": received last_block_index = " << exchange->block_index() << ".";
    ep.add_last_block_index(exchange->block_index());

    return Status::OK;
}

Status PeerCommImpl::resume_block_formation(ServerContext *context, const PeerExchange *exchange, google::protobuf::Empty *response) {
    if (exchange->no_progress() || exchange->block_index() == block_index) {
        queue<string>().swap(ep.pending_request_queue);
    }

    ep.timeout = false;
    ep.B_h = exchange->block_index();
    ep.block_formation_paused = false;

    return Status::OK;
}

Status PeerCommImpl::reached_new_watermark(ServerContext *context, const PeerExchange *request, PeerExchange *response) {
    ep.num_reached_new_watermark++;
    response->set_raft_index(last_log_index);

    return Status::OK;
}

void spawn_raft_threads(const Value &followers, int batch_size) {
    /* spawn the leader main thread */
    pthread_t leader_main_tid;
    struct RaftThreadContext *ctx = (struct RaftThreadContext *)calloc(1, sizeof(struct RaftThreadContext));
    ctx[0].log_entry_batch = batch_size;
    pthread_create(&leader_main_tid, NULL, leader_main_thread, &ctx[0]);
    pthread_detach(leader_main_tid);

    /* spawn log replication threads */
    assert(followers.IsArray());
    int num_followers = followers.Size();
    pthread_t *repl_tids;
    repl_tids = (pthread_t *)malloc(sizeof(pthread_t) * num_followers);
    struct RaftThreadContext *ctxs = (struct RaftThreadContext *)calloc(num_followers, sizeof(struct RaftThreadContext));
    for (int i = 0; i < num_followers; i++) {
        next_index.emplace_back(1);
        match_index.emplace_back(0);
        ctxs[i].grpc_endpoint = followers[i].GetString();
        ctxs[i].server_index = i;
        ctxs[i].log_entry_batch = batch_size;
        pthread_create(&repl_tids[i], NULL, log_replication_thread, &ctxs[i]);
        pthread_detach(repl_tids[i]);
    }
}

void set_timestamp(google::protobuf::Timestamp *ts) {
    struct timeval tv;
    gettimeofday(&tv, NULL);

    ts->set_seconds(tv.tv_sec);
    ts->set_nanos(tv.tv_usec * 1000);
}
