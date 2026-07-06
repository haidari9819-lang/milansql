#pragma once
#include <cmath>
#include <ctime>
#include <cctype>
#include <functional>
// ============================================================
// table_stats.hpp — Phase 86: Statistik-basierter Query Planner
// Echte Spaltenstatistiken: Kardinalität, Min/Max, MCVs,
// Histogramm, Selektivitätsschätzung.
//
// Optimizer-Roadmap Phase 1 (Statistics Collection):
//   - MVCC-korrekt: nur lebende Rows (xmax == 0) werden gezählt
//   - NULL-Erkennung: leerer String UND Literal "NULL"
//   - Numerische Min/Max bei numerischen Spaltentypen
//   - Sampling bei großen Tabellen (siehe SAMPLING-STRATEGIE unten)
//   - Index-Statistiken: distinct keys + Selektivität pro Index
//   - Persistenz: database.table_stats (TSV, abwärtskompatibel)
//
// SAMPLING-STRATEGIE (dokumentiert für Phase 2):
//   - Tabellen mit <= STATS_FULL_SCAN_LIMIT (100k) lebenden Rows
//     werden vollständig gescannt → exakte Statistiken.
//   - Darüber: systematisches Sampling (jede k-te lebende Row,
//     deterministischer Startoffset aus hash(tablename) ^ rowcount,
//     damit ANALYZE reproduzierbar bleibt). Sample-Größe:
//     n = clamp(N/10, 100000, 1000000) — mindestens 10%, nie unter
//     100k, gedeckelt bei 1M Rows, damit ANALYZE auch bei vielen
//     Millionen Rows schnell bleibt.
//   - Hochrechnung distinct: GEE-Schätzer (Charikar et al.):
//     D̂ = sqrt(N/n)·f1 + (d − f1), geklemmt auf [d, N].
//     (d = distinct im Sample, f1 = Werte mit Häufigkeit 1)
//   - null_frac / MCV-Frequenzen: direkte Sample-Anteile (unbiased)
//   - Min/Max aus Sample: echte Extreme können fehlen — bewusster
//     Phase-1-Kompromiss, für Range-Selektivität ausreichend.
//   - Caveat systematisches Sampling: setzt voraus, dass die
//     physische Row-Reihenfolge kein periodisches Muster mit
//     Periode ≈ k hat; bei Bedarf in Phase 2 auf Bernoulli umstellbar.
//
// FÜR PHASE 2 (Cost Model) VORBEREITETE FELDER:
//   ColumnStats: distinctCount (n_distinct), nullFrac, avgWidth
//     (Bytes, für Row-Width/IO-Kosten), minVal/maxVal + isNumeric
//     (Range-Interpolation), mostCommonVals (Equality-Selektivität),
//     histogram (Equi-Depth, Range-Selektivität)
//   IndexStats: distinctKeys, selectivity (= 1/distinctKeys,
//     erwartete Trefferquote eines Equality-Lookups), avgRowsPerKey
//   TableStats: rowCount (live), deadRowCount (Vacuum-Kostensignal),
//     sampled/sampledRows (Konfidenz der Schätzungen),
//     lastAnalyzed (Staleness-Check für Auto-ANALYZE, Phase 1.5)
// ============================================================

namespace milansql {

// ── Sampling-Parameter ─────────────────────────────────────────
static constexpr size_t STATS_FULL_SCAN_LIMIT = 100000;  // bis hier: exakt
static constexpr size_t STATS_SAMPLE_MIN      = 100000;  // Sample-Untergrenze
static constexpr size_t STATS_SAMPLE_MAX      = 1000000; // Sample-Obergrenze

// ── Spalten-Statistiken ────────────────────────────────────────
struct ColumnStats {
    size_t      rowCount     = 0;   // lebende Rows der Tabelle
    size_t      distinctCount = 0;  // (hochgerechnete) Kardinalität
    size_t      nullCount     = 0;  // (hochgerechnete) NULL-Anzahl
    double      nullFrac      = 0.0; // NULL-Anteil 0..1 (Phase 2: Cost Model)
    double      avgWidth      = 0.0; // Ø Bytes je Nicht-NULL-Wert (Phase 2)
    bool        isNumeric     = false; // Min/Max numerisch vergleichbar
    std::string minVal;
    std::string maxVal;
    // most common values: value → frequency (0..1)
    std::map<std::string, double> mostCommonVals;   // top 10
    // histogram: bucket upper bound → row count in bucket
    std::vector<std::pair<std::string, size_t>> histogram; // 10 buckets
};

// ── Index-Statistiken (Phase 1: Optimizer-Roadmap) ────────────
struct IndexStats {
    std::string indexName;
    std::string cols;             // ", "-getrennte Spaltenliste
    size_t      distinctKeys = 0; // (hochgerechnete) distinct Schlüssel
    double      selectivity  = 1.0; // 1/distinctKeys — Equality-Lookup-Anteil
    double      avgRowsPerKey = 0.0; // rowCount/distinctKeys (Phase 2)
};

// ── Tabellen-Statistiken ───────────────────────────────────────
struct TableStats {
    std::string name;
    size_t      rowCount = 0;      // lebende Rows (MVCC: xmax == 0)
    size_t      deadRowCount = 0;  // tote Versionen (Phase 2: Vacuum-Signal)
    bool        sampled = false;   // true = Werte sind Hochrechnungen
    size_t      sampledRows = 0;   // tatsächlich gelesene Rows
    std::string lastAnalyzed;      // ISO-Zeitstempel des letzten ANALYZE
    std::map<std::string, ColumnStats> cols;
    std::vector<IndexStats> indexes;
};

// ── Manager ───────────────────────────────────────────────────
class TableStatsManager {
public:
    TableStatsManager()  { loadStats(); }
    ~TableStatsManager() { saveStats(); }

    static bool isNullVal(const std::string& v) {
        return v.empty() || v == "NULL";
    }

    static bool isNumericType(const std::string& type) {
        std::string t;
        for (char c : type) {
            if (c == '(') break;  // DECIMAL(10,2) → DECIMAL
            t += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
        return t == "INT" || t == "INTEGER" || t == "BIGINT" || t == "SMALLINT" ||
               t == "FLOAT" || t == "DOUBLE" || t == "REAL" || t == "DECIMAL" ||
               t == "NUMERIC" || t == "SERIAL" || t == "BIGSERIAL";
    }

    // Numerisch wenn möglich, sonst lexikographisch
    static bool lessVal(const std::string& a, const std::string& b, bool numeric) {
        if (numeric) {
            try { return std::stod(a) < std::stod(b); } catch (...) {}
        }
        return a < b;
    }

    // GEE-Schätzer: Hochrechnung distinct count vom Sample auf N
    static size_t estimateDistinct(size_t d, size_t f1, size_t n, size_t N) {
        if (n == 0 || d == 0) return 0;
        if (n >= N) return d;  // Vollscan → exakt
        double est = std::sqrt(static_cast<double>(N) / static_cast<double>(n))
                       * static_cast<double>(f1)
                   + static_cast<double>(d - f1);
        if (est < static_cast<double>(d)) est = static_cast<double>(d);
        if (est > static_cast<double>(N)) est = static_cast<double>(N);
        return static_cast<size_t>(est + 0.5);
    }

    // Analyze a single table
    void analyzeTable(const std::string& name, const Table& tbl) {
        TableStats ts;
        ts.name = name;

        const auto& cols = tbl.columns();
        const auto& rows = tbl.rows();

        // ── MVCC: lebende Row-Indizes einsammeln ──
        std::vector<size_t> live;
        live.reserve(rows.size());
        for (size_t ri = 0; ri < rows.size(); ++ri) {
            if (rows[ri].xmax == 0) live.push_back(ri);
            else ++ts.deadRowCount;
        }
        ts.rowCount = live.size();
        const size_t N = live.size();

        // ── Sampling-Entscheidung (siehe Header-Doku) ──
        std::vector<size_t> sample;
        if (N <= STATS_FULL_SCAN_LIMIT) {
            sample = std::move(live);
        } else {
            size_t target = N / 10;
            if (target < STATS_SAMPLE_MIN) target = STATS_SAMPLE_MIN;
            if (target > STATS_SAMPLE_MAX) target = STATS_SAMPLE_MAX;
            size_t step = N / target;
            if (step < 1) step = 1;
            // deterministischer Startoffset → reproduzierbares ANALYZE
            size_t start = (std::hash<std::string>{}(name) ^ N) % step;
            sample.reserve(target + 1);
            for (size_t i = start; i < N; i += step)
                sample.push_back(live[i]);
            ts.sampled = true;
        }
        ts.sampledRows = sample.size();
        const size_t n = sample.size();

        {   // Zeitstempel
            std::time_t t = std::time(nullptr);
            char buf[32];
            if (std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S",
                              std::localtime(&t)))
                ts.lastAnalyzed = buf;
        }

        // ── Spaltenstatistiken über das Sample ──
        for (size_t ci = 0; ci < cols.size(); ++ci) {
            ColumnStats cs;
            cs.rowCount  = ts.rowCount;
            cs.isNumeric = isNumericType(cols[ci].type);

            std::map<std::string, size_t> freq;
            size_t sampleNulls = 0;
            size_t widthSum = 0;
            bool hasMin = false;

            for (size_t ri : sample) {
                const auto& row = rows[ri];
                const std::string& v =
                    (ci < row.values.size()) ? row.values[ci] : "";
                if (isNullVal(v)) { ++sampleNulls; continue; }
                freq[v]++;
                widthSum += v.size();
                if (!hasMin) {
                    cs.minVal = v; cs.maxVal = v; hasMin = true;
                } else {
                    if (lessVal(v, cs.minVal, cs.isNumeric)) cs.minVal = v;
                    if (lessVal(cs.maxVal, v, cs.isNumeric)) cs.maxVal = v;
                }
            }

            size_t nonNull = n - sampleNulls;
            cs.nullFrac = (n > 0)
                ? static_cast<double>(sampleNulls) / static_cast<double>(n)
                : 0.0;
            cs.nullCount = ts.sampled
                ? static_cast<size_t>(cs.nullFrac * static_cast<double>(N) + 0.5)
                : sampleNulls;
            cs.avgWidth = (nonNull > 0)
                ? static_cast<double>(widthSum) / static_cast<double>(nonNull)
                : 0.0;

            // distinct: exakt bei Vollscan, sonst GEE-Hochrechnung
            size_t f1 = 0;
            for (const auto& kv : freq) if (kv.second == 1) ++f1;
            size_t nonNullTotal = ts.sampled
                ? static_cast<size_t>((1.0 - cs.nullFrac)
                                      * static_cast<double>(N) + 0.5)
                : nonNull;
            cs.distinctCount = ts.sampled
                ? estimateDistinct(freq.size(), f1, nonNull, nonNullTotal)
                : freq.size();

            // top-10 MCVs by frequency (Frequenzen relativ zu allen Rows)
            std::vector<std::pair<std::string, size_t>> sorted(freq.begin(), freq.end());
            std::sort(sorted.begin(), sorted.end(),
                      [](const auto& a, const auto& b){ return a.second > b.second; });
            size_t top = std::min<size_t>(10, sorted.size());
            for (size_t i = 0; i < top; ++i) {
                double rel = n > 0
                    ? static_cast<double>(sorted[i].second) / static_cast<double>(n)
                    : 0.0;
                cs.mostCommonVals[sorted[i].first] = rel;
            }

            // 10-bucket equi-depth histogram over sorted distinct values
            if (!sorted.empty()) {
                std::vector<std::string> vals;
                vals.reserve(freq.size());
                for (const auto& kv : freq) vals.push_back(kv.first);
                bool num = cs.isNumeric;
                std::sort(vals.begin(), vals.end(),
                          [num](const std::string& a, const std::string& b){
                              return lessVal(a, b, num);
                          });
                size_t buckets = std::min<size_t>(10, vals.size());
                size_t perBucket = vals.size() / buckets;
                for (size_t b = 0; b < buckets; ++b) {
                    size_t idx = std::min((b + 1) * perBucket, vals.size()) - 1;
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

        // ── Index-Statistiken: distinct Keys je Index über das Sample ──
        {
            // Spaltenname → Index für schnellen Zugriff
            std::map<std::string, size_t> colIdx;
            for (size_t ci = 0; ci < cols.size(); ++ci)
                colIdx[cols[ci].name] = ci;

            for (const auto& info : tbl.getIndexes()) {
                IndexStats is;
                is.indexName = info.indexName;
                is.cols      = info.colName;

                // ", "-getrennte Spaltenliste auflösen
                std::vector<size_t> parts;
                {
                    std::string cur;
                    std::istringstream ss(info.colName);
                    while (std::getline(ss, cur, ',')) {
                        while (!cur.empty() && cur.front() == ' ') cur.erase(0, 1);
                        while (!cur.empty() && cur.back() == ' ')  cur.pop_back();
                        auto it = colIdx.find(cur);
                        if (it != colIdx.end()) parts.push_back(it->second);
                    }
                }
                if (parts.empty()) { ts.indexes.push_back(is); continue; }

                std::map<std::string, size_t> keyFreq;
                for (size_t ri : sample) {
                    const auto& row = rows[ri];
                    std::string key;
                    for (size_t pi : parts) {
                        key += (pi < row.values.size()) ? row.values[pi] : "";
                        key += '\x1f';  // Feldtrenner gegen Kollisionen
                    }
                    keyFreq[key]++;
                }
                size_t f1 = 0;
                for (const auto& kv : keyFreq) if (kv.second == 1) ++f1;
                is.distinctKeys = ts.sampled
                    ? estimateDistinct(keyFreq.size(), f1, n, N)
                    : keyFreq.size();
                if (is.distinctKeys > 0) {
                    is.selectivity   = 1.0 / static_cast<double>(is.distinctKeys);
                    is.avgRowsPerKey = static_cast<double>(N)
                                     / static_cast<double>(is.distinctKeys);
                }
                ts.indexes.push_back(std::move(is));
            }
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

    // Direkter Zugriff für Tests + Phase 2 (Cost Model)
    const TableStats* getStats(const std::string& name) const {
        auto it = stats_.find(name);
        return it != stats_.end() ? &it->second : nullptr;
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
                  << "  (" << ts.rowCount << " Zeilen";
        if (ts.deadRowCount > 0)
            std::cout << ", " << ts.deadRowCount << " tot";
        if (ts.sampled)
            std::cout << ", Sample: " << ts.sampledRows << " Rows";
        std::cout << ")";
        if (!ts.lastAnalyzed.empty())
            std::cout << "  [analyzed: " << ts.lastAnalyzed << "]";
        std::cout << "\n\n";

        for (const auto& ckv : ts.cols) {
            const std::string&  col = ckv.first;
            const ColumnStats&  cs  = ckv.second;
            std::cout << "  Spalte: " << col << "\n";
            std::cout << "    Distinct  : " << cs.distinctCount
                      << (ts.sampled ? " (geschaetzt)" : "") << "\n";
            std::cout << "    Null      : " << cs.nullCount
                      << " (" << std::fixed << std::setprecision(1)
                      << (cs.nullFrac * 100.0) << "%)\n";
            std::cout << "    AvgWidth  : " << std::fixed << std::setprecision(1)
                      << cs.avgWidth << " B\n";
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

        if (!ts.indexes.empty()) {
            std::cout << "  Indizes:\n";
            for (const auto& is : ts.indexes) {
                std::cout << "    " << is.indexName << " (" << is.cols << "): "
                          << is.distinctKeys << " Keys, Selektivitaet "
                          << std::scientific << std::setprecision(3)
                          << is.selectivity << std::fixed
                          << ", ~" << std::setprecision(1)
                          << is.avgRowsPerKey << " Rows/Key\n";
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

    // Persist to file (TSV; neue Record-Typen META/IDX — alte Loader
    // ignorieren unbekannte Typen, neuer Loader liest alte Dateien)
    void saveStats(const std::string& path = "database.table_stats") const {
        std::ofstream f(path);
        if (!f) return;
        for (const auto& tv : stats_) {
            const TableStats& ts = tv.second;
            f << "TABLE\t" << ts.name << "\t" << ts.rowCount << "\n";
            f << "META\t" << ts.name << "\t" << ts.deadRowCount
              << "\t" << (ts.sampled ? 1 : 0)
              << "\t" << ts.sampledRows
              << "\t" << ts.lastAnalyzed << "\n";
            for (const auto& ckv : ts.cols) {
                const ColumnStats& cs = ckv.second;
                f << "COL\t" << ts.name << "\t" << ckv.first
                  << "\t" << cs.distinctCount
                  << "\t" << cs.nullCount
                  << "\t" << cs.minVal
                  << "\t" << cs.maxVal
                  << "\t" << cs.nullFrac
                  << "\t" << cs.avgWidth
                  << "\t" << (cs.isNumeric ? 1 : 0) << "\n";
                for (const auto& mv : cs.mostCommonVals)
                    f << "MCV\t" << ts.name << "\t" << ckv.first
                      << "\t" << mv.first << "\t" << mv.second << "\n";
                for (const auto& hb : cs.histogram)
                    f << "HIST\t" << ts.name << "\t" << ckv.first
                      << "\t" << hb.first << "\t" << hb.second << "\n";
            }
            for (const auto& is : ts.indexes)
                f << "IDX\t" << ts.name << "\t" << is.indexName
                  << "\t" << is.cols
                  << "\t" << is.distinctKeys
                  << "\t" << is.selectivity
                  << "\t" << is.avgRowsPerKey << "\n";
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
            } else if (type == "META") {
                std::string tname, dead, smp, srows, when;
                std::getline(ss, tname, '\t'); std::getline(ss, dead, '\t');
                std::getline(ss, smp, '\t');   std::getline(ss, srows, '\t');
                std::getline(ss, when);
                if (!tname.empty()) {
                    auto& ts = stats_[tname];
                    try { ts.deadRowCount = std::stoull(dead); } catch (...) {}
                    ts.sampled = (smp == "1");
                    try { ts.sampledRows = std::stoull(srows); } catch (...) {}
                    ts.lastAnalyzed = when;
                }
            } else if (type == "COL") {
                std::string tname, col, dc, nc, mn, mx, nf, aw, num;
                std::getline(ss, tname, '\t'); std::getline(ss, col, '\t');
                std::getline(ss, dc, '\t'); std::getline(ss, nc, '\t');
                std::getline(ss, mn, '\t'); std::getline(ss, mx, '\t');
                // optionale neue Felder (Dateien aus alten Versionen haben sie nicht)
                std::getline(ss, nf, '\t'); std::getline(ss, aw, '\t');
                std::getline(ss, num);
                if (!tname.empty() && !col.empty()) {
                    auto& cs = stats_[tname].cols[col];
                    try { cs.distinctCount = std::stoull(dc); } catch (...) {}
                    try { cs.nullCount     = std::stoull(nc); } catch (...) {}
                    cs.minVal = mn; cs.maxVal = mx;
                    try { if (!nf.empty()) cs.nullFrac = std::stod(nf); } catch (...) {}
                    try { if (!aw.empty()) cs.avgWidth = std::stod(aw); } catch (...) {}
                    cs.isNumeric = (num == "1");
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
            } else if (type == "IDX") {
                std::string tname, iname, cols, dk, sel, arpk;
                std::getline(ss, tname, '\t'); std::getline(ss, iname, '\t');
                std::getline(ss, cols, '\t');  std::getline(ss, dk, '\t');
                std::getline(ss, sel, '\t');   std::getline(ss, arpk);
                if (!tname.empty() && !iname.empty()) {
                    IndexStats is;
                    is.indexName = iname; is.cols = cols;
                    try { is.distinctKeys = std::stoull(dk); } catch (...) {}
                    try { is.selectivity  = std::stod(sel); } catch (...) {}
                    try { is.avgRowsPerKey = std::stod(arpk); } catch (...) {}
                    stats_[tname].indexes.push_back(std::move(is));
                }
            }
        }
    }

    const std::map<std::string, TableStats>& all() const { return stats_; }

private:
    std::map<std::string, TableStats> stats_;
};

// ── Single Source of Truth für Optimizer-Statistiken ──────────
// Konsolidierung (Optimizer Phase 1): genutzt von dispatch.hpp
// (REPL/HTTP-stdout-Pfad) UND dispatch_result.hpp (QueryResult-Pfad).
// Der frühere Fake-Store (stats/statistics_manager.hpp) hält nur noch
// CREATE-STATISTICS-Definitionen, keine Zahlen.
inline TableStatsManager g_tableStats;

} // namespace milansql
