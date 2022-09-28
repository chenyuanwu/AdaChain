#ifndef GRAPH_H
#define GRAPH_H

#include <assert.h>
#include <bits/stdc++.h>

#include <algorithm>
#include <boost/dynamic_bitset.hpp>
#include <boost/heap/fibonacci_heap.hpp>
#include <limits>
#include <map>
#include <queue>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "blockchain.grpc.pb.h"
#include "easylogging++.h"
#include "rapidjson/document.h"

using namespace std;
using namespace rapidjson;

template <typename T>
bool vector_contains_value(const vector<T>& vect, const T& value) {
    return find(vect.begin(), vect.end(), value) != vect.end();
}

struct Vertex {
    Vertex() : valid(true), index(-1), lowlink(-1) {}
    bool valid;
    set<int> out_edges;
    set<int> in_edges;
    // We sometimes wish to consider a subgraph of a graph. A subgraph would have
    // a subset of the vertices from the graph and a subset of the edges.
    // When considering this vertex within a subgraph, subgraph_edges stores
    // the out-edges.
    set<int> subgraph_edges;

    // For Tarjan's algorithm:
    int index;
    int lowlink;
};

typedef vector<Vertex> Graph;

class TarjanAlgorithm {
   public:
    TarjanAlgorithm() : i_(0), required_vertex_(0) {}

    // 'out' is set to the result if there is one, otherwise it's untouched.
    void execute(int vertex, Graph* graph, vector<int>* out);

   private:
    void strong_connect(int vertex, Graph* graph);

    int i_;
    int required_vertex_;  // CyclesSearch asks for the SCC which contains the required_vertex_
    vector<int> stack_;
    vector<vector<int>> components_;
};

class CyclesSearch {
   public:
    CyclesSearch() {
    }
    vector<vector<int>> cycles;
    void get_elementary_cycles(const Graph& graph);

   private:
    void unblock(int u);
    bool circuit(int vertex);

    vector<bool> blocked_;          // "blocked" in the paper
    int current_vertex_;            // "s" in the paper
    vector<int> stack_;             // the stack variable in the paper
    Graph subgraph_;                // "A_K" in the paper
    vector<set<int>> blocked_map_;  // "B" in the paper
};

struct heap_data {
    int key;
    int payload;

    heap_data(int i, int j) : key(i), payload(j) {}

    bool operator<(heap_data const& rhs) const {
        return payload < rhs.payload;
    }
};

void xov_reorder(queue<string>& request_queue, Block& block);
void build_conflict_graph_oxii(queue<string>& request_queue, vector<TransactionProposal>& proposals, Graph& conflict_graph);

#endif