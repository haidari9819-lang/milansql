#pragma once
#include <string>
#include <map>
#include <vector>
#include <algorithm>

namespace milansql {

struct StatsColumnInfo {
    std::string colName;
    long long distinctCount = 0;
    double nullFraction = 0.0;
    std::string mostCommonValue;
    double mcvFreq = 0.0;
    double avgWidth = 8.0;
};

struct StatsTableInfo {
    std::string tableName;
    long long rowCount = 0;
    int pageCount = 0;
    double fillFactor = 1.0;
    std::string lastAnalyzed = "never";
    std::vector<StatsColumnInfo> columnStats;
};

struct StatisticsDef {
    std::string name;
    std::string tableName;
    std::vector<std::string> columns;
    std::string createdAt;
    std::string kind = "dependencies"; // dependencies, ndistinct, mcv
};

class StatisticsManager {
    std::map<std::string, StatsTableInfo> tableStats_;
    std::map<std::string, StatisticsDef> statsDefs_;

public:
    void analyzeTable(const std::string& tableName, long long rowCount, const std::vector<std::string>& cols) {
        StatsTableInfo ts;
        ts.tableName = tableName;
        ts.rowCount = rowCount;
        ts.pageCount = (int)std::max(1LL, rowCount / 100);
        ts.lastAnalyzed = "2026-06-05";
        for (auto& col : cols) {
            StatsColumnInfo cs;
            cs.colName = col;
            cs.distinctCount = std::max(1LL, rowCount / 3);
            cs.nullFraction = 0.01;
            cs.avgWidth = 12.0;
            ts.columnStats.push_back(cs);
        }
        tableStats_[tableName] = ts;
    }

    bool hasStats(const std::string& tableName) const {
        return tableStats_.count(tableName) > 0;
    }

    const StatsTableInfo* getTableStats(const std::string& t) const {
        auto it = tableStats_.find(t);
        return it != tableStats_.end() ? &it->second : nullptr;
    }

    const std::map<std::string, StatsTableInfo>& getAllTableStats() const { return tableStats_; }

    void createStatistics(const std::string& name, const std::string& tableName, const std::vector<std::string>& cols) {
        StatisticsDef sd;
        sd.name = name;
        sd.tableName = tableName;
        sd.columns = cols;
        sd.createdAt = "2026-06-05";
        statsDefs_[name] = sd;
    }

    const std::map<std::string, StatisticsDef>& getAllStatsDefs() const { return statsDefs_; }

    const std::map<std::string, StatisticsDef>& getStatsDefs() const { return statsDefs_; }

    std::vector<StatisticsDef> getStatsForTable(const std::string& t) const {
        std::vector<StatisticsDef> result;
        for (auto& [k, v] : statsDefs_)
            if (v.tableName == t) result.push_back(v);
        return result;
    }
};

} // namespace milansql
