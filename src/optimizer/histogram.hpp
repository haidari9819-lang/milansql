#pragma once
// ============================================================
// histogram.hpp — Phase 113: Column Histogram for Selectivity
//
// Wraps the equi-depth histogram stored in ColumnStats and
// provides refined selectivity estimation for range queries.
// Bridges with existing TableStats (Phase 86) data.
// ============================================================

#include <vector>
#include <string>
#include <algorithm>
#include <cmath>

namespace milansql {

// One bucket of an equi-depth histogram
struct HistogramBucket {
    std::string lower;     // inclusive lower bound (empty = −∞)
    std::string upper;     // inclusive upper bound
    double      frequency; // fraction of total rows in this bucket (0..1)
};

// Histogram for a single column (Phase 113)
class Histogram {
public:
    // Build from the (upperBound, count) pairs stored in ColumnStats.histogram
    void build(const std::vector<std::pair<std::string,size_t>>& rawBuckets,
               size_t totalRows) {
        buckets_.clear();
        if (totalRows == 0 || rawBuckets.empty()) return;

        std::string prevUpper;
        for (size_t i = 0; i < rawBuckets.size(); ++i) {
            HistogramBucket b;
            b.lower     = prevUpper;           // exclusive: prev bucket's upper bound
            b.upper     = rawBuckets[i].first;
            b.frequency = static_cast<double>(rawBuckets[i].second)
                        / static_cast<double>(totalRows);
            buckets_.push_back(b);
            prevUpper = rawBuckets[i].first;
        }
    }

    // Estimate selectivity for col op val  (returns fraction 0..1)
    double estimateSelectivity(const std::string& op, const std::string& val) const {
        if (buckets_.empty()) return 0.1;

        if (op == "<" || op == "<=") {
            double sel = 0.0;
            for (const auto& b : buckets_) {
                if (val >= b.upper) {
                    sel += b.frequency;             // entire bucket below val
                } else if (val >= b.lower) {
                    sel += b.frequency * 0.5;       // partial bucket
                    break;
                } else {
                    break;
                }
            }
            return std::min(std::max(sel, 0.0), 1.0);
        }

        if (op == ">" || op == ">=") {
            return std::max(1.0 - estimateSelectivity("<", val), 0.0);
        }

        if (op == "=" || op == "IS") {
            // Find which bucket val falls in; assume uniform distribution within bucket
            for (const auto& b : buckets_) {
                if (val <= b.upper) {
                    // Rough: 1/10 of the bucket
                    return b.frequency * 0.1;
                }
            }
            return 0.01;
        }

        return 0.1; // default
    }

    // Estimate selectivity for  low <= col <= high
    double estimateRangeSelectivity(const std::string& low, const std::string& high) const {
        if (buckets_.empty()) return 0.1;
        if (low > high) return 0.0;

        double sel = 0.0;
        for (const auto& b : buckets_) {
            if (high < b.lower) break;     // entirely above range
            if (low  > b.upper) continue;  // entirely below range
            sel += b.frequency;            // at least partially overlaps
        }
        return std::min(std::max(sel, 0.0), 1.0);
    }

    bool empty() const { return buckets_.empty(); }
    const std::vector<HistogramBucket>& buckets() const { return buckets_; }

private:
    std::vector<HistogramBucket> buckets_;
};

} // namespace milansql
