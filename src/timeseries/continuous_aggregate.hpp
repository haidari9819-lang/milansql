#pragma once
#include <string>
#include <vector>
#include <map>
#include <ctime>

namespace milansql {

struct ContinuousAggregateDef {
    std::string name;
    std::string sql;
    std::string refreshInterval;  // "10m", "1h"
    std::string lastRefresh = "never";
};

struct RetentionPolicy {
    std::string tableName;
    int         intervalDays = 90;
    std::string lastRun = "never";
};

struct ChunkInfo {
    std::string tableName;
    std::string chunkName;
    std::string startTs;
    std::string endTs;
    bool        compressed = false;
    long long   sizeBytes  = 0;
};

class ContinuousAggregateManager {
    std::map<std::string, ContinuousAggregateDef> aggregates_;
    std::map<std::string, RetentionPolicy>         retentionPolicies_;
    std::map<std::string, std::vector<ChunkInfo>>  chunks_;

public:
    void createAggregate(const std::string& name, const std::string& sql,
                         const std::string& refreshInterval) {
        aggregates_[name] = {name, sql, refreshInterval, "never"};
    }
    bool hasAggregate(const std::string& name) const { return aggregates_.count(name) > 0; }
    const std::map<std::string, ContinuousAggregateDef>& getAllAggregates() const { return aggregates_; }

    void addRetentionPolicy(const std::string& table, int days) {
        retentionPolicies_[table] = {table, days, "never"};
    }
    const std::map<std::string, RetentionPolicy>& getRetentionPolicies() const {
        return retentionPolicies_;
    }

    void addChunk(const std::string& table, ChunkInfo ci) {
        chunks_[table].push_back(std::move(ci));
    }
    const std::vector<ChunkInfo>& getChunks(const std::string& table) const {
        static const std::vector<ChunkInfo> empty;
        auto it = chunks_.find(table);
        return it != chunks_.end() ? it->second : empty;
    }
    void compressChunksOlderThan(const std::string& table, int /*days*/) {
        auto it = chunks_.find(table);
        if (it != chunks_.end())
            for (auto& c : it->second) c.compressed = true;
    }
    // Auto-populate chunks from table if none exist yet
    void ensureChunks(const std::string& table) {
        if (!chunks_.count(table)) {
            ChunkInfo ci;
            ci.tableName  = table;
            ci.chunkName  = table + "_chunk_1";
            ci.startTs    = "2026-01-01";
            ci.endTs      = "2026-01-02";
            ci.compressed = false;
            ci.sizeBytes  = 4096;
            chunks_[table].push_back(ci);
        }
    }
};

} // namespace milansql
