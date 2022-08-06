export MY_INSTALL_DIR := $(HOME)/.local
export PKG_CONFIG_PATH := $(MY_INSTALL_DIR)/lib/pkgconfig
export PATH := $(PATH):$(MY_INSTALL_DIR)/bin

CXX = g++-9
USERFLAGS = -DELPP_THREAD_SAFE 
CPPFLAGS += -I./include -I../leveldb/include `pkg-config --cflags protobuf grpc` $(USERFLAGS)
CXXFLAGS += -std=c++17 -ggdb3
LDFLAGS += -L/usr/local/lib `pkg-config --libs protobuf grpc++`\
		   -L../leveldb/build -lleveldb\
           -pthread -lcrypto\
           -Wl,--no-as-needed -lgrpc++_reflection -Wl,--as-needed\
           -ldl
PROTOC = protoc
GRPC_CPP_PLUGIN = grpc_cpp_plugin
GRPC_CPP_PLUGIN_PATH ?= `which $(GRPC_CPP_PLUGIN)`
PROTOS_PATH = .

all: peer client

peer: peer.cc easylogging++.cc smart_contracts.o consensus.o blockchain.pb.o blockchain.grpc.pb.o
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ $(LDFLAGS) -o $@

client: client.cc easylogging++.cc blockchain.pb.o blockchain.grpc.pb.o
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ $(LDFLAGS) -o $@

%.grpc.pb.cc: %.proto
	$(PROTOC) -I $(PROTOS_PATH) --grpc_out=. --plugin=protoc-gen-grpc=$(GRPC_CPP_PLUGIN_PATH) $<

%.pb.cc: %.proto
	$(PROTOC) -I $(PROTOS_PATH) --cpp_out=. $<

.PHONY: clean
clean:
	rm -f *.o peer client