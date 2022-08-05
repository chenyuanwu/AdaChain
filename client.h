#ifndef CLIENT_H
#define CLIENT_H

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
#include <random>

#include "blockchain.grpc.pb.h"
#include "easylogging++.h"
#include "rapidjson/document.h"
#include "rapidjson/istreamwrapper.h"

#define BALANCE_HIGH 100000

using namespace rapidjson;
using namespace std;
using grpc::Channel;
using grpc::ClientAsyncWriter;
using grpc::ClientContext;
using grpc::ClientWriter;
using grpc::CompletionQueue;
using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::ServerReader;
using grpc::Status;

struct CliThreadContext {
    string peer_grpc_endpoint;
    int trans_per_interval;
    int interval;
    int num_keys;
    double hot_key_ratio;
    double write_ratio;
};

#endif