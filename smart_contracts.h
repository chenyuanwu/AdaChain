#ifndef SMART_CONTRACT_H
#define SMART_CONTRACT_H

#include <assert.h>
#include <google/protobuf/repeated_field.h>
#include <math.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <queue>
#include <random>
#include <string>
#include <unordered_set>

#include "blockchain.grpc.pb.h"
#include "easylogging++.h"
#include "rapidjson/document.h"
#include "rapidjson/istreamwrapper.h"

using namespace rapidjson;
using namespace google::protobuf;
using namespace std;

struct RecordVersion {
    uint64_t version_blockid;
    uint64_t version_transid;
};

bool ycsb_get(const RepeatedPtrField<string> &keys, Endorsement *endorsement, long long last_block_id=-1);
void ycsb_put(const RepeatedPtrField<string> &keys, const RepeatedPtrField<string> &values, struct RecordVersion record_version,
              bool expose_write, Endorsement *endorsement = nullptr);
string kv_get(const string &key, Endorsement *endorsement = nullptr, struct RecordVersion *record_version = nullptr, uint64_t &last_block_id = 0);
int kv_put(const string &key, const string &value, struct RecordVersion record_version, bool expose_write,
           Endorsement *endorsement = nullptr);
bool smallbank(const RepeatedPtrField<string> &keys, TransactionProposal::Type type, int execution_delay, bool expose_write,
               struct RecordVersion record_version, Endorsement *endorsement = nullptr, uint64_t &last_block_id = 0);
#endif