#pragma once
// ============================================================
// table_stats.hpp — Phase 86: Statistik-basierter Query Planner
// Echte Spaltenstatistiken: Kardinalität, Min/Max, MCVs,
// Histogramm, Selektivitätsschätzung.
// ============================================================

namespace milansql {

// ── Spalten-Statistiken ────────────────────────────────────────
struct ColumnStats {
    size_t      rowCount     = 0;
    size_t      distinctCount = 0;
    size_t      nullCount     = 0;
    std::string minVal;
    std::string maxVal;
    // most common values: value → frequency (0..1)
    std::map<std::string, double> mostCommonVals;   // top 10
    // histogram: bucket upper bound → row count in bucket
    std::vector<std::pair<std::string, size_t>> histogram; // 10 buckets
};

// ── Tabellen-Statistiken ───────────────────────────────────────
struct TableStats {
    std::string name;
    size_t      rowCount = 0;
    std::map<std::string, ColumnStats> cols;
};

// ── Manager ───────────────────────────────────────────────────
class TableStatsManager {
public:
    TableStatsManager()  { loadStats(); }
    ~TableStatsManager() { saveStats(); }

    // Analyze a single table
    void analyzeTable(const std::string& name, const Table& tbl) {
        TableStats ts;
        ts.name     = name;
        ts.rowCount = tbl.rowCount();

        const auto& cols = tbl.columns();
        const auto& rows = tbl.rows();

        for (size_t ci = 0; ci < cols.size(); ++ci) {
            ColumnStats cs;
            cs.rowCount = ts.rowCount;

            std::map<std::string, size_t> freq;
            bool hasMin = false;

            for (const auto& row : rows) {
                const std::string& v = (ci < row.values.size()) ? row.values[ci] : "";
                if (v.empty()) { cs.nullCount++; continue; }
                freq[v]++;
                if (!hasMin) {
                    cs.minVal = v; cs.maxVal = v; hasMin = true;
                } else {
                    if (v < cs.minVal) cs.minVal = v;
                    if (v > cs.maxVal) cs.maxVal = v;
                }
            }

            cs.distinctCount = freq.size();

            // top-10 MCVs by frequency
            std::vector<std::pair<std::string, size_t>> sorted(freq.begin(), freq.end());
            std::sort(sorted.begin(), sorted.end(),
                      [](const auto& a, const auto& b){ return a.second > b.second; });
            size_t top = std::min<size_t>(10, sorted.size());
            for (size_t i = 0; i < top; ++i) {
                double rel = ts.rowCount > 0
                    ? static_cast<double>(sorted[i].second) / static_cast<double>(ts.rowCount)
                    : 0.0;
                cs.mostCommonVals[sorted[i].first] = rel;
            }

            // 10-bucket equi-depth histogram over sorted distinct values
            if (!sorted.empty()) {
                std::vector<std::string> vals;
                for (const auto& kv : freq) vals.push_back(kv.first);
                std::sort(vals.begin(), vals.end());
                size_t buckets = std::min<size_t>(10, vals.size());
                size_t perBucket = vals.size() / buckets;
                for (size_t b = 0; b < buckets; ++b) {
                    size_t idx = std::min((b + 1) * perBucket, vals.size()) - 1;
                    // count rows in this bucket range
                    size_t lo = b * perBucket;
                    size_t hi = (b + 1 < buckets) ? (b + 1) * perBucket : vals.size();
                    size_t cnt = 0;
                    for (size_t vi = lo; vi < hi; ++vi)
                        cnt += freq[vals[vi]];
                    cs.histogram.push_back({vals[idx], cnt});
                }
            }

            ts.cols[cols[ci].name] = std::move(cs);
        }

        stats_[name] = std::move(ts);
    }

    // Analyze all tables
    void analyzeAll(const std::map<std::string, Table>& tables) {
        for (const auto& kv : tables)
            analyzeTable(kv.first, kv.second);
    }

    bool hasStats(const std::string& name) const {
        return stats_.count(name) > 0;
    }

    // Show column statistics for a table
    void showStatistics(const std::string& name) const {
        auto it = stats_.find(name);
        if (it == stats_.end()) {
            std::cout << "  (Keine Statistiken fuer '" << name
                      << "' — ANALYZE TABLE " << name << " ausfuehren)\n\n";
            return;
        }
        const TableStats& ts = it->second;
        std::cout << "\n  Tabelle: " << ts.name
                  << "  (" << ts.rowCount << " Zeilen)\n\n";

        for (const auto& ckv : ts.cols) {
            const std::string&  col = ckv.first;
            const ColumnStats&  cs  = ckv.second;
            std::cout << "  Spalte: " << col << "\n";
            std::cout << "    Distinct  : " << cs.distinctCount << "\n";
            std::cout << "    Null      : " << cs.nullCount << "\n";
            std::cout << "    Min       : " << (cs.minVal.empty() ? "(null)" : cs.minVal) << "\n";
            std::cout << "    Max       : " << (cs.maxVal.empty() ? "(null)" : cs.maxVal) << "\n";

            if (!cs.mostCommonVals.empty()) {
                std::cout << "    Top-MCVs  :";
                // sort MCVs by frequency desc
                std::vector<std::pair<std::string,double>> mcv(
                    cs.mostCommonVals.begin(), cs.mostCommonVals.end());
                std::sort(mcv.begin(), mcv.end(),
                          [](const auto& a, const auto& b){ return a.second > b.second; });
                size_t shown = std::min<size_t>(5, mcv.size());
                for (size_t i = 0; i < shown; ++i)
                    std::cout << " '" << mcv[i].first << "' ("
                              << std::fixed << std::setprecision(1)
                              << (mcv[i].second * 100.0) << "%)";
                if (mcv.size() > shown) std::cout << " ...";
                std::cout << "\n";
            }

            if (!cs.histogram.empty()) {
                std::cout << "    Histogramm: ";
                for (size_t i = 0; i < cs.histogram.size(); ++i) {
                    if (i > 0) std::cout << ", ";
                    std::cout << "[<=" << cs.histogram[i].first
                              << ":" << cs.histogram[i].second << "]";
                }
                std::cout << "\n";
            }
            std::cout << "\n";
        }
    }

    // Estimate selectivity for col op val (returns fraction 0..1)
    double estimateSelectivity(const std::string& tbl,
                               const std::string& col,
                               const std::string& op,
                               const std::string& val) const {
        auto it = stats_.find(tbl);
        if (it == stats_.end()) return 0.1; // default: 10%

        auto ci = it->second.cols.find(col);
        if (ci == it->second.cols.end()) return 0.1;

        const ColumnStats& cs = ci->second;
        if (cs.distinctCount == 0) return 1.0;

        if (op == "=" || op == "IS") {
            // Check MCV first
            auto mv = cs.mostCommonVals.find(val);
            if (mv != cs.mostCommonVals.end()) return mv->second;
            return 1.0 / static_cast<double>(cs.distinctCount);
        }
        if (op == "!=" || op == "<>") {
            auto mv = cs.mostCommonVals.find(val);
            double eq = (mv != cs.mostCommonVals.end())
                ? mv->second
                : 1.0 / static_cast<double>(cs.distinctCount);
            return 1.0 - eq;
        }
        if (op == "<" || op == "<=") return 0.33;
        if (op == ">" || op == ">=") return 0.33;
        if (op == "LIKE")            return 0.05;
        if (op == "IS NULL")
            return cs.rowCount > 0
                ? static_cast<double>(cs.nullCount) / static_cast<double>(cs.rowCount)
                : 0.0;
        if (op == "IS NOT NULL")
            return cs.rowCount > 0
                ? 1.0 - static_cast<double>(cs.nullCount) / static_cast<double>(cs.rowCount)
                : 1.0;
        return 0.1;
    }

    // Estimate result row count given WHERE conditions
    size_t estimateRowCount(const std::string& tbl,
                            const std::vector<WhereCondition>& conds) const {
        auto it = stats_.find(tbl);
        if (it == stats_.end()) return 0;
        size_t total = it->second.rowCount;
        if (conds.empty()) return total;

        double sel = 1.0;
        for (const auto& wc : conds)
            sel *= estimateSelectivity(tbl, wc.col, wc.op, wc.val);

        return static_cast<size_t>(static_cast<double>(total) * sel + 0.5);
    }

    // Persist to file
    void saveStats(const std::string& path = "database.table_stats") const {
        std::ofstream f(path);
        if (!f) return;
        for (const auto& tv : stats_) {
            const TableStats& ts = tv.second;
            f << "TABLE\t" << ts.name << "\t" << ts.rowCount << "\n";
            for (const auto& ckv : ts.cols) {
                const ColumnStats& cs = ckv.second;
                f << "COL\t" << ts.name << "\t" << ckv.first
                  << "\t" << cs.distinctCount
                  << "\t" << cs.nullCount
                  << "\t" << cs.minVal
                  << "\t" << cs.maxVal << "\n";
                for (const auto& mv : cs.mostCommonVals)
                    f << "MCV\t" << ts.name << "\t" << ckv.first
                      << "\t" << mv.first << "\t" << mv.second << "\n";
                for (const auto& hb : cs.histogram)
                    f << "HIST\t" << ts.name << "\t" << ckv.first
                      << "\t" << hb.first << "\t" << hb.second << "\n";
            }
        }
    }

    void loadStats(const std::string& path = "database.table_stats") {
        std::ifstream f(path);
        if (!f) return;
        std::string line;
        while (std::getline(f, line)) {
            if (line.empty()) continue;
            std::istringstream ss(line);
            std::string type; std::getline(ss, type, '\t');
            if (type == "TABLE") {
                std::string name, cnt;
                std::getline(ss, name, '\t');
                std::getline(ss, cnt);
                if (!name.empty()) {
                    stats_[name].name = name;
                    try { stats_[name].rowCount = std::stoull(cnt); } catch (...) {}
                }
            } else if (type == "COL") {
                std::string tname, col, dc, nc, mn, mx;
                std::getline(ss, tname, '\t'); std::getline(ss, col, '\t');
                std::getline(ss, dc, '\t'); std::getline(ss, nc, '\t');
                std::getline(ss, mn, '\t'); std::getline(ss, mx);
                if (!tname.empty() && !col.empty()) {
                    auto& cs = stats_[tname].cols[col];
                    try { cs.distinctCount = std::stoull(dc); } catch (...) {}
                    try { cs.nullCount     = std::stoull(nc); } catch (...) {}
                    cs.minVal = mn; cs.maxVal = mx;
                }
            } else if (type == "MCV") {
                std::string tname, col, val, freq;
                std::getline(ss, tname, '\t'); std::getline(ss, col, '\t');
                std::getline(ss, val, '\t'); std::getline(ss, freq);
                if (!tname.empty() && !col.empty())
                    try { stats_[tname].cols[col].mostCommonVals[val] = std::stod(freq); }
                    catch (...) {}
            } else if (type == "HIST") {
                std::string tname, col, bnd, cnt;
                std::getline(ss, tname, '\t'); std::getline(ss, col, '\t');
                std::getline(ss, bnd, '\t'); std::getline(ss, cnt);
                if (!tname.empty() && !col.empty())
                    try { stats_[tname].cols[col].histogram.push_back({bnd, std::stoull(cnt)}); }
                    catch (...) {}
            }
        }
    }

    const std::map<std::string, TableStats>& all() const { return stats_; }

private:
    std::map<std::string, TableStats> stats_;
};

} // namespace milansql
