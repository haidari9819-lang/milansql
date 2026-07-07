#pragma once
#include <string>
#include <vector>
#include <cctype>
#include <sstream>
#include <iomanip>
#include "cost_model.hpp"
// ============================================================
// plan_selector.hpp — Optimizer-Roadmap Phase 2: Plan Selector
//
// Waehlt fuer eine Tabelle + WHERE-Bedingungen den guenstigsten
// Zugriffspfad (SeqScan / IndexScan / IndexRangeScan) auf Basis
// des Cost Models (cost_model.hpp) und der Statistiken aus
// Phase 1 (g_tableStats).
//
// Entscheidungslogik:
//   1. Kandidat SeqScan ist immer verfuegbar.
//   2. Fuer jede WHERE-Bedingung mit Index auf der Spalte und
//      sargierbarem Operator (=, <, <=, >, >=, BETWEEN, IN)
//      wird ein Index-Kandidat gekostet.
//   3. Guenstigster Plan gewinnt; bei Gleichstand Index
//      bevorzugen (weniger I/O bei warmem Cache).
//   4. Ohne ANALYZE-Stats: SeqScan als sicherer Fallback
//      (keine Selektivitaetsschaetzung moeglich).
//
// Join-Reihenfolge: Greedy — kleinste Tabelle (geschaetzte Rows
// NACH Filter) zuerst. Kein DP (das ist Phase 3).
// ============================================================

namespace milansql {

struct PlanChoice {
    std::string planType = "SeqScan";  // SeqScan | IndexScan | IndexRangeScan
    std::string table;
    std::string indexName;             // leer = kein Index
    std::string indexCol;              // Spalte des gewaehlten Index
    double estimatedRows = 0.0;
    double estimatedCost = 0.0;
    double selectivity   = 1.0;        // kombinierte Selektivitaet aller Conds
    std::string filter;                // z.B. "price > 100 AND stock > 0"
    bool fromStats = false;            // Entscheidung basiert auf ANALYZE-Stats
};

class PlanSelector {
public:
    // Index-Kandidat: Name + (erste) indizierte Spalte
    struct IndexCandidate {
        std::string indexName;
        std::string colName;
    };

    // Indizes aus dem TableStatsManager lesen (nach ANALYZE gefuellt).
    // Mehrspaltige Indizes: nur die fuehrende Spalte ist sargierbar.
    static std::vector<IndexCandidate> indexesFromStats(const std::string& tbl) {
        std::vector<IndexCandidate> out;
        const TableStats* ts = g_tableStats.getStats(tbl);
        if (!ts) return out;
        for (const auto& is : ts->indexes) {
            std::string lead = is.cols;
            auto comma = lead.find(',');
            if (comma != std::string::npos) lead = lead.substr(0, comma);
            while (!lead.empty() && lead.back() == ' ') lead.pop_back();
            out.push_back({is.indexName, lead});
        }
        return out;
    }

    // Menschenlesbarer Filter-Text aus den WHERE-Bedingungen
    static std::string filterText(const std::vector<WhereCondition>& conds,
                                  const std::string& logic = "AND") {
        std::string s;
        for (const auto& wc : conds) {
            if (!s.empty()) s += " " + logic + " ";
            std::string op = upper(wc.op);
            if (op == "BETWEEN" || op == "NOT BETWEEN") {
                s += wc.col + " " + wc.op + " " + wc.betweenLow
                   + " AND " + wc.betweenHigh;
            } else if (op == "IN" || op == "NOT IN") {
                s += wc.col + " " + wc.op + " (";
                for (size_t i = 0; i < wc.inList.size(); ++i) {
                    if (i) s += ", ";
                    s += wc.inList[i];
                }
                s += ")";
            } else if (op == "IS NULL" || op == "IS NOT NULL") {
                s += wc.col + " " + wc.op;
            } else {
                s += wc.col + " " + wc.op + " " + wc.val;
            }
        }
        return s;
    }

    // Zentrale Entscheidung: guenstigster Zugriffspfad fuer tbl.
    //   conds         WHERE-Bedingungen (AND-verknuepft angenommen)
    //   indexes       verfuegbare Indizes (z.B. via indexesFromStats
    //                 oder direkt aus der Engine)
    //   fallbackRows/-Width  Basisdaten falls keine Stats vorliegen
    static PlanChoice choose(const std::string& tbl,
                             const std::vector<WhereCondition>& conds,
                             const std::vector<IndexCandidate>& indexes,
                             double fallbackRows  = 1000.0,
                             double fallbackWidth = 64.0) {
        PlanChoice best;
        best.table  = tbl;
        best.filter = filterText(conds);

        CostModel::TableInput in =
            CostModel::tableInput(tbl, fallbackRows, fallbackWidth);
        best.fromStats = in.fromStats;

        // Kombinierte Selektivitaet aller Bedingungen
        double combinedSel = 1.0;
        for (const auto& wc : conds)
            combinedSel *= g_tableStats.conditionSelectivity(tbl, wc);
        double outRows = in.rows * combinedSel;

        // Kandidat 1: SeqScan (immer verfuegbar)
        CostEstimate seq = CostModel::seqScan(in, conds.size(), outRows);
        best.planType      = "SeqScan";
        best.estimatedRows = seq.rows;
        best.estimatedCost = seq.total;
        best.selectivity   = conds.empty() ? 1.0 : combinedSel;

        // Ohne ANALYZE-Stats: SeqScan als sicherer Fallback —
        // Index-Kosten waeren reine Spekulation.
        if (!in.fromStats) return best;

        // Kandidaten 2..n: ein Index-Zugriff pro sargierbarer Bedingung
        for (const auto& wc : conds) {
            std::string op = upper(wc.op);
            bool isEq    = (op == "=");
            bool isRange = (op == "<" || op == "<=" || op == ">" ||
                            op == ">=" || op == "BETWEEN");
            bool isIn    = (op == "IN");
            if (!isEq && !isRange && !isIn) continue;

            for (const auto& idx : indexes) {
                if (idx.colName != wc.col) continue;

                double condSel = g_tableStats.conditionSelectivity(tbl, wc);
                CostEstimate ie = CostModel::indexScan(in, condSel);
                // Rest-Filter auf den getroffenen Rows
                if (conds.size() > 1)
                    ie.total += ie.rows
                              * static_cast<double>(conds.size() - 1)
                              * g_costConsts.cpu_operator_cost;

                // Guenstiger — oder Gleichstand → Index bevorzugen
                if (ie.total < best.estimatedCost ||
                    (ie.total == best.estimatedCost && best.indexName.empty())) {
                    best.planType      = isEq ? "IndexScan"
                                              : (isIn ? "IndexScan"
                                                      : "IndexRangeScan");
                    best.indexName     = idx.indexName;
                    best.indexCol      = wc.col;
                    best.estimatedRows = outRows;
                    best.estimatedCost = ie.total;
                    best.selectivity   = conds.empty() ? 1.0 : combinedSel;
                }
            }
        }
        return best;
    }

    // ── Join-Reihenfolge (Greedy): kleinste Tabelle zuerst ─────
    struct JoinInput {
        std::string table;                  // Stats-Key (physischer Name)
        std::vector<WhereCondition> conds;  // Filter auf dieser Tabelle
        double fallbackRows = 1000.0;
    };

    // Liefert Indizes in Ausfuehrungsreihenfolge (kleinste est. Rows
    // nach Filter zuerst). Stabil bei Gleichstand (Eingabereihenfolge).
    static std::vector<size_t> joinOrder(const std::vector<JoinInput>& inputs) {
        std::vector<std::pair<double, size_t>> ranked;
        ranked.reserve(inputs.size());
        for (size_t i = 0; i < inputs.size(); ++i) {
            const auto& ji = inputs[i];
            double rows;
            if (g_tableStats.hasStats(ji.table)) {
                rows = static_cast<double>(
                    g_tableStats.estimateRowCount(ji.table, ji.conds));
            } else {
                rows = ji.fallbackRows;
                for (size_t k = 0; k < ji.conds.size(); ++k) rows *= 0.1;
            }
            ranked.push_back({rows, i});
        }
        std::stable_sort(ranked.begin(), ranked.end(),
            [](const std::pair<double, size_t>& a,
               const std::pair<double, size_t>& b) {
                return a.first < b.first;
            });
        std::vector<size_t> order;
        order.reserve(ranked.size());
        for (const auto& r : ranked) order.push_back(r.second);
        return order;
    }

    // ── EXPLAIN (FORMAT JSON): PlanChoice → JSON-Objekt ─────────
    static std::string toJson(const PlanChoice& pc) {
        std::ostringstream ss;
        ss << "{\"plan\":\"" << pc.planType << "\""
           << ",\"table\":\"" << jsonEscape(pc.table) << "\""
           << ",\"index\":";
        if (pc.indexName.empty()) ss << "null";
        else ss << "\"" << jsonEscape(pc.indexName) << "\"";
        ss << ",\"estimated_rows\":"
           << static_cast<long long>(pc.estimatedRows + 0.5)
           << ",\"estimated_cost\":"
           << std::fixed << std::setprecision(2) << pc.estimatedCost
           << ",\"selectivity\":"
           << std::setprecision(6) << pc.selectivity
           << ",\"filter\":";
        if (pc.filter.empty()) ss << "null";
        else ss << "\"" << jsonEscape(pc.filter) << "\"";
        ss << "}";
        return ss.str();
    }

    // ── Komplettes EXPLAIN-JSON fuer eine ParsedCommand ─────────
    // Template, damit beide Dispatch-Pfade (dispatch.hpp und
    // dispatch_result.hpp) dieselbe Logik nutzen, ohne dass
    // plan_selector.hpp parser.hpp/engine.hpp einziehen muss.
    // Einzeltabelle → ein Objekt; JOIN → Array (kleinste Tabelle
    // nach Filter zuerst, Greedy). Fuehrt die Query NICHT aus.
    template <typename EngineT, typename CmdT>
    static std::string explainJson(EngineT& engine, const CmdT& cmd) {
        struct TRef {
            std::string logical;   // Name wie in der Query
            std::string alias;     // optionaler Alias
            std::string statsKey;  // Key unter dem g_tableStats die Stats hat
            std::vector<WhereCondition> conds;
        };
        std::vector<TRef> refs;
        {
            TRef base;
            base.logical = cmd.tableName;
            base.alias   = cmd.tableAlias;
            refs.push_back(base);
            if (cmd.isJoin)
                for (const auto& jc : cmd.joinClauses) {
                    TRef r;
                    r.logical = jc.table;
                    r.alias   = jc.tableAlias;
                    refs.push_back(r);
                }
        }

        // Stats-Key aufloesen: erst logischer Name, dann physischer
        // (Tenant-Praefix u<id>_/Schema — resolveTableName der Engine)
        for (auto& r : refs) {
            r.statsKey = r.logical;
            if (!g_tableStats.hasStats(r.statsKey)) {
                try {
                    std::string phys = engine.resolveTableName(r.logical);
                    if (g_tableStats.hasStats(phys)) r.statsKey = phys;
                } catch (...) {}
            }
        }

        // WHERE-Bedingungen den Tabellen zuordnen:
        // "alias.col"/"table.col" → passende Tabelle (Praefix strippen),
        // unpraefixiert → Basistabelle.
        for (const auto& wc : cmd.whereConds) {
            std::string col = wc.col;
            size_t dot = col.find('.');
            size_t target = 0;
            if (dot != std::string::npos) {
                std::string pre = col.substr(0, dot);
                for (size_t i = 0; i < refs.size(); ++i)
                    if (pre == refs[i].alias || pre == refs[i].logical) {
                        target = i;
                        col = col.substr(dot + 1);
                        break;
                    }
            }
            WhereCondition stripped = wc;
            stripped.col = col;
            refs[target].conds.push_back(stripped);
        }

        // Fallback-Rows/-Breite aus der Engine (falls keine Stats)
        auto fallbackFor = [&](const TRef& r, double& rows, double& width) {
            rows = 1000.0; width = 64.0;
            try { rows = static_cast<double>(engine.countRows(r.logical)); }
            catch (...) {}
            try {
                width = static_cast<double>(
                    engine.tableColumns(r.logical).size()) * 8.0;
            } catch (...) {}
        };

        auto planFor = [&](const TRef& r) {
            double fbRows, fbWidth;
            fallbackFor(r, fbRows, fbWidth);
            PlanChoice pc = choose(r.statsKey, r.conds,
                                   indexesFromStats(r.statsKey),
                                   fbRows, fbWidth);
            pc.table = r.logical;  // logischen Namen berichten
            return pc;
        };

        if (refs.size() == 1) return toJson(planFor(refs[0]));

        // JOIN: Greedy-Reihenfolge (kleinste est. Rows nach Filter zuerst)
        std::vector<JoinInput> ji;
        for (const auto& r : refs) {
            JoinInput j;
            j.table = r.statsKey;
            j.conds = r.conds;
            double fbRows, fbWidth;
            fallbackFor(r, fbRows, fbWidth);
            j.fallbackRows = fbRows;
            ji.push_back(j);
        }
        std::vector<size_t> order = joinOrder(ji);
        std::string out = "[";
        for (size_t k = 0; k < order.size(); ++k) {
            if (k) out += ",";
            out += toJson(planFor(refs[order[k]]));
        }
        out += "]";
        return out;
    }

private:
    static std::string jsonEscape(const std::string& s) {
        std::string r;
        r.reserve(s.size());
        for (char c : s) {
            if (c == '"' || c == '\\') { r += '\\'; r += c; }
            else if (c == '\n') r += "\\n";
            else if (c == '\t') r += "\\t";
            else if (c == '\r') r += "\\r";
            else r += c;
        }
        return r;
    }

    static std::string upper(const std::string& s) {
        std::string r = s;
        for (auto& c : r)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return r;
    }
};

// ── Engine-Integration (Optimizer Phase 2) ──────────────────────
// Installiert den kostenbasierten Advisor in den Index-Pfad der
// Engine (selectWhere, Phase-173-Heuristik). Konservativ:
//   - keine ANALYZE-Stats            → true  (altes Verhalten)
//   - Index in den Stats unbekannt   → true  (Stats veraltet,
//     z.B. Index nach ANALYZE erstellt — nicht spekulativ ablehnen)
//   - sonst: PlanSelector entscheidet; SeqScan billiger → false
inline const bool g_planAdvisorInstalled = []() {
    g_indexPathAdvisor = [](const std::string& tbl,
                            const std::vector<WhereCondition>& conds) -> bool {
        const TableStats* ts = g_tableStats.getStats(tbl);
        if (!ts || conds.empty()) return true;
        bool indexKnown = false;
        auto candidates = PlanSelector::indexesFromStats(tbl);
        for (const auto& ic : candidates)
            if (ic.colName == conds[0].col) { indexKnown = true; break; }
        if (!indexKnown) return true;
        PlanChoice pc = PlanSelector::choose(tbl, conds, candidates);
        return pc.planType != "SeqScan";
    };
    return true;
}();

} // namespace milansql
