#ifndef PEER_H
#define PEER_H

#include <grpc/grpc.h>
#include <grpcpp/channel.h>
#include <grpcpp/client_context.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/security/credentials.h>
#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <grpcpp/server_context.h>
#include <openssl/sha.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <list>
#include <memory>
#include <queue>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "blockchain.grpc.pb.h"
#include "consensus.h"
#include "rapidjson/document.h"
#include "rapidjson/istreamwrapper.h"

using namespace rapidjson;
using namespace std;
using grpc::Channel;
using grpc::ClientAsyncWriter;
using grpc::ClientContext;
using grpc::CompletionQueue;
using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::ServerReader;
using grpc::Status;

struct ExecThreadContext {
    int thread_index;
};

class OXIIHelper {
   private:
    set<uint64_t> C;
    pthread_mutex_t mutex;

   public:
    set<uint64_t> W;
    vector<Endorsement> endorsements;

    OXIIHelper() {
        pthread_mutex_init(&mutex, NULL);
    }

    ~OXIIHelper() {
        pthread_mutex_destroy(&mutex);
    }

    void C_add(uint64_t trans_id, Endorsement& endorsement) {
        pthread_mutex_lock(&mutex);
        C.insert(trans_id);
        endorsements[trans_id] = endorsement;
        pthread_mutex_unlock(&mutex);
    }

    void C_clear() {
        pthread_mutex_lock(&mutex);
        C.clear();
        pthread_mutex_unlock(&mutex);
    }

    bool C_find(uint64_t trans_id) {
        bool find = false;
        pthread_mutex_lock(&mutex);
        if (C.find(trans_id) != C.end()) {
            find = true;
        }
        pthread_mutex_unlock(&mutex);

        return find;
    }

    size_t C_size() {
        size_t size;
        pthread_mutex_lock(&mutex);
        size = C.size();
        pthread_mutex_unlock(&mutex);

        return size;
    }
};

#endif