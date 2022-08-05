#include "client.h"

INITIALIZE_EASYLOGGINGPP

Document peer_config;
Document client_config;
el::Logger *logger = el::Loggers::getLogger("default");
pthread_barrier_t prep_barrier;
atomic_bool end_flag(false);

void *client_thread(void *arg) {
    struct CliThreadContext ctx = *(struct CliThreadContext *)arg;
    shared_ptr<grpc::Channel> peer_channel = grpc::CreateChannel(ctx.peer_grpc_endpoint, grpc::InsecureChannelCredentials());
    unique_ptr<PeerComm::Stub> stub(PeerComm::NewStub(peer_channel));

    /* prepopulate */
    CompletionQueue cq;
    ClientContext context;
    PrepopulateResponse response;
    unique_ptr<ClientWriter<TransactionProposal>> prep_writer(stub->prepopulate_stream(&context, &response));

    for (int i = 0; i < ctx.num_keys; i++) {
        {
            TransactionProposal proposal;
            string key = "checking_" + to_string(i); // for smallbank workload
            string value = to_string(rand() % BALANCE_HIGH); // guarantee the same value for a particular key across all peers
            proposal.set_keys(0, key);
            proposal.set_values(0, value);
            if (!prep_writer->Write(proposal)) {
                logger->error("prepopulate node %v: broken stream.", ctx.peer_grpc_endpoint);
            }
        }
        {
            TransactionProposal proposal;
            string key = "saving_" + to_string(i);
            string value = to_string(rand() % BALANCE_HIGH);
            proposal.set_keys(0, key);
            proposal.set_values(0, value);
            if (!prep_writer->Write(proposal)) {
                logger->error("prepopulate node %v: broken stream.", ctx.peer_grpc_endpoint);
            }
        }
    }
    prep_writer->WritesDone();
    Status status = prep_writer->Finish();
    if (status.ok()) {
        logger->info("prepopulate node %v: %v keys successfully written.", ctx.peer_grpc_endpoint, response.num_keys());
    } else {
        logger->error("prepopulate node %v: prepopulate_stream rpc failed.", ctx.peer_grpc_endpoint);
    }

    /* start benchmarking */
    random_device rd;
    mt19937 gen(rd());
    bernoulli_distribution is_hot(ctx.hot_key_ratio);
    bernoulli_distribution is_update(ctx.write_ratio);
    uniform_int_distribution<int> trans(0, 4);
    int hot_keys_range = ctx.num_keys * 0.01;
    uniform_int_distribution<int> hot_key(0, hot_keys_range - 1);
    uniform_int_distribution<int> cold_key(hot_keys_range, ctx.num_keys - 1);
    pthread_barrier_wait(&prep_barrier);  // set a barrier here
    {
        ClientContext context;
        google::protobuf::Empty req;
        stub->Asyncstart_benchmarking(&context, req, &cq);
        bool ok;
        void *got_tag;
        cq.Next(&got_tag, &ok);  // notify peer to start benchmarking
    }

    while (!end_flag) {
        usleep(ctx.interval);

        for (int i = 0; (!end_flag) & (i < ctx.trans_per_interval); i++) {
            TransactionProposal proposal;
            int trans_choice = -1; 
            if (is_update(gen)) {
                trans_choice = trans(gen);
                if (trans_choice == 0) {
                    proposal.set_type(TransactionProposal::Type::TransactionProposal_Type_TransactSavings);
                } else if (trans_choice == 1) {
                    proposal.set_type(TransactionProposal::Type::TransactionProposal_Type_DepositChecking);
                } else if (trans_choice == 2) {
                    proposal.set_type(TransactionProposal::Type::TransactionProposal_Type_SendPayment);
                } else if (trans_choice == 3) {
                    proposal.set_type(TransactionProposal::Type::TransactionProposal_Type_WriteCheck);
                } else if (trans_choice == 4) {
                    proposal.set_type(TransactionProposal::Type::TransactionProposal_Type_Amalgamate);
                }
            }
            else {
                proposal.set_type(TransactionProposal::Type::TransactionProposal_Type_Query);
            }

            if (is_hot(gen)) {
                proposal.set_keys(0, to_string(hot_key(gen)));
                if (trans_choice == 2) {
                    proposal.set_keys(1, to_string(hot_key(gen)));
                }
            } else {
                proposal.set_keys(0, to_string(cold_key(gen)));
                if (trans_choice == 2) {
                    proposal.set_keys(1, to_string(cold_key(gen)));
                }
            }

            ClientContext context;
            Request req;
            req.set_allocated_proposal(&proposal);
            stub->Asyncsend_to_peer(&context, req, &cq);
            bool ok;
            void *got_tag;
            cq.Next(&got_tag, &ok);
        }
    }

    {
        ClientContext context;
        google::protobuf::Empty req;
        stub->Asyncend_benchmarking(&context, req, &cq);
        bool ok;
        void *got_tag;
        cq.Next(&got_tag, &ok);  // notify peer to end benchmarking
    }
    return nullptr;
}

int main(int argc, char *argv[]) {
    string peer_configfile = "config/peer_config.json";
    string client_configfile = "config/client_config.json";

    ifstream ifs(peer_configfile);
    IStreamWrapper isw(ifs);
    peer_config.ParseStream(isw);

    ifstream ifs_(client_configfile);
    IStreamWrapper isw_(ifs_);
    client_config.ParseStream(isw_);

    vector<string> peers;
    peers.push_back(peer_config["sysconfig"]["leader"].GetString());
    const Value &followers = peer_config["sysconfig"]["followers"];
    for (int i = 0; i < followers.Size(); i++) {
        peers.push_back(followers[i].GetString());
    }

    int num_peers = peers.size();
    pthread_barrier_init(&prep_barrier, NULL, num_peers + 1);

    pthread_t client_tids[num_peers];
    struct CliThreadContext *ctxs = (struct CliThreadContext *)calloc(num_peers, sizeof(struct CliThreadContext));
    for (int i = 0; i < num_peers; i++) {
        ctxs[i].peer_grpc_endpoint = peers[i];
        ctxs[i].num_keys = client_config["num_keys"].GetInt();
        ctxs[i].trans_per_interval = client_config["trans_per_interval"].GetInt() / num_peers;
        ctxs[i].interval = client_config["interval"].GetInt();
        ctxs[i].write_ratio = client_config["write_ratio"].GetDouble();
        ctxs[i].hot_key_ratio = client_config["hot_key_ratio"].GetDouble();
        pthread_create(&client_tids[i], NULL, client_thread, &ctxs[i]);
        /* stick thread to a core for better performance */
        int num_cores = sysconf(_SC_NPROCESSORS_ONLN);
        int core_id = i % num_cores;
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(core_id, &cpuset);
        int ret = pthread_setaffinity_np(client_tids[i], sizeof(cpu_set_t), &cpuset);
        if (ret) {
            logger->error("pthread_setaffinity_np failed with '%v'.", strerror(ret));
        }
    }

    /* set a barrier here and then wait for benchmarking completion */
    pthread_barrier_wait(&prep_barrier);
    sleep(15);
    end_flag = true;
    for (int i = 0; i < num_peers; i++) {
        void *status;
        pthread_join(client_tids[i], &status);
    }

    pthread_barrier_destroy(&prep_barrier);

    return 0;
}