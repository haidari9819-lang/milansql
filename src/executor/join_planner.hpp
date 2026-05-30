#pragma once
// ============================================================
// join_planner.hpp — Phase 83: Join Strategy Planner
// Included inside namespace milansql in engine.hpp.
// Do NOT add a namespace milansql wrapper here.
// ============================================================

#include <string>
#include <cstddef>

enum class JoinStrategy { NESTED_LOOP, HASH_JOIN, MERGE_JOIN };

struct JoinPlanner {
    // Threshold: if BOTH sides have fewer rows than THRESHOLD → NESTED_LOOP
    static constexpr size_t THRESHOLD = 10;

    // Choose the optimal join strategy based on table sizes and index availability.
    // Priority:
    //   1. Both sides < THRESHOLD rows                          → NESTED_LOOP
    //   2. Both sides have an index on their join column        → MERGE_JOIN
    //   3. Otherwise (large, no matching indexes)               → HASH_JOIN
    static JoinStrategy choose(size_t leftSize, size_t rightSize,
                               bool leftHasIndex, bool rightHasIndex) {
        if (leftSize < THRESHOLD && rightSize < THRESHOLD)
            return JoinStrategy::NESTED_LOOP;
        if (leftHasIndex && rightHasIndex)
            return JoinStrategy::MERGE_JOIN;
        return JoinStrategy::HASH_JOIN;
    }

    // Short name for display in EXPLAIN
    static std::string name(JoinStrategy s) {
        switch (s) {
            case JoinStrategy::NESTED_LOOP: return "NESTED_LOOP";
            case JoinStrategy::HASH_JOIN:   return "HASH_JOIN";
            case JoinStrategy::MERGE_JOIN:  return "MERGE_JOIN";
        }
        return "NESTED_LOOP";
    }

    // Descriptive string for EXPLAIN output
    static std::string description(JoinStrategy s, size_t leftSize, size_t rightSize) {
        switch (s) {
            case JoinStrategy::NESTED_LOOP:
                return "NESTED_LOOP (left: " + std::to_string(leftSize) +
                       " rows, right: " + std::to_string(rightSize) + " rows)";
            case JoinStrategy::HASH_JOIN:
                return "HASH_JOIN (left: " + std::to_string(leftSize) +
                       " rows, right: " + std::to_string(rightSize) + " rows)";
            case JoinStrategy::MERGE_JOIN:
                return "MERGE_JOIN (index available, left: " + std::to_string(leftSize) +
                       " rows, right: " + std::to_string(rightSize) + " rows)";
        }
        return "NESTED_LOOP";
    }
};
