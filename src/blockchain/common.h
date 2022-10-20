#ifndef COMMON_H
#define COMMON_H

#include <google/protobuf/message.h>
#include <google/protobuf/text_format.h>
#include <google/protobuf/util/message_differencer.h>
#include <google/protobuf/util/time_util.h>
#include <pthread.h>
#include <sys/time.h>

#include <atomic>
#include <deque>
#include <queue>

#include "blockchain.grpc.pb.h"
#include "leveldb/db.h"
#include "rapidjson/document.h"

using namespace std;
using namespace rapidjson;

extern Document peer_config;
extern leveldb::DB *db;
extern leveldb::Options options;
extern atomic<unsigned long> last_log_index;
extern deque<atomic<unsigned long>> match_index;
extern atomic<unsigned long> commit_index;
extern uint64_t block_index;

struct Architecture {
    size_t max_block_size;
    bool is_xov;
    bool reorder;
    bool early_abort;
};

extern Architecture arch;

class Episode {
   private:
    google::protobuf::util::MessageDifferencer differencer;
    pthread_mutex_t mutex;

   public:
    Episode() : total_ops(0), total_ops_entire_run(0), freeze(false), agent_notified(false), timeout(false), block_formation_paused(false), consensus_paused(false), num_reached_new_watermark(0), episode(0) {
        pthread_mutex_init(&mutex, NULL);
    }
    ~Episode() {
        pthread_mutex_destroy(&mutex);
    }

    atomic<long> total_ops;
    atomic<long long> total_ops_entire_run;
    atomic_bool freeze;
    atomic_bool agent_notified;
    atomic_bool timeout;
    atomic_bool block_formation_paused;
    atomic_bool consensus_paused;
    atomic<uint64_t> num_reached_new_watermark;
    atomic<uint64_t> B_h;
    atomic<uint64_t> T_h;
    atomic<uint64_t> B_l;
    atomic<uint64_t> B_start;
    atomic<uint64_t> episode;
    chrono::milliseconds start;
    vector<uint64_t> last_block_indexes;
    queue<string> pending_request_queue;

    Action curr_action;
    Action next_action;

    bool equal_curr_action(const Action &action) {
        return differencer.Equals(action, curr_action);
    }

    void add_last_block_index(uint64_t block_index) {
        pthread_mutex_lock(&mutex);
        last_block_indexes.push_back(block_index);
        pthread_mutex_unlock(&mutex);
    }

    void clear_last_block_indexes() {
        pthread_mutex_lock(&mutex);
        last_block_indexes.clear();
        pthread_mutex_unlock(&mutex);
    }

    size_t num_received_indexes() {
        size_t size;
        pthread_mutex_lock(&mutex);
        size = last_block_indexes.size();
        pthread_mutex_unlock(&mutex);

        return size;
    }
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

void set_timestamp(google::protobuf::Timestamp *ts);

#endif