#pragma once
// ============================================================
// dp_planner.hpp — Phase 113: Dynamic Programming Join Order Optimizer
//
// Guarantees optimal join order for n tables via bitmask DP.
//   - Enumerates all 2^n subsets bottom-up
//   - memo[mask] = cheapest plan to join that subset of tables
//   - Respects join-graph connectivity (only valid orderings)
//   - Plan cache keyed on comma-separated sorted table names
//
// Cost model:
//   scan       = rows × SEQ_PAGE_COST
//   hash_join  = build_rows × HASH_BUILD + probe_rows × HASH_PROBE
//   merge_join = (left + right) × SORT_FACTOR
//   nested_loop= outer × inner × CPU_COST
//
// Reference: Selinger et al. (1979) "Access Path Selection in a RDBMS"
// ============================================================

#include <string>
#include <vector>
#include <map>
#include <set>
#include <mutex>
#include <numeric>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <atomic>
#include <cstdint>

namespace milansql {

// ── JoinPlan: result of the DP optimizer ──────────────────────
struct JoinPlan {
    std::vector<int> joinOrder;      // 0-based indices into original JoinClauses[]
    double           estimatedCost  = 0.0;
    size_t           subsetsEvaluated = 0;
    bool             dpUsed         = false;  // true when DP ran (>= 3 tables)
    std::string      description;             // human-readable summary
};

// ── Per-table info fed to the planner ─────────────────────────
struct JoinTableInfo {
    std::string      name;
    size_t           rowCount       = 0;
    std::vector<int> requiredTables; // table indices that must already be in result
};

// ── Global lock-free DP stats ─────────────────────────────────
struct DpStats {
    std::atomic<uint64_t> queriesPlanned{0};   // times DP ran (>= 3 tables)
    std::atomic<uint64_t> planCacheHits{0};    // plan cache hits
    std::atomic<uint64_t> planCacheMisses{0};  // plan cache misses
    std::atomic<uint64_t> totalSubsetsEval{0}; // cumulative subsets evaluated
};
inline DpStats& g_dpStats() {
    static DpStats s;
    return s;
}

// ── DpPlanner class ────────────────────────────────────────────
class DpPlanner {
public:
    // ── Cost model constants ────────────────────────────────────
    static constexpr double SEQ_PAGE_COST  = 1.0;
    static constexpr double HASH_BUILD     = 1.5;
    static constexpr double HASH_PROBE     = 1.0;
    static constexpr double SORT_FACTOR    = 1.2;
    static constexpr double CPU_COST       = 0.01;
    static constexpr double JOIN_SEL       = 0.1; // default join selectivity

    // ── Cost functions ──────────────────────────────────────────
    static double scanCost(size_t rows) {
        return static_cast<double>(rows) * SEQ_PAGE_COST;
    }
    static double hashJoinCost(size_t buildRows, size_t probeRows) {
        return static_cast<double>(buildRows) * HASH_BUILD
             + static_cast<double>(probeRows) * HASH_PROBE;
    }
    static double nestedLoopCost(size_t outer, size_t inner) {
        return static_cast<double>(outer) * static_cast<double>(inner) * CPU_COST;
    }
    static double mergeJoinCost(size_t left, size_t right) {
        return static_cast<double>(left + right) * SORT_FACTOR;
    }
    static size_t estimateJoinRows(size_t leftRows, size_t rightRows,
                                   double sel = JOIN_SEL) {
        size_t r = static_cast<size_t>(
            static_cast<double>(leftRows) * static_cast<double>(rightRows) * sel);
        return std::max<size_t>(r, 1);
    }

    // ── Main plan() method ──────────────────────────────────────
    // tables[0] = base (always the entry point, no JoinClause)
    // tables[1..n-1] = join tables corresponding to JoinClauses[0..n-2]
    // cacheKey = "" to skip caching
    JoinPlan plan(const std::vector<JoinTableInfo>& tables,
                  const std::string& cacheKey = "") {
        const int n = static_cast<int>(tables.size());

        // ── Trivial cases ─────────────────────────────────────
        if (n <= 1) {
            JoinPlan p; p.dpUsed = false;
            p.description = "(single table)";
            return p;
        }
        if (n == 2) {
            JoinPlan p; p.joinOrder = {0}; p.dpUsed = false;
            p.estimatedCost = scanCost(tables[0].rowCount)
                            + scanCost(tables[1].rowCount)
                            + pickJoinCost(tables[0].rowCount, tables[1].rowCount);
            p.description = "(2 tables, single join)";
            return p;
        }

        // ── Plan cache lookup ─────────────────────────────────
        if (!cacheKey.empty()) {
            std::lock_guard<std::mutex> g(cacheMu_);
            auto it = planCache_.find(cacheKey);
            if (it != planCache_.end()) {
                g_dpStats().planCacheHits.fetch_add(1, std::memory_order_relaxed);
                JoinPlan cached = it->second;
                cached.description = "[cached] " + cached.description;
                return cached;
            }
        }
        g_dpStats().planCacheMisses.fetch_add(1, std::memory_order_relaxed);

        // ── Bitmask DP ────────────────────────────────────────
        // bit i set ↔ tables[i] is included in the plan
        // We always include bit 0 (base table) in all valid subsets.
        struct DpState {
            double           cost    = 1e18;
            size_t           estRows = 0;
            std::vector<int> order;   // JC indices (tables[i]-1 for i>0) in join order
            bool             valid   = false;
        };

        const int fullMask = (1 << n) - 1;
        std::vector<DpState> memo(static_cast<size_t>(fullMask + 1));
        size_t subsetsEval = 0;

        // Base cases: single-table scans
        for (int i = 0; i < n; ++i) {
            int mask = 1 << i;
            DpState& s   = memo[static_cast<size_t>(mask)];
            s.cost        = scanCost(tables[static_cast<size_t>(i)].rowCount);
            s.estRows     = tables[static_cast<size_t>(i)].rowCount;
            s.valid       = true;
            ++subsetsEval;
        }

        // Build larger subsets (must always include bit 0)
        for (int sz = 2; sz <= n; ++sz) {
            for (int mask = 1; mask <= fullMask; ++mask) {
                if (__builtin_popcount(mask) != sz) continue;
                if (!(mask & 1)) continue;   // must include base table
                ++subsetsEval;

                DpState& cur = memo[static_cast<size_t>(mask)];

                // Try adding each table j (j > 0) to a smaller subset
                for (int j = 1; j < n; ++j) {
                    if (!(mask & (1 << j))) continue;  // j not in mask
                    int submask = mask ^ (1 << j);

                    const DpState& sub = memo[static_cast<size_t>(submask)];
                    if (!sub.valid) continue;

                    // Connectivity check: all of j's required tables must be in submask
                    bool ok = true;
                    for (int req : tables[static_cast<size_t>(j)].requiredTables) {
                        if (!(submask & (1 << req))) { ok = false; break; }
                    }
                    if (!ok) continue;

                    // Cost of adding table j
                    size_t jRows    = tables[static_cast<size_t>(j)].rowCount;
                    double jCost    = pickJoinCost(sub.estRows, jRows);
                    double newCost  = sub.cost + jCost;
                    size_t newRows  = estimateJoinRows(sub.estRows, jRows);

                    if (!cur.valid || newCost < cur.cost) {
                        cur.cost    = newCost;
                        cur.estRows = newRows;
                        cur.valid   = true;
                        cur.order   = sub.order;
                        cur.order.push_back(j - 1);  // JC index = tableIdx - 1
                    }
                }
            }
        }

        // ── Extract result ────────────────────────────────────
        JoinPlan result;
        result.subsetsEvaluated = subsetsEval;
        result.dpUsed           = true;

        const DpState& best = memo[static_cast<size_t>(fullMask)];
        if (best.valid && static_cast<int>(best.order.size()) == n - 1) {
            result.joinOrder     = best.order;
            result.estimatedCost = best.cost;

            std::ostringstream oss;
            oss << n << " tables, " << subsetsEval << " subsets evaluated";
            oss << ", est.cost=" << std::fixed << std::setprecision(1) << best.cost;
            oss << "  order: " << tables[0].name;
            for (int idx : result.joinOrder)
                oss << " \u2192 " << tables[static_cast<size_t>(idx + 1)].name;
            result.description = oss.str();
        } else {
            // Fallback: original order (disconnected join graph)
            result.joinOrder.resize(static_cast<size_t>(n - 1));
            std::iota(result.joinOrder.begin(), result.joinOrder.end(), 0);
            result.dpUsed       = false;
            result.estimatedCost = 0.0;
            result.description  = "(fallback: original order — connectivity constraint)";
        }

        // Update global stats
        g_dpStats().queriesPlanned.fetch_add(1, std::memory_order_relaxed);
        g_dpStats().totalSubsetsEval.fetch_add(
            static_cast<uint64_t>(subsetsEval), std::memory_order_relaxed);

        // Cache the result
        if (!cacheKey.empty() && result.dpUsed) {
            std::lock_guard<std::mutex> g(cacheMu_);
            planCache_[cacheKey] = result;
        }

        return result;
    }

    // ── Cache invalidation ──────────────────────────────────────
    void invalidate() {
        std::lock_guard<std::mutex> g(cacheMu_);
        planCache_.clear();
    }

    void invalidate(const std::string& tableName) {
        std::lock_guard<std::mutex> g(cacheMu_);
        for (auto it = planCache_.begin(); it != planCache_.end(); ) {
            if (it->first.find(tableName) != std::string::npos)
                it = planCache_.erase(it);
            else
                ++it;
        }
    }

    size_t cacheSize() const {
        std::lock_guard<std::mutex> g(cacheMu_);
        return planCache_.size();
    }

private:
    mutable std::mutex              cacheMu_;
    std::map<std::string, JoinPlan> planCache_;

    static double pickJoinCost(size_t left, size_t right) {
        if (left < 10 && right < 10)
            return nestedLoopCost(left, right);
        double h = hashJoinCost(std::min(left, right), std::max(left, right));
        double m = mergeJoinCost(left, right);
        return std::min(h, m);
    }
};

// ── Global singleton ───────────────────────────────────────────
inline DpPlanner& g_dpPlanner() {
    static DpPlanner planner;
    return planner;
}

} // namespace milansql
