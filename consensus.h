#ifndef CONSENSUS_H
#define CONSENSUS_H

#include <grpc/grpc.h>
#include <grpcpp/channel.h>
#include <grpcpp/client_context.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/security/credentials.h>
#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <grpcpp/server_context.h>
#include <pthread.h>
#include <semaphore.h>

#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <queue>
#include <random>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

#include "blockchain.grpc.pb.h"
#include "easylogging++.h"
#include "rapidjson/document.h"
#include "smart_contracts.h"

#define LOG_ENTRY_BATCH 1

using namespace std;
using namespace rapidjson;
using grpc::Channel;
using grpc::ClientAsyncResponseReader;
using grpc::ClientAsyncWriter;
using grpc::ClientContext;
using grpc::CompletionQueue;
using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::ServerReader;
using grpc::Status;

struct RaftThreadContext {
    string grpc_endpoint;
    int server_index;
    int majority;
};

template <typename T>
class Queue {
   private:
    queue<T> queue_;
    pthread_mutex_t mutex;
    sem_t full;

   public:
    Queue() {
        pthread_mutex_init(&mutex, NULL);
        sem_init(&full, 0, 0);
    }

    ~Queue() {
        pthread_mutex_destroy(&mutex);
        sem_destroy(&full);
    }

    void add(const T &request) {
        pthread_mutex_lock(&mutex);
        queue_.push(request);
        pthread_mutex_unlock(&mutex);
        sem_post(&full);
    }

    T pop() {
        sem_wait(&full);
        pthread_mutex_lock(&mutex);
        T request = queue_.front();
        queue_.pop();
        pthread_mutex_unlock(&mutex);

        return request;
    }
};

void *log_replication_thread(void *arg);
void run_rpc_server(const string &server_address);
void spawn_raft_threads(const Value &followers);

#endif