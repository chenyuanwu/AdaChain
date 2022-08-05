#include "consensus.h"

atomic<unsigned long> commit_index(0);
atomic<unsigned long> last_log_index(0);
deque<atomic<unsigned long>> next_index;
deque<atomic<unsigned long>> match_index;
extern el::Logger *logger;
extern Queue<TransactionProposal> proposal_queue;
extern Queue<string> ordering_queue;
extern Queue<TransactionProposal> execution_queue;
extern atomic<long> total_ops;

void *log_replication_thread(void *arg) {
    struct RaftThreadContext ctx = *(struct RaftThreadContext *)arg;
    logger->info("[server_index = %v]log replication thread is running for follower %v.", ctx.server_index, ctx.grpc_endpoint.c_str());
    shared_ptr<grpc::Channel> channel = grpc::CreateChannel(ctx.grpc_endpoint, grpc::InsecureChannelCredentials());
    unique_ptr<PeerComm::Stub> stub(PeerComm::NewStub(channel));

    ifstream log("./log/raft.log", ios::in);
    assert(log.is_open());

    while (true) {
        if (last_log_index >= next_index[ctx.server_index]) {
            /* send AppendEntries RPC */
            ClientContext context;
            AppendRequest app_req;
            AppendResponse app_rsp;

            app_req.set_leader_commit(commit_index);
            int index = 0;
            for (; index < LOG_ENTRY_BATCH && next_index[ctx.server_index] + index <= last_log_index; index++) {
                uint32_t size;
                log.read((char *)&size, sizeof(uint32_t));
                char *entry_ptr = (char *)malloc(size);
                log.read(entry_ptr, size);
                app_req.add_log_entries(entry_ptr, size);
                free(entry_ptr);
            }

            Status status = stub->append_entries(&context, app_req, &app_rsp);
            if (!status.ok()) {
                logger->error("[server_index = %v]gRPC failed with error message: %v.", ctx.server_index, status.error_message().c_str());
                continue;
            } else {
                logger->debug("[server_index = %v]send append_entries RPC. last_log_index = %v. next_index = %v. commit_index = %v.",
                              ctx.server_index, last_log_index.load(), next_index[ctx.server_index].load(), commit_index.load());
            }

            next_index[ctx.server_index] += index;
            match_index[ctx.server_index] = next_index[ctx.server_index] - 1;
            // logger->debug("[server_index = %v]match_index is %v.", ctx.server_index, match_index[ctx.server_index].load());
        }
    }
}

void *leader_main_thread(void *arg) {
    ofstream log("./log/raft.log", ios::out | ios::binary);
    while (true) {
        int i = 0;
        for (; i < LOG_ENTRY_BATCH; i++) {
            string req = ordering_queue.pop();
            uint32_t size = req.size();
            log.write((char *)&size, sizeof(uint32_t));
            log.write(req.c_str(), size);
        }
        log.flush();
        last_log_index += i;
    }
}

class PeerCommImpl final : public PeerComm::Service {
   public:
    explicit PeerCommImpl() : log("./log/raft.log", ios::out | ios::binary) {}

    /* implementation of AppendEntriesRPC */
    Status append_entries(ServerContext *context, const AppendRequest *request, AppendResponse *response) override {
        int i = 0;
        for (; i < request->log_entries_size(); i++) {
            uint32_t size = request->log_entries(i).size();
            log.write((char *)&size, sizeof(uint32_t));
            log.write(request->log_entries(i).c_str(), size);
            last_log_index++;
        }
        log.flush();

        uint64_t leader_commit = request->leader_commit();
        if (leader_commit > commit_index) {
            if (leader_commit > last_log_index) {
                commit_index = last_log_index.load();
            } else {
                commit_index = leader_commit;
            }
        }

        logger->debug("AppendEntriesRPC finished: last_log_index = %v, commit_index = %v.", last_log_index.load(), commit_index.load());

        return Status::OK;
    }

    Status send_to_peer(ServerContext *context, const Request *request, google::protobuf::Empty *response) override {
        if (request->Message_case() == Request::MessageCase::kEndorsement) {
            ordering_queue.add(request->endorsement().SerializeAsString());
        } else if (request->Message_case() == Request::MessageCase::kProposal) {
            proposal_queue.add(request->proposal());
        }

        return Status::OK;
    }

    Status send_to_peer_stream(ServerContext *context, ServerReader<Request> *reader, google::protobuf::Empty *response) override {
        Request request;

        while (reader->Read(&request)) {
            if (request.Message_case() == Request::MessageCase::kEndorsement) {
                ordering_queue.add(request.endorsement().SerializeAsString());
            } else if (request.Message_case() == Request::MessageCase::kProposal) {
                proposal_queue.add(request.proposal());
            }
        }

        return Status::OK;
    }

    Status prepopulate_stream(ServerContext *context, ServerReader<TransactionProposal> *reader, PrepopulateResponse *response) override {
        TransactionProposal proposal;
        struct RecordVersion record_version = {
            .version_blockid = 0,
            .version_transid = 0,
        };
        int num_keys = 0;

        while (reader->Read(&proposal)) {
            kv_put(proposal.keys(0), proposal.values(0), record_version, true, nullptr);
            num_keys++;
        }
        response->set_num_keys(num_keys);

        return Status::OK;
    }

    Status start_benchmarking(ServerContext *context, const google::protobuf::Empty *request, google::protobuf::Empty *response) override {
        start = chrono::duration_cast<chrono::milliseconds>(chrono::system_clock::now().time_since_epoch());

        return Status::OK;
    }

    Status end_benchmarking(ServerContext *context, const google::protobuf::Empty *request, google::protobuf::Empty *response) override {
        end = chrono::duration_cast<chrono::milliseconds>(chrono::system_clock::now().time_since_epoch());
        uint64_t time = (end - start).count();
        double throughput = ((double)total_ops.load() / time) * 1000;
        logger->info("throughput = %v tps.", throughput);

        return Status::OK;
    }

   private:
    ofstream log;
    chrono::milliseconds start;
    chrono::milliseconds end;
};

void run_rpc_server(const string &server_address) {
    PeerCommImpl service;
    ServerBuilder builder;
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);
    std::unique_ptr<Server> server(builder.BuildAndStart());
    logger->info("RPC server listening on %v", server_address.c_str());
}

void spawn_raft_threads(const Value &followers) {
    /* spawn the leader main thread */
    pthread_t leader_main_tid;
    pthread_create(&leader_main_tid, NULL, leader_main_thread, NULL);
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
        pthread_create(&repl_tids[i], NULL, log_replication_thread, &ctxs[i]);
        pthread_detach(repl_tids[i]);
    }
}
