#pragma once
// ============================================================
// hnsw_index.hpp — Phase 111: Hierarchical Navigable Small World
//
// Simplified HNSW for approximate nearest neighbour search.
// Supports L2, cosine, and inner product metrics.
// M=16 connections per node, ef_construction=64, ef_search=10.
//
// Reference: Malkov & Yashunin (2018) arXiv:1603.09320
// ============================================================

#include <string>
#include <vector>
#include <map>
#include <set>
#include <queue>
#include <functional>
#include <random>
#include <cmath>
#include <mutex>
#include <algorithm>
#include <limits>

#include "../types/vector_type.hpp"

namespace milansql {

enum class VectorMetric { L2, COSINE, INNER_PRODUCT };

// ── HNSW Index ────────────────────────────────────────────────
class HnswIndex {
public:
    struct Config {
        int M                = 16;   // max connections per layer
        int ef_construction  = 64;   // candidate queue size during build
        int ef_search        = 10;   // candidate queue size during search
        VectorMetric metric  = VectorMetric::L2;
    };

    HnswIndex() : cfg_(Config{}), rng_(42) {}
    explicit HnswIndex(const Config& cfg) : cfg_(cfg), rng_(42) {}

    // Non-copyable, non-movable (mutex member)
    HnswIndex(const HnswIndex&) = delete;
    HnswIndex& operator=(const HnswIndex&) = delete;
    HnswIndex(HnswIndex&&) = delete;
    HnswIndex& operator=(HnswIndex&&) = delete;

    // Insert a vector with a given row ID
    void insert(int64_t id, const std::vector<float>& vec) {
        std::lock_guard<std::mutex> lk(mu_);
        if (vec.empty()) return;

        Node node;
        node.id  = id;
        node.vec = vec;
        node.level = randomLevel();
        nodes_.push_back(node);
        size_t ni = nodes_.size() - 1;

        if (nodes_.size() == 1) {
            // First node: it is the entry point
            entryIdx_ = 0;
            entryLevel_ = node.level;
            // Ensure connections vector has enough levels
            nodes_[ni].conns.resize(static_cast<size_t>(node.level + 1));
            return;
        }

        // Ensure connections vector size
        nodes_[ni].conns.resize(static_cast<size_t>(node.level + 1));

        // Start greedy search from entry point down to node.level+1
        size_t ep = entryIdx_;
        for (int lc = entryLevel_; lc > node.level; --lc) {
            ep = greedyClosest(vec, ep, lc);
        }

        // For each level from min(node.level, entryLevel_) down to 0
        for (int lc = std::min(node.level, entryLevel_); lc >= 0; --lc) {
            // Get candidates
            auto candidates = searchLayer(vec, ep, cfg_.ef_construction, lc);
            auto neighbors  = selectNeighbors(ni, candidates, cfg_.M, lc);

            // Add bidirectional links
            for (size_t nb : neighbors) {
                if (nb == ni) continue;
                addLink(ni, nb, lc);
                addLink(nb, ni, lc);
                // Prune if too many connections
                pruneConnections(nb, lc, cfg_.M);
            }

            // Update entry point for next level
            if (!candidates.empty()) ep = candidates[0].second;
        }

        // Update global entry point if new node has higher level
        if (node.level > entryLevel_) {
            entryIdx_  = ni;
            entryLevel_ = node.level;
        }
    }

    // Search for k nearest neighbours to query vector
    // Returns list of {distance, row_id} sorted by distance ascending
    std::vector<std::pair<float, int64_t>> search(
            const std::vector<float>& query, int k) const {
        std::lock_guard<std::mutex> lk(mu_);
        if (nodes_.empty() || query.empty()) return {};

        size_t ep = entryIdx_;
        // Greedy descent to level 1
        for (int lc = entryLevel_; lc > 0; --lc) {
            ep = greedyClosest(query, ep, lc);
        }

        // Search at level 0 with ef = max(k, ef_search)
        int ef = std::max(k, cfg_.ef_search);
        auto candidates = searchLayer(query, ep, ef, 0);

        // Return top k
        std::vector<std::pair<float, int64_t>> result;
        int cnt = 0;
        for (const auto& [dist, ni] : candidates) {
            if (cnt++ >= k) break;
            result.push_back({dist, nodes_[ni].id});
        }
        std::sort(result.begin(), result.end());
        if (static_cast<int>(result.size()) > k) result.resize(k);
        return result;
    }

    // Delete all entries with the given row ID
    void remove(int64_t id) {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& node : nodes_) {
            if (node.id == id) node.deleted = true;
        }
    }

    size_t size() const {
        std::lock_guard<std::mutex> lk(mu_);
        size_t cnt = 0;
        for (const auto& n : nodes_) if (!n.deleted) ++cnt;
        return cnt;
    }

    VectorMetric metric() const { return cfg_.metric; }

private:
    struct Node {
        int64_t              id      = 0;
        std::vector<float>   vec;
        int                  level   = 0;
        bool                 deleted = false;
        // conns[lc] = list of neighbour indices at level lc
        std::vector<std::vector<size_t>> conns;
    };

    Config                    cfg_;
    std::vector<Node>         nodes_;
    size_t                    entryIdx_   = 0;
    int                       entryLevel_ = 0;
    mutable std::mt19937      rng_;
    mutable std::mutex        mu_;

    float distance(const std::vector<float>& a, const std::vector<float>& b) const {
        switch (cfg_.metric) {
            case VectorMetric::COSINE:
                return vector_type::cosineDistance(a, b);
            case VectorMetric::INNER_PRODUCT:
                // Negate so lower = better (max inner product → min negated IP)
                return -vector_type::innerProduct(a, b);
            default:
                return vector_type::l2Distance(a, b);
        }
    }

    float distance(size_t ni, const std::vector<float>& q) const {
        if (nodes_[ni].deleted) return std::numeric_limits<float>::max();
        return distance(nodes_[ni].vec, q);
    }

    int randomLevel() {
        std::uniform_real_distribution<double> dist(0.0, 1.0);
        double r = dist(rng_);
        int level = 0;
        double mL = 1.0 / std::log(static_cast<double>(cfg_.M));
        while (r < std::exp(-static_cast<double>(level + 1) / (mL + 1e-9)) && level < 16) {
            ++level;
            r = dist(rng_);
        }
        return level;
    }

    // Greedy search: find the closest node to query at a given level
    size_t greedyClosest(const std::vector<float>& query,
                         size_t startIdx, int level) const {
        size_t cur = startIdx;
        float curDist = distance(cur, query);
        bool improved = true;
        while (improved) {
            improved = false;
            if (static_cast<size_t>(level) >= nodes_[cur].conns.size()) break;
            for (size_t nb : nodes_[cur].conns[static_cast<size_t>(level)]) {
                if (nb >= nodes_.size()) continue;
                float d = distance(nb, query);
                if (d < curDist) {
                    curDist  = d;
                    cur      = nb;
                    improved = true;
                }
            }
        }
        return cur;
    }

    // BFS-style search at a given level; returns candidates sorted by distance asc
    // Returns {dist, node_index} sorted ascending
    std::vector<std::pair<float, size_t>> searchLayer(
            const std::vector<float>& query,
            size_t ep, int ef, int level) const {
        // visited set
        std::set<size_t> visited;
        // candidates: min-heap by distance
        using PairFS = std::pair<float, size_t>;
        std::priority_queue<PairFS, std::vector<PairFS>, std::greater<PairFS>> cands;
        // results: max-heap (so we can prune worst)
        std::priority_queue<PairFS> results;

        float d0 = distance(ep, query);
        cands.push({d0, ep});
        results.push({d0, ep});
        visited.insert(ep);

        while (!cands.empty()) {
            auto [dist, cur] = cands.top(); cands.pop();
            if (!results.empty() && dist > results.top().first) break;

            if (static_cast<size_t>(level) >= nodes_[cur].conns.size()) continue;
            for (size_t nb : nodes_[cur].conns[static_cast<size_t>(level)]) {
                if (nb >= nodes_.size()) continue;
                if (visited.count(nb)) continue;
                visited.insert(nb);
                float dn = distance(nb, query);
                if (static_cast<int>(results.size()) < ef || dn < results.top().first) {
                    cands.push({dn, nb});
                    results.push({dn, nb});
                    if (static_cast<int>(results.size()) > ef) results.pop();
                }
            }
        }

        // Convert to sorted vector
        std::vector<PairFS> out;
        while (!results.empty()) { out.push_back(results.top()); results.pop(); }
        std::sort(out.begin(), out.end());
        return out;
    }

    // Select M best neighbours from candidates (simple greedy)
    std::vector<size_t> selectNeighbors(
            size_t /*ni*/,
            const std::vector<std::pair<float,size_t>>& candidates,
            int M, int /*level*/) const {
        std::vector<size_t> result;
        for (const auto& [dist, idx] : candidates) {
            if (static_cast<int>(result.size()) >= M) break;
            result.push_back(idx);
        }
        return result;
    }

    void addLink(size_t from, size_t to, int level) {
        auto& conns = nodes_[from].conns;
        if (static_cast<size_t>(level) >= conns.size())
            conns.resize(static_cast<size_t>(level + 1));
        conns[static_cast<size_t>(level)].push_back(to);
    }

    void pruneConnections(size_t ni, int level, int maxM) {
        if (static_cast<size_t>(level) >= nodes_[ni].conns.size()) return;
        auto& conns = nodes_[ni].conns[static_cast<size_t>(level)];
        if (static_cast<int>(conns.size()) <= maxM) return;

        // Keep closest maxM neighbours
        const auto& vec = nodes_[ni].vec;
        std::sort(conns.begin(), conns.end(), [&](size_t a, size_t b) {
            return distance(a, vec) < distance(b, vec);
        });
        conns.resize(static_cast<size_t>(maxM));
    }
};

// ── VectorIndexManager — one index per (table, column) ────────
class VectorIndexManager {
public:
    struct IndexKey {
        std::string tableName;
        std::string colName;
        std::string method; // "hnsw" or "ivfflat"
        VectorMetric metric = VectorMetric::L2;

        bool operator<(const IndexKey& o) const {
            if (tableName != o.tableName) return tableName < o.tableName;
            return colName < o.colName;
        }
    };

    // Create or replace an index
    void createIndex(const std::string& table, const std::string& col,
                     const std::string& method, VectorMetric metric) {
        IndexKey key{table, col, method, metric};
        HnswIndex::Config cfg;
        cfg.metric = metric;
        // Remove existing index for same table+col first
        for (auto it = indexes_.begin(); it != indexes_.end(); ) {
            if (it->first.tableName == table && it->first.colName == col)
                it = indexes_.erase(it);
            else ++it;
        }
        indexes_.emplace(key, std::make_unique<HnswIndex>(cfg));
    }

    bool hasIndex(const std::string& table, const std::string& col) const {
        for (const auto& [k, _] : indexes_)
            if (k.tableName == table && k.colName == col) return true;
        return false;
    }

    HnswIndex* getIndex(const std::string& table, const std::string& col) {
        for (auto& [k, idx] : indexes_)
            if (k.tableName == table && k.colName == col) return idx.get();
        return nullptr;
    }

    std::string showIndexes() const {
        if (indexes_.empty()) return "  (no vector indexes)\n";
        std::string out = "\n";
        for (const auto& [k, idx] : indexes_) {
            out += "  " + k.tableName + "." + k.colName
                + " [" + k.method + ", "
                + (k.metric == VectorMetric::L2 ? "L2" :
                   k.metric == VectorMetric::COSINE ? "cosine" : "ip")
                + ", " + std::to_string(idx->size()) + " entries]\n";
        }
        out += "\n";
        return out;
    }

private:
    std::map<IndexKey, std::unique_ptr<HnswIndex>> indexes_;
};

inline VectorIndexManager& g_vectorIndexManager() {
    static VectorIndexManager mgr;
    return mgr;
}

} // namespace milansql
