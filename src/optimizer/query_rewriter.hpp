#pragma once
// ============================================================
// query_rewriter.hpp — Phase 82: Automatic Query Rewriting
// Rule-based query transformations applied before execution.
// ============================================================

#include <string>
#include <vector>

#include "../engine/engine.hpp"
#include "../parser/parser.hpp"

namespace milansql {

class QueryRewriter {
public:
    bool isEnabled() const { return enabled_; }
    void setEnabled(bool v) { enabled_ = v; }
    const std::vector<std::string>& notes() const { return notes_; }
    const std::string& originalSql() const { return originalSql_; }

    // Rewrites cmd in-place. Returns true if any transformation was applied.
    bool rewrite(ParsedCommand& cmd) {
        notes_.clear();
        originalSql_ = cmd.raw;
        if (!enabled_ || cmd.type != CommandType::SELECT) return false;

        bool changed = false;
        changed |= removeAlwaysTrueConditions(cmd);
        changed |= removeRedundantConditions(cmd);
        noteSubqueries(cmd);
        return changed;
    }

private:
    bool enabled_ = false;
    std::vector<std::string> notes_;
    std::string originalSql_;

    // B) WHERE 1=1 entfernen (immer wahre Bedingung)
    bool removeAlwaysTrueConditions(ParsedCommand& cmd) {
        bool changed = false;
        std::vector<WhereCondition> kept;
        for (const auto& wc : cmd.whereConds) {
            bool alwaysTrue = ((wc.col == "1" || wc.col == "'1'") &&
                               wc.op == "=" &&
                               (wc.val == "1" || wc.val == "'1'"));
            if (alwaysTrue) {
                notes_.push_back("WHERE 1=1 entfernt (Bedingung immer wahr)");
                changed = true;
            } else {
                kept.push_back(wc);
            }
        }
        if (changed) {
            cmd.whereConds = std::move(kept);
            if (cmd.whereConds.empty()) {
                cmd.whereColumn.clear();
                cmd.whereValue.clear();
            }
        }
        return changed;
    }

    // C) Konstanten-Faltung: redundante numerische WHERE-Bedingungen entfernen
    // WHERE col > 100 AND col > 50  =>  WHERE col > 100
    // WHERE col < 50  AND col < 100 =>  WHERE col < 50
    bool removeRedundantConditions(ParsedCommand& cmd) {
        if (cmd.whereConds.size() < 2 || cmd.whereLogic != "AND") return false;
        bool changed = false;
        std::vector<bool> redundant(cmd.whereConds.size(), false);

        for (size_t i = 0; i < cmd.whereConds.size(); ++i) {
            if (redundant[i]) continue;
            const auto& ci = cmd.whereConds[i];
            if (ci.op != ">" && ci.op != ">=" && ci.op != "<" && ci.op != "<=") continue;
            double vi = 0;
            try { vi = std::stod(ci.val); } catch (...) { continue; }

            for (size_t j = 0; j < cmd.whereConds.size(); ++j) {
                if (i == j || redundant[j]) continue;
                const auto& cj = cmd.whereConds[j];
                if (ci.col != cj.col || ci.op != cj.op) continue;
                double vj = 0;
                try { vj = std::stod(cj.val); } catch (...) { continue; }

                // For > / >= : more restrictive = larger threshold
                if ((ci.op == ">" || ci.op == ">=") && vi > vj) {
                    redundant[j] = true; changed = true;
                }
                // For < / <= : more restrictive = smaller threshold
                if ((ci.op == "<" || ci.op == "<=") && vi < vj) {
                    redundant[j] = true; changed = true;
                }
            }
        }

        if (changed) {
            std::vector<WhereCondition> kept;
            for (size_t i = 0; i < cmd.whereConds.size(); ++i) {
                if (redundant[i]) {
                    notes_.push_back("Redundante Bedingung entfernt: " +
                        cmd.whereConds[i].col + " " + cmd.whereConds[i].op +
                        " " + cmd.whereConds[i].val);
                } else {
                    kept.push_back(cmd.whereConds[i]);
                }
            }
            cmd.whereConds = std::move(kept);
        }
        return changed;
    }

    // A) Subquery-to-JOIN Hinweis: IN (SELECT ...) als JOIN-Kandidat notieren.
    // Echte Ausfuehrung laeuft bereits ueber inList-Aufloesung (effizient).
    void noteSubqueries(ParsedCommand& cmd) {
        for (const auto& sq : cmd.subqueries) {
            notes_.push_back(
                "IN (SELECT " + sq.subCol + " FROM " + sq.subTable + ")"
                " als JOIN-Kandidat erkannt — inList-Aufloesung aktiv");
        }
    }
};

} // namespace milansql
