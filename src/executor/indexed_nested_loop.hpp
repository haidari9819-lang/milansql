#pragma once
// ============================================================
// indexed_nested_loop.hpp — Optimizer Phase 3 (Block 3):
// Indexed Nested Loop Join. Included inside namespace milansql
// in engine.hpp (after hash_join.hpp). Do NOT add a namespace
// wrapper and do NOT include system headers already included
// by engine.hpp.
//
// Pro outer-Row ein Index-Lookup auf der Join-Spalte der
// inneren (rechten) Tabelle: O(n · log m) statt O(n + m)
// Build/Probe — lohnt sich nur bei kleiner outer-Seite
// (Entscheidung: JoinEnumerator::chooseJoinMethod).
//
// Nur INNER und LEFT (bei RIGHT/FULL waere die outer-Seite
// die rechte Tabelle → HashJoin bleibt zustaendig).
// Semantik identisch zu HashJoin::execute: keine Filterung
// toter (xmax != 0) Rows; Gleichheit wird nach dem Index-
// Lookup nachgeprueft (Schutz vor veralteten Index-Eintraegen).
// ============================================================

struct IndexedNestedLoop {
    // current      — akkumulierte linke Seite (qualifizierte Spalten)
    // right        — rohe rechte Tabelle (mit Index auf rightColBare)
    // leftCI       — Join-Spaltenindex in `current`
    // rightCI      — Join-Spaltenindex in `right`
    // rightColBare — Spaltenname fuer right.indexSearch (ohne Prefix)
    // joinType     — "INNER" | "LEFT"
    // newCols      — Spaltenschema des Ergebnisses
    static Table execute(const Table& current,
                         const Table& right,
                         size_t leftCI,
                         size_t rightCI,
                         const std::string& rightColBare,
                         const std::string& joinType,
                         const std::vector<Column>& newCols)
    {
        Table next("", newCols);
        const size_t leftWidth  = current.columns().size();
        const size_t rightWidth = right.columns().size();
        const auto&  rrows      = right.rows();

        for (const auto& lrow : current.rows()) {
            const std::string& lval =
                leftCI < lrow.values.size() ? lrow.values[leftCI] : "";
            bool matched = false;

            for (size_t ri : right.indexSearch(rightColBare, lval)) {
                if (ri >= rrows.size()) continue;
                // Recheck: Index kann veraltete Eintraege enthalten
                const std::string& rval =
                    rightCI < rrows[ri].values.size()
                        ? rrows[ri].values[rightCI] : "";
                if (rval != lval) continue;

                std::vector<std::string> vals;
                vals.reserve(leftWidth + rrows[ri].values.size());
                vals.insert(vals.end(),
                            lrow.values.begin(), lrow.values.end());
                vals.insert(vals.end(),
                            rrows[ri].values.begin(), rrows[ri].values.end());
                next.insert(Row(vals));
                matched = true;
            }

            if (!matched && joinType == "LEFT") {
                std::vector<std::string> vals;
                vals.reserve(leftWidth + rightWidth);
                vals.insert(vals.end(), lrow.values.begin(), lrow.values.end());
                for (size_t k = 0; k < rightWidth; ++k) vals.push_back("NULL");
                next.insert(Row(vals));
            }
        }
        return next;
    }
};
