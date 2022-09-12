#ifndef COMMON_H
#define COMMON_H

#include <pthread.h>

#include <atomic>
#include <deque>

#include "blockchain.grpc.pb.h"
#include "leveldb/db.h"

using namespace std;

extern Document peer_config;
extern leveldb::DB *db;
extern leveldb::Options options;
extern deque<atomic<unsigned long>> match_index;
extern atomic<unsigned long> commit_index;

struct Architecture {
    size_t max_block_size;
    bool is_xov;
    bool reorder;
};

extern Architecture arch;

class Episode {
   private:
   public:
    Episode() : total_ops(0), freeze(false), episode(0) {
    }

    atomic<long> total_ops;
    atomic_bool freeze;
    atomic<uint64_t> B_n;
    atomic<uint64_t> T_n;
    atomic<uint64_t> episode;
    chrono::milliseconds start;
    chrono::milliseconds end;
};  // the class used for synchronization across episodes

extern Episode ep;

template <typename T>
class Queue {
   private:
    deque<T> queue_;
    pthread_mutex_t mutex;
    pthread_cond_t more;

   public:
    size_t max_queue_size;
    Queue() : max_queue_size(20000) {
        pthread_mutex_init(&mutex, NULL);
        pthread_cond_init(&more, NULL);
    }

    ~Queue() {
        pthread_mutex_destroy(&mutex);
        pthread_cond_destroy(&more);
    }

    void add(const T &request) {
        pthread_mutex_lock(&mutex);
        if (queue_.size() <= max_queue_size && !ep.freeze) {
            queue_.push_back(request);
            pthread_cond_signal(&more);
        }
        pthread_mutex_unlock(&mutex);
    }

    T pop() {
        pthread_mutex_lock(&mutex);
        while (queue_.empty()) {
            pthread_cond_wait(&more, &mutex);
        }
        T request = queue_.front();
        queue_.pop_front();
        pthread_mutex_unlock(&mutex);

        return request;
    }

    void clear() {
        pthread_mutex_lock(&mutex);
        queue_.clear();
        pthread_mutex_unlock(&mutex);
    }
};

extern Queue<TransactionProposal> proposal_queue;
extern Queue<string> ordering_queue;
extern Queue<TransactionProposal> execution_queue;

#endif