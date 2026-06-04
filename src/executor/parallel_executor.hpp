#pragma once
// ============================================================
// parallel_executor.hpp — Phase 141: Parallel Query Execution V2
// ============================================================
#include <thread>
#include <vector>
#include <atomic>
#include <mutex>
#include <functional>
#include <cstdint>
#include <algorithm>
#include <limits>
#include <string>
#include <stdexcept>

namespace milansql {

// Per-thread partial aggregate result
struct PartialAgg {
    int64_t count  = 0;
    double  sum    = 0.0;
    double  minVal = std::numeric_limits<double>::max();
    double  maxVal = std::numeric_limits<double>::lowest();
    bool    hasVal = false;
};

// Merge partial aggregates for SUM/COUNT/AVG/MIN/MAX
inline PartialAgg mergePartials(const std::vector<PartialAgg>& parts) {
    PartialAgg result;
    for (const auto& p : parts) {
        result.count  += p.count;
        result.sum    += p.sum;
        if (p.hasVal) {
            if (!result.hasVal || p.minVal < result.minVal) result.minVal = p.minVal;
            if (!result.hasVal || p.maxVal > result.maxVal) result.maxVal = p.maxVal;
            result.hasVal = true;
        }
    }
    return result;
}

// Compute aggregate on a range of row indices
// rows: all row values for the column
inline PartialAgg computeChunkAgg(const std::vector<std::string>& colVals,
                                   const std::vector<size_t>& indices,
                                   size_t startIdx, size_t endIdx) {
    PartialAgg p;
    for (size_t i = startIdx; i < endIdx && i < indices.size(); ++i) {
        size_t ri = indices[i];
        if (ri >= colVals.size()) continue;
        const std::string& v = colVals[ri];
        if (v == "NULL" || v.empty()) continue;
        try {
            double d = std::stod(v);
            p.sum   += d;
            p.count += 1;
            if (!p.hasVal || d < p.minVal) p.minVal = d;
            if (!p.hasVal || d > p.maxVal) p.maxVal = d;
            p.hasVal = true;
        } catch (...) {
            // non-numeric: count it but skip for numeric aggregates
            p.count += 1;
        }
    }
    return p;
}

// Parallel aggregate executor
// colVals: all values for the target column (indexed by row idx)
// indices: row indices to process
// workers: number of parallel threads (clamped to 1..hardware_concurrency)
inline PartialAgg parallelAggregate(const std::vector<std::string>& colVals,
                                     const std::vector<size_t>& indices,
                                     int workers) {
    if (workers <= 1 || indices.size() < 64) {
        return computeChunkAgg(colVals, indices, 0, indices.size());
    }

    int hw = static_cast<int>(std::thread::hardware_concurrency());
    if (hw < 1) hw = 1;
    workers = std::min(workers, hw);
    workers = std::min(workers, static_cast<int>(indices.size()));
    if (workers < 1) workers = 1;

    size_t chunkSize = (indices.size() + workers - 1) / workers;
    std::vector<PartialAgg> partials(static_cast<size_t>(workers));
    std::vector<std::thread> threads;
    threads.reserve(static_cast<size_t>(workers));

    for (int t = 0; t < workers; ++t) {
        size_t start = static_cast<size_t>(t) * chunkSize;
        size_t end   = std::min(start + chunkSize, indices.size());
        threads.emplace_back([&, t, start, end]() {
            partials[static_cast<size_t>(t)] = computeChunkAgg(colVals, indices, start, end);
        });
    }
    for (auto& th : threads) th.join();

    return mergePartials(partials);
}

// Parallel COUNT(*) — counts all non-dead rows
inline int64_t parallelCount(const std::vector<size_t>& indices, int workers) {
    (void)workers;
    // COUNT is trivially the size of indices (already filtered)
    return static_cast<int64_t>(indices.size());
}

} // namespace milansql
