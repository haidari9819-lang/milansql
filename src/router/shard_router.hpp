#pragma once
// ============================================================
// shard_router.hpp — Sharding (Phase 3.3)
// Shard metadata management + consistent-hash-based routing.
// Actual cross-node transport is simulated (nodes may not exist).
// ============================================================

#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <stdexcept>
#include <cstdint>

namespace milansql {

struct ShardNode {
    std::string address; // "host:port"
    int shardId;
};

struct ShardedTable {
    std::string tableName;
    std::string shardKey;
    int numShards;
    std::vector<ShardNode> nodes;
};

class ShardRouter {
public:
    static ShardRouter& global() {
        static ShardRouter inst;
        return inst;
    }

    bool createShardedTable(const ShardedTable& st, std::string& error) {
        std::lock_guard<std::mutex> lk(mu_);
        if (shardedTables_.count(st.tableName)) {
            error = "Sharded table '" + st.tableName + "' already exists";
            return false;
        }
        if (st.numShards <= 0) {
            error = "numShards must be > 0";
            return false;
        }
        shardedTables_[st.tableName] = st;
        return true;
    }

    // FNV-1a hash of keyValue % numShards
    int routeShard(const std::string& table, const std::string& keyValue) const {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = shardedTables_.find(table);
        if (it == shardedTables_.end()) return -1;
        int ns = it->second.numShards;
        if (ns <= 0) return 0;
        uint64_t hash = 14695981039346656037ULL;
        for (unsigned char c : keyValue) {
            hash ^= static_cast<uint64_t>(c);
            hash *= 1099511628211ULL;
        }
        return static_cast<int>(hash % static_cast<uint64_t>(ns));
    }

    std::vector<ShardNode> getNodes(const std::string& table) const {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = shardedTables_.find(table);
        if (it == shardedTables_.end()) return {};
        return it->second.nodes;
    }

    std::vector<ShardedTable> listShards() const {
        std::lock_guard<std::mutex> lk(mu_);
        std::vector<ShardedTable> result;
        for (auto& kv : shardedTables_) result.push_back(kv.second);
        return result;
    }

    bool isSharded(const std::string& table) const {
        std::lock_guard<std::mutex> lk(mu_);
        return shardedTables_.count(table) > 0;
    }

    ShardedTable getShardInfo(const std::string& table) const {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = shardedTables_.find(table);
        if (it == shardedTables_.end())
            throw std::runtime_error("Table '" + table + "' is not sharded");
        return it->second;
    }

    bool dropShardedTable(const std::string& table, std::string& error) {
        std::lock_guard<std::mutex> lk(mu_);
        if (!shardedTables_.count(table)) {
            error = "Sharded table '" + table + "' not found";
            return false;
        }
        shardedTables_.erase(table);
        return true;
    }

private:
    mutable std::mutex mu_;
    std::map<std::string, ShardedTable> shardedTables_;
};

} // namespace milansql
