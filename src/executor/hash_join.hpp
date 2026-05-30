#pragma once
// ============================================================
// hash_join.hpp — Phase 83: Hash Join Algorithm
// Included inside namespace milansql in engine.hpp (after Table
// class). Do NOT add a namespace milansql wrapper here and do NOT
// include system headers already included by engine.hpp.
//
// Complexity: O(n + m)  vs O(n × m) for Nested Loop.
//
// Build phase : smaller/right table  → unordered_map<key, row-indices>
// Probe phase : left rows            → hash-map lookup, combine matches
//
// Supports INNER, LEFT, RIGHT, FULL join types.
// ============================================================

#include <unordered_map>

struct HashJoin {
    // Execute a hash join between `current` (left side, already qualified
    // column names) and `right` (raw right table).
    //
    // Parameters:
    //   current   — accumulated left-side table (column names are qualified)
    //   right     — raw right table from the engine (column names unqualified)
    //   leftCI    — join-column index inside `current`
    //   rightCI   — join-column index inside `right`
    //   joinType  — "INNER" | "LEFT" | "RIGHT" | "FULL"
    //   newCols   — column schema for the result table
    //
    // Returns a new Table with the joined rows.
    static Table execute(const Table& current,
                         const Table& right,
                         size_t leftCI,
                         size_t rightCI,
                         const std::string& joinType,
                         const std::vector<Column>& newCols)
    {
        Table next("", newCols);
        const size_t leftWidth  = current.columns().size();
        const size_t rightWidth = right.columns().size();
        const auto&  lrows      = current.rows();
        const auto&  rrows      = right.rows();

        if (joinType == "INNER" || joinType == "LEFT") {
            // ── Build phase: hash map keyed on right join column ──────
            std::unordered_map<std::string, std::vector<size_t>> hashMap;
            hashMap.reserve(rrows.size());
            for (size_t ri = 0; ri < rrows.size(); ++ri) {
                const std::string& key =
                    rightCI < rrows[ri].values.size() ? rrows[ri].values[rightCI] : "";
                hashMap[key].push_back(ri);
            }

            // ── Probe phase: for each left row, look up the hash map ──
            for (const auto& lrow : lrows) {
                const std::string& lval =
                    leftCI < lrow.values.size() ? lrow.values[leftCI] : "";
                auto it = hashMap.find(lval);

                if (it != hashMap.end()) {
                    for (size_t ri : it->second) {
                        std::vector<std::string> vals;
                        vals.reserve(leftWidth + rrows[ri].values.size());
                        vals.insert(vals.end(),
                                    lrow.values.begin(), lrow.values.end());
                        vals.insert(vals.end(),
                                    rrows[ri].values.begin(), rrows[ri].values.end());
                        next.insert(Row(vals));
                    }
                } else if (joinType == "LEFT") {
                    // LEFT JOIN: unmatched left row → NULL-pad right side
                    std::vector<std::string> vals;
                    vals.reserve(leftWidth + rightWidth);
                    vals.insert(vals.end(), lrow.values.begin(), lrow.values.end());
                    for (size_t k = 0; k < rightWidth; ++k) vals.push_back("NULL");
                    next.insert(Row(vals));
                }
            }

        } else if (joinType == "RIGHT") {
            // ── Build on left side, probe with right ─────────────────
            std::unordered_map<std::string, std::vector<size_t>> hashMap;
            hashMap.reserve(lrows.size());
            for (size_t li = 0; li < lrows.size(); ++li) {
                const std::string& key =
                    leftCI < lrows[li].values.size() ? lrows[li].values[leftCI] : "";
                hashMap[key].push_back(li);
            }

            for (const auto& rrow : rrows) {
                const std::string& rval =
                    rightCI < rrow.values.size() ? rrow.values[rightCI] : "";
                auto it = hashMap.find(rval);

                if (it != hashMap.end()) {
                    for (size_t li : it->second) {
                        std::vector<std::string> vals;
                        vals.reserve(leftWidth + rrow.values.size());
                        vals.insert(vals.end(),
                                    lrows[li].values.begin(), lrows[li].values.end());
                        vals.insert(vals.end(),
                                    rrow.values.begin(), rrow.values.end());
                        next.insert(Row(vals));
                    }
                } else {
                    // RIGHT JOIN: unmatched right row → NULL-pad left side
                    std::vector<std::string> vals;
                    vals.reserve(leftWidth + rightWidth);
                    for (size_t k = 0; k < leftWidth; ++k) vals.push_back("NULL");
                    vals.insert(vals.end(), rrow.values.begin(), rrow.values.end());
                    next.insert(Row(vals));
                }
            }

        } else if (joinType == "FULL") {
            // ── FULL OUTER: LEFT hash join + anti-right scan ─────────
            std::unordered_map<std::string, std::vector<size_t>> hashMap;
            hashMap.reserve(rrows.size());
            for (size_t ri = 0; ri < rrows.size(); ++ri) {
                const std::string& key =
                    rightCI < rrows[ri].values.size() ? rrows[ri].values[rightCI] : "";
                hashMap[key].push_back(ri);
            }

            // Track which right rows were matched
            std::vector<bool> rightMatched(rrows.size(), false);

            // Step 1: Probe from left — all left rows
            for (const auto& lrow : lrows) {
                const std::string& lval =
                    leftCI < lrow.values.size() ? lrow.values[leftCI] : "";
                auto it = hashMap.find(lval);

                if (it != hashMap.end()) {
                    for (size_t ri : it->second) {
                        rightMatched[ri] = true;
                        std::vector<std::string> vals;
                        vals.reserve(leftWidth + rrows[ri].values.size());
                        vals.insert(vals.end(),
                                    lrow.values.begin(), lrow.values.end());
                        vals.insert(vals.end(),
                                    rrows[ri].values.begin(), rrows[ri].values.end());
                        next.insert(Row(vals));
                    }
                } else {
                    std::vector<std::string> vals;
                    vals.reserve(leftWidth + rightWidth);
                    vals.insert(vals.end(), lrow.values.begin(), lrow.values.end());
                    for (size_t k = 0; k < rightWidth; ++k) vals.push_back("NULL");
                    next.insert(Row(vals));
                }
            }

            // Step 2: Anti-right — unmatched right rows → NULL-pad left
            for (size_t ri = 0; ri < rrows.size(); ++ri) {
                if (rightMatched[ri]) continue;
                std::vector<std::string> vals;
                vals.reserve(leftWidth + rightWidth);
                for (size_t k = 0; k < leftWidth; ++k) vals.push_back("NULL");
                vals.insert(vals.end(), rrows[ri].values.begin(), rrows[ri].values.end());
                next.insert(Row(vals));
            }
        }

        return next;
    }
};
