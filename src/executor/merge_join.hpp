#pragma once
// ============================================================
// merge_join.hpp — Phase 83: Merge Join Algorithm
// Included inside namespace milansql in engine.hpp (after Table
// class). Do NOT add a namespace milansql wrapper here and do NOT
// include system headers already included by engine.hpp.
//
// Complexity: O(n log n + m log m)
// Optimal when both join columns are indexed (data is sorted).
//
// Algorithm:
//   1. Build sorted index arrays for both sides (no data copy).
//   2. Two-pointer merge: advance the pointer with the smaller key.
//   3. On equal keys: output the cross product of the matching group.
//
// Only INNER join is implemented here. LEFT / RIGHT / FULL fall back
// to HashJoin (handled by the caller in executeJoins).
// ============================================================

struct MergeJoin {
    // Execute a sort-merge join between `current` (left) and `right`.
    // Only handles joinType == "INNER".
    static Table execute(const Table& current,
                         const Table& right,
                         size_t leftCI,
                         size_t rightCI,
                         const std::string& /* joinType — always INNER */,
                         const std::vector<Column>& newCols)
    {
        Table next("", newCols);
        const size_t leftWidth = current.columns().size();
        const auto&  lrows     = current.rows();
        const auto&  rrows     = right.rows();

        // ── Step 1: Build sorted index arrays (sort indices, not row data) ──
        std::vector<size_t> leftIdx, rightIdx;
        leftIdx.reserve(lrows.size());
        rightIdx.reserve(rrows.size());

        for (size_t i = 0; i < lrows.size(); ++i) leftIdx.push_back(i);
        for (size_t i = 0; i < rrows.size(); ++i) rightIdx.push_back(i);

        std::sort(leftIdx.begin(), leftIdx.end(), [&](size_t a, size_t b) {
            const std::string& av =
                leftCI < lrows[a].values.size() ? lrows[a].values[leftCI] : "";
            const std::string& bv =
                leftCI < lrows[b].values.size() ? lrows[b].values[leftCI] : "";
            return av < bv;
        });

        std::sort(rightIdx.begin(), rightIdx.end(), [&](size_t a, size_t b) {
            const std::string& av =
                rightCI < rrows[a].values.size() ? rrows[a].values[rightCI] : "";
            const std::string& bv =
                rightCI < rrows[b].values.size() ? rrows[b].values[rightCI] : "";
            return av < bv;
        });

        // ── Step 2: Two-pointer merge ─────────────────────────────────────
        size_t li = 0, ri = 0;
        while (li < leftIdx.size() && ri < rightIdx.size()) {
            const std::string& lv =
                leftCI < lrows[leftIdx[li]].values.size()
                    ? lrows[leftIdx[li]].values[leftCI] : "";
            const std::string& rv =
                rightCI < rrows[rightIdx[ri]].values.size()
                    ? rrows[rightIdx[ri]].values[rightCI] : "";

            if (lv < rv) { ++li; continue; }
            if (lv > rv) { ++ri; continue; }

            // Equal keys — find the extent of the right group for this key
            size_t ri_end = ri + 1;
            while (ri_end < rightIdx.size()) {
                const std::string& rv2 =
                    rightCI < rrows[rightIdx[ri_end]].values.size()
                        ? rrows[rightIdx[ri_end]].values[rightCI] : "";
                if (rv2 != lv) break;
                ++ri_end;
            }

            // Step 3: Output cross product of left group × right group
            //         Advance li over all left rows that share the same key.
            while (li < leftIdx.size()) {
                const std::string& lv2 =
                    leftCI < lrows[leftIdx[li]].values.size()
                        ? lrows[leftIdx[li]].values[leftCI] : "";
                if (lv2 != lv) break;

                const Row& lrow = lrows[leftIdx[li]];
                for (size_t rj = ri; rj < ri_end; ++rj) {
                    const Row& rrow = rrows[rightIdx[rj]];
                    std::vector<std::string> vals;
                    vals.reserve(leftWidth + rrow.values.size());
                    vals.insert(vals.end(),
                                lrow.values.begin(), lrow.values.end());
                    vals.insert(vals.end(),
                                rrow.values.begin(), rrow.values.end());
                    next.insert(Row(vals));
                }
                ++li;
            }

            // Advance right pointer past this group
            ri = ri_end;
        }

        return next;
    }
};
