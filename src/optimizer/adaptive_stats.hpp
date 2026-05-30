#pragma once
// ============================================================
// adaptive_stats.hpp — Phase 82: Adaptive Query Statistics
// Tracks table/column access patterns and suggests indexes.
// ============================================================

#include <string>
#include <vector>
#include <map>
#include <fstream>
#include <sstream>
#include <iostream>
#include <iomanip>
#include <algorithm>

#include "../engine/engine.hpp"
#include "../parser/parser.hpp"

namespace milansql {

class AdaptiveStats {
public:
    AdaptiveStats()  { loadStats(); }
    ~AdaptiveStats() { saveStats(); }

    void recordQuery(const ParsedCommand& cmd) {
        if (cmd.tableName.empty()) return;
        if (cmd.type != CommandType::SELECT &&
            cmd.type != CommandType::UPDATE &&
            cmd.type != CommandType::DELETE &&
            cmd.type != CommandType::INSERT) return;
        // Skip internal temp tables
        if (cmd.tableName.size() >= 2 && cmd.tableName.substr(0, 2) == "__") return;

        tableAccessCount_[cmd.tableName]++;

        for (const auto& wc : cmd.whereConds)
            if (!wc.col.empty())
                columnFilterCount_[cmd.tableName][wc.col]++;
        if (!cmd.whereColumn.empty())
            columnFilterCount_[cmd.tableName][cmd.whereColumn]++;
    }

    void showStats() const {
        std::cout << "\n";
        if (tableAccessCount_.empty()) {
            std::cout << "  (Keine Query-Statistiken vorhanden)\n\n";
            return;
        }
        const std::string h0 = "Tabelle", h1 = "Zugriffe", h2 = "Haeufigste WHERE-Spalte";
        size_t w0 = h0.size(), w1 = h1.size(), w2 = h2.size();

        struct RowData { std::string tbl, cnt, col; };
        std::vector<RowData> rows;

        for (const auto& kv : tableAccessCount_) {
            const std::string& tbl = kv.first;
            int cnt = kv.second;
            std::string topCol = "-";
            int topCount = 0;
            auto it = columnFilterCount_.find(tbl);
            if (it != columnFilterCount_.end()) {
                for (const auto& cv : it->second)
                    if (cv.second > topCount) { topCount = cv.second; topCol = cv.first; }
                if (topCount > 0) topCol += " (" + std::to_string(topCount) + "x)";
            }
            std::string cntStr = std::to_string(cnt);
            w0 = std::max(w0, tbl.size());
            w1 = std::max(w1, cntStr.size());
            w2 = std::max(w2, topCol.size());
            rows.push_back({tbl, cntStr, topCol});
        }

        auto bar = [&]() {
            return "+" + std::string(w0+2,'-') + "+"
                 + std::string(w1+2,'-') + "+"
                 + std::string(w2+2,'-') + "+";
        };
        auto printRow = [&](const std::string& a, const std::string& b, const std::string& c) {
            std::cout << "  | " << std::left
                      << std::setw(static_cast<int>(w0)) << a << " | "
                      << std::setw(static_cast<int>(w1)) << b << " | "
                      << std::setw(static_cast<int>(w2)) << c << " |\n";
        };
        std::cout << "  " << bar() << "\n";
        printRow(h0, h1, h2);
        std::cout << "  " << bar() << "\n";
        for (const auto& r : rows) printRow(r.tbl, r.cnt, r.col);
        std::cout << "  " << bar() << "\n";
        std::cout << "  " << rows.size() << " Tabelle(n) beobachtet.\n\n";
    }

    std::vector<std::string> suggestIndexes() const {
        std::vector<std::string> out;
        for (const auto& tv : columnFilterCount_) {
            const std::string& tbl = tv.first;
            int total = 0;
            auto it = tableAccessCount_.find(tbl);
            if (it != tableAccessCount_.end()) total = it->second;
            if (total == 0) continue;
            for (const auto& cv : tv.second) {
                const std::string& col = cv.first;
                int cnt = cv.second;
                if (col.empty()) continue;
                int pct = (cnt * 100) / total;
                if (pct >= 20) {
                    std::string idxName = "idx_" + tbl + "_" + col;
                    out.push_back("CREATE INDEX " + idxName + " ON " + tbl
                        + "(" + col + ")  -- " + std::to_string(pct)
                        + "% der Queries filtern nach " + col);
                }
            }
        }
        return out;
    }

    void showIndexSuggestions() const {
        auto sugg = suggestIndexes();
        std::cout << "\n";
        if (sugg.empty()) {
            std::cout << "  (Keine Index-Empfehlungen — mehr Queries notwendig)\n\n";
            return;
        }
        std::cout << "  Index-Empfehlungen:\n";
        for (const auto& s : sugg)
            std::cout << "  " << s << "\n";
        std::cout << "\n";
    }

    void analyzeTable(const std::string& tbl) {
        tableAccessCount_.erase(tbl);
        columnFilterCount_.erase(tbl);
        tableAccessCount_[tbl] = 0;
        std::cout << "  ANALYZE TABLE '" << tbl << "': Statistiken zurueckgesetzt.\n\n";
    }

    void saveStats(const std::string& path = "database.stats") const {
        std::ofstream f(path);
        if (!f) return;
        for (const auto& kv : tableAccessCount_)
            f << "T\t" << kv.first << "\t" << kv.second << "\n";
        for (const auto& tv : columnFilterCount_)
            for (const auto& cv : tv.second)
                f << "C\t" << tv.first << "\t" << cv.first << "\t" << cv.second << "\n";
    }

    void loadStats(const std::string& path = "database.stats") {
        std::ifstream f(path);
        if (!f) return;
        std::string line;
        while (std::getline(f, line)) {
            if (line.empty()) continue;
            std::istringstream ss(line);
            std::string type;
            std::getline(ss, type, '\t');
            if (type == "T") {
                std::string tbl, cnt;
                std::getline(ss, tbl, '\t');
                std::getline(ss, cnt);
                if (!tbl.empty())
                    try { tableAccessCount_[tbl] = std::stoi(cnt); } catch (...) {}
            } else if (type == "C") {
                std::string tbl, col, cnt;
                std::getline(ss, tbl, '\t');
                std::getline(ss, col, '\t');
                std::getline(ss, cnt);
                if (!tbl.empty() && !col.empty())
                    try { columnFilterCount_[tbl][col] = std::stoi(cnt); } catch (...) {}
            }
        }
    }

private:
    std::map<std::string, int> tableAccessCount_;
    std::map<std::string, std::map<std::string, int>> columnFilterCount_;
};

} // namespace milansql
