#include "graph.h"

const static int k_invalid_index = -1;

void TarjanAlgorithm::execute(int vertex, Graph* graph, vector<int>* out) {
    stack_.clear();
    components_.clear();
    i_ = 0;
    for (auto it = graph->begin(); it != graph->end(); ++it) {
        it->index = it->lowlink = k_invalid_index;
    }
    required_vertex_ = vertex;

    strong_connect(vertex, graph);
    if (!components_.empty())
        out->swap(components_[0]);
}

void TarjanAlgorithm::strong_connect(int vertex, Graph* graph) {
    assert((*graph)[vertex].index == k_invalid_index);
    (*graph)[vertex].index = i_;
    (*graph)[vertex].lowlink = i_;
    i_++;
    stack_.push_back(vertex);
    for (auto it = (*graph)[vertex].out_edges.begin(); it != (*graph)[vertex].out_edges.end(); ++it) {
        int vertex_next = *it;
        if ((*graph)[vertex_next].index == k_invalid_index) {
            strong_connect(vertex_next, graph);
            (*graph)[vertex].lowlink = min((*graph)[vertex].lowlink,
                                           (*graph)[vertex_next].lowlink);
        } else if (vector_contains_value(stack_, vertex_next)) {
            (*graph)[vertex].lowlink = min((*graph)[vertex].lowlink,
                                           (*graph)[vertex_next].index);
        }
    }
    if ((*graph)[vertex].lowlink == (*graph)[vertex].index) {
        vector<int> component;
        int other_vertex;
        do {
            other_vertex = stack_.back();
            stack_.pop_back();
            component.push_back(other_vertex);
        } while (other_vertex != vertex && !stack_.empty());

        if (vector_contains_value(component, required_vertex_)) {
            components_.resize(components_.size() + 1);
            component.swap(components_.back());
        }
    }
}

// This is the outer function from the original Johnson's paper.
void CyclesSearch::get_elementary_cycles(const Graph& graph) {
    // Make a copy, which we will modify by removing edges. Thus, in each
    // iteration subgraph_ is the current subgraph or the original with
    // vertices we desire. This variable was "A_K" in the original paper.
    subgraph_ = graph;

    // The paper calls for the "adjacency structure (i.e., graph) of
    // strong (-ly connected) component K with least vertex in subgraph
    // induced by {s, s + 1, ..., n}".
    // We arbitrarily order each vertex by its index in the graph. Thus,
    // each iteration, we are looking at the subgraph {s, s + 1, ..., n}
    // and looking for the strongly connected component with vertex s.
    TarjanAlgorithm tarjan;

    for (int i = 0; i < subgraph_.size(); i++) {
        if (i > 0) {
            // Erase node (i - 1) from subgraph_. First, erase what it points to
            subgraph_[i - 1].out_edges.clear();
            // Now, erase any pointers to node (i - 1)
            for (int j = i; j < subgraph_.size(); j++) {
                subgraph_[j].out_edges.erase(i - 1);
            }
        }

        // Calculate SCC (strongly connected component) with vertex i.
        vector<int> component_indexes;
        tarjan.execute(i, &subgraph_, &component_indexes);

        // Set subgraph edges in the SCC.
        for (auto it = component_indexes.begin(); it != component_indexes.end(); ++it) {
            subgraph_[*it].subgraph_edges.clear();
            for (auto jt = component_indexes.begin(); jt != component_indexes.end(); ++jt) {
                // If there's a link from *it -> *jt in the graph,
                // add a subgraph_ edge
                if (subgraph_[*it].out_edges.count(*jt))
                    subgraph_[*it].subgraph_edges.insert(*jt);
            }
        }

        current_vertex_ = i;
        blocked_.clear();
        blocked_.resize(subgraph_.size());
        blocked_map_.clear();
        blocked_map_.resize(subgraph_.size());
        circuit(current_vertex_);
    }

    assert(stack_.empty());
}

void CyclesSearch::unblock(int u) {
    blocked_[u] = false;

    for (auto it = blocked_map_[u].begin(); it != blocked_map_[u].end();) {
        int w = *it;
        blocked_map_[u].erase(it++);
        if (blocked_[w])
            unblock(w);
    }
}

bool CyclesSearch::circuit(int vertex) {
    // "vertex" was "v" in the original paper.
    bool found = false;  // Was "f" in the original paper.
    stack_.push_back(vertex);
    blocked_[vertex] = true;

    for (auto w = subgraph_[vertex].subgraph_edges.begin(); w != subgraph_[vertex].subgraph_edges.end(); ++w) {
        if (*w == current_vertex_) {
            // The original paper called for printing stack_ followed by
            // current_vertex_ here, which is a cycle.
            vector<int> cycle;
            for (int i = 0; i < stack_.size(); i++) {
                int index = stack_[i];
                cycle.push_back(index);
            }
            cycles.push_back(cycle);
            found = true;
        } else if (!blocked_[*w]) {
            if (circuit(*w)) {
                found = true;
            }
        }
    }

    if (found) {
        unblock(vertex);
    } else {
        for (auto w = subgraph_[vertex].subgraph_edges.begin(); w != subgraph_[vertex].subgraph_edges.end(); ++w) {
            if (blocked_map_[*w].find(vertex) == blocked_map_[*w].end()) {
                blocked_map_[*w].insert(vertex);
            }
        }
    }
    assert(vertex == stack_.back());
    stack_.pop_back();
    return found;
}

void build_conflict_graph_xov(const vector<Endorsement>& transactions, Graph& conflict_graph) {
    unordered_map<string, int> key_to_bitmap_idx;  // track the length of bitmap
    int bitmap_idx = 0;
    for (int i = 0; i < transactions.size(); i++) {
        for (int read_id = 0; read_id < transactions[i].read_set_size(); read_id++) {
            string read_key = transactions[i].read_set(read_id).read_key();
            if (key_to_bitmap_idx.find(read_key) == key_to_bitmap_idx.end()) {
                key_to_bitmap_idx[read_key] = bitmap_idx++;
            }
        }

        for (int write_id = 0; write_id < transactions[i].write_set_size(); write_id++) {
            string write_key = transactions[i].write_set(write_id).write_key();
            if (key_to_bitmap_idx.find(write_key) == key_to_bitmap_idx.end()) {
                key_to_bitmap_idx[write_key] = bitmap_idx++;
            }
        }
    }
    vector<boost::dynamic_bitset<>> read_bitmaps;  // construct the bitmaps
    vector<boost::dynamic_bitset<>> write_bitmaps;
    for (int i = 0; i < transactions.size(); i++) {
        boost::dynamic_bitset<> read_bitmap(bitmap_idx);
        for (int read_id = 0; read_id < transactions[i].read_set_size(); read_id++) {
            string read_key = transactions[i].read_set(read_id).read_key();
            int bitmap_idx = key_to_bitmap_idx[read_key];
            read_bitmap[bitmap_idx] = 1;
        }
        read_bitmaps.push_back(read_bitmap);

        boost::dynamic_bitset<> write_bitmap(bitmap_idx);
        for (int write_id = 0; write_id < transactions[i].write_set_size(); write_id++) {
            string write_key = transactions[i].write_set(write_id).write_key();
            int bitmap_idx = key_to_bitmap_idx[write_key];
            write_bitmap[bitmap_idx] = 1;
        }
        write_bitmaps.push_back(write_bitmap);
    }

    for (int i = 0; i < transactions.size(); i++) {
        conflict_graph.emplace_back();
        for (int j = 0; j < transactions.size(); j++) {
            if (i != j && (read_bitmaps[i] & write_bitmaps[j]).any()) {  // no self loop
                // add an edge in conflict_graph from i to j
                conflict_graph[i].out_edges.insert(j);
            }
        }
    }
}

void xov_reorder(queue<string>& request_queue, Block& block) {
    Graph conflict_graph;
    vector<Endorsement> S;  // the index represents the transaction id
    while (request_queue.size()) {
        Endorsement endorsement;
        if (!endorsement.ParseFromString(request_queue.front()) ||
            !endorsement.GetReflection()->GetUnknownFields(endorsement).empty()) {
            LOG(WARNING) << "block formation thread: error in deserialising endorsement.";
        } else {
            endorsement.set_aborted(false);
            S.push_back(endorsement);
        }
        request_queue.pop();
    }

    build_conflict_graph_xov(S, conflict_graph);  // step 1
    // LOG(INFO) << "finished step 1.";

    CyclesSearch cycles_search;
    cycles_search.get_elementary_cycles(conflict_graph);  // step 2
    // LOG(INFO) << "finished step 2 with " << cycles_search.cycles.size() << " cycles.";

    boost::heap::fibonacci_heap<heap_data> transactions_in_cycles;  // step 3
    typedef typename boost::heap::fibonacci_heap<heap_data>::handle_type handle_t;
    unordered_map<int, handle_t> transaction_to_handle;
    for (int i = 0; i < cycles_search.cycles.size(); i++) {
        for (int j = 0; j < cycles_search.cycles[i].size(); j++) {
            int t = cycles_search.cycles[i][j];
            auto it = transaction_to_handle.find(t);
            if (it != transaction_to_handle.end()) {
                int count = (*(it->second)).payload;
                count++;
                heap_data f(t, count);
                transactions_in_cycles.update(it->second, f);
            } else {
                heap_data f(t, 1);
                handle_t handle = transactions_in_cycles.push(f);
                transaction_to_handle[t] = handle;
            }
        }
    }
    // LOG(INFO) << "finished step 3.";

    while (!cycles_search.cycles.empty()) {  // step 4
        // LOG(INFO) << "now there exists " << cycles_search.cycles.size() << " cycles.";
        int t = transactions_in_cycles.top().key;
        int appear_count = transactions_in_cycles.top().payload;
        transactions_in_cycles.pop();
        // LOG(INFO) << "removed transaction " << t << ", which appeared in " << appear_count << " cycles.";
        S[t].set_aborted(true);
        for (auto c_it = cycles_search.cycles.begin(); c_it != cycles_search.cycles.end();) {
            auto p = find(c_it->begin(), c_it->end(), t);
            if (p != c_it->end()) {
                c_it->erase(p);
                for (auto it = c_it->begin(); it != c_it->end(); it++) {
                    int t_prime = *it;
                    int count = (*transaction_to_handle[t_prime]).payload;
                    count--;
                    heap_data f(t_prime, count);
                    transactions_in_cycles.update(transaction_to_handle[t_prime], f);
                }
                c_it = cycles_search.cycles.erase(c_it);
            } else {
                c_it++;
            }
        }
    }
    // LOG(INFO) << "finished step 4.";

    vector<Endorsement> S_prime;    // step 5
    vector<Endorsement> S_aborted;  // record early aborted transactions
    Graph conflict_graph_prime;     // cycle-free conflict graph
    for (int i = 0; i < S.size(); i++) {
        if (!S[i].aborted()) {
            S_prime.push_back(S[i]);
        } else {
            S_aborted.push_back(S[i]);
        }
    }
    // LOG(INFO) << "constructed S_prime, now S_prime has " << S_prime.size() << " transactions.";
    build_conflict_graph_xov(S_prime, conflict_graph_prime);

    vector<int> in_degree(conflict_graph_prime.size(), 0);
    queue<int> Q;
    for (auto u_it = conflict_graph_prime.begin(); u_it != conflict_graph_prime.end(); u_it++) {
        for (auto v_it = u_it->out_edges.begin(); v_it != u_it->out_edges.end(); v_it++) {
            in_degree[*v_it] = in_degree[*v_it] + 1;
        }
    }
    for (int u = 0; u < in_degree.size(); u++) {
        if (in_degree[u] == 0) {
            Q.push(u);
        }
    }
    while (Q.size()) {
        int u = Q.front();
        Q.pop();
        Endorsement* endorsement = block.add_transactions();  // output u
        (*endorsement) = S_prime[u];
        for (auto v_it = conflict_graph_prime[u].out_edges.begin(); v_it != conflict_graph_prime[u].out_edges.end(); v_it++) {
            in_degree[*v_it] = in_degree[*v_it] - 1;
            if (in_degree[*v_it] == 0) {
                Q.push(*v_it);
            }
        }
    }
    if (block.transactions_size() != conflict_graph_prime.size()) {
        LOG(ERROR) << "cycle detected in topological sort.";
    } else {
        // LOG(INFO) << "successfully finished topological sort.";
    }

    for (int i = 0; i < S_aborted.size(); i++) {
        Endorsement* endorsement = block.add_transactions();
        (*endorsement) = S_aborted[i];
    }
}

void build_conflict_graph_oxii(queue<string>& request_queue, vector<TransactionProposal>& proposals, Graph& conflict_graph) {
    int index = 0; // the index in proposals represents the transaction propoal id
    while (request_queue.size()) {
        TransactionProposal proposal;
        if (!proposal.ParseFromString(request_queue.front()) ||
            !proposal.GetReflection()->GetUnknownFields(proposal).empty()) {
            LOG(WARNING) << "block formation thread: error in deserialising endorsement.";
        } else {
            proposal.set_id(index++);
            proposals.push_back(proposal);
        }
        request_queue.pop();
    }

    unordered_map<string, int> key_to_bitmap_idx;  // track the length of bitmap
    int bitmap_idx = 0;
    for (int i = 0; i < proposals.size(); i++) {
        for (int key_id = 0; key_id < proposals[i].keys_size(); key_id++) {
            string key = proposals[i].keys(key_id);
            if (key_to_bitmap_idx.find(key) == key_to_bitmap_idx.end()) {
                key_to_bitmap_idx[key] = bitmap_idx++;
            }
        }
    }

    vector<boost::dynamic_bitset<>> read_bitmaps;  // construct the bitmaps
    vector<boost::dynamic_bitset<>> write_bitmaps;
    for (int i = 0; i < proposals.size(); i++) {
        boost::dynamic_bitset<> read_bitmap(bitmap_idx);
        boost::dynamic_bitset<> write_bitmap(bitmap_idx);
        if (proposals[i].type() == TransactionProposal::Type::TransactionProposal_Type_Get) {
            string read_key = proposals[i].keys(0);
            int bitmap_idx = key_to_bitmap_idx[read_key];
            read_bitmap[bitmap_idx] = 1;
        } else if (proposals[i].type() == TransactionProposal::Type::TransactionProposal_Type_Put) {
            string write_key = proposals[i].keys(0);
            int bitmap_idx = key_to_bitmap_idx[write_key];
            write_bitmap[bitmap_idx] = 1;
        } else if (proposals[i].type() == TransactionProposal::Type::TransactionProposal_Type_Query) {
            // read only transaction in smallbank
            for (int key_id = 0; key_id < proposals[i].keys_size(); key_id++) {
                string read_key = proposals[i].keys(key_id);
                int bitmap_idx = key_to_bitmap_idx[read_key];
                read_bitmap[bitmap_idx] = 1;
            }
        } else {
            // update transaction in smallbank
            for (int key_id = 0; key_id < proposals[i].keys_size(); key_id++) {
                string key = proposals[i].keys(key_id);
                int bitmap_idx = key_to_bitmap_idx[key];
                read_bitmap[bitmap_idx] = 1;
                write_bitmap[bitmap_idx] = 1;
            }
        }
        read_bitmaps.push_back(read_bitmap);
        write_bitmaps.push_back(write_bitmap);
    }

    conflict_graph.resize(proposals.size());
    for (int i = 0; i < proposals.size(); i++) {
        for (int j = i + 1; j < proposals.size(); j++) {  // only from early transaction (in arrival order) to later transaction
            if ((read_bitmaps[i] & write_bitmaps[j]).any() || (write_bitmaps[i] & read_bitmaps[j]).any() ||
                (write_bitmaps[i] & write_bitmaps[j]).any()) {
                // add an edge in conflict_graph from i to j
                conflict_graph[j].in_edges.insert(i);  // use in_edges instead of out_edges
            }
        }
    }
}
