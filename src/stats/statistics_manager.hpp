#pragma once
#include <string>
#include <map>
#include <vector>

// ============================================================
// statistics_manager.hpp — Phase 149: CREATE STATISTICS Katalog
//
// KONSOLIDIERT (Optimizer Phase 1): Die früheren Fake-Tabellen-/
// Spaltenstatistiken (hardcodete Schätzwerte) wurden entfernt.
// Echte Statistiken (Row-Count, Cardinality, NULL-Ratio, Min/Max,
// Index-Selektivität, Sampling) liefert ausschließlich der
// TableStatsManager in src/optimizer/table_stats.hpp (g_tableStats).
//
// Diese Klasse bleibt bewusst bestehen, weil sie etwas anderes
// verwaltet: die METADATEN von CREATE STATISTICS — benutzerdefinierte
// Multi-Column-Statistik-Objekte (analog zu Postgres' pg_statistic_ext),
// die von SHOW STATISTICS gelistet werden. Sie enthält keine Zahlen.
// ============================================================

namespace milansql {

struct StatisticsDef {
    std::string name;
    std::string tableName;
    std::vector<std::string> columns;
    std::string createdAt;
    std::string kind = "dependencies"; // dependencies, ndistinct, mcv
};

class StatisticsManager {
    std::map<std::string, StatisticsDef> statsDefs_;

public:
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
