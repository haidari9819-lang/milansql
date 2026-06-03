#pragma once
#include <string>
#include <unordered_map>
#include <vector>
#include <chrono>
#include <algorithm>
#include <sstream>

namespace milansql {

struct CachedPlan {
    std::string fingerprint;
    std::string tableName;    // primary table (for invalidation)
    std::string planDesc;     // human-readable plan description
    long long lastUsed = 0;   // epoch ms
    int hitCount = 0;
    double avgExecMs = 0.0;
};

class PlanCache {
public:
    int maxPlans = 1000;

    void store(const std::string& fp, const std::string& table,
               const std::string& planDesc, double execMs) {
        auto it = cache_.find(fp);
        if (it != cache_.end()) {
            it->second.hitCount++;
            it->second.avgExecMs = (it->second.avgExecMs * (it->second.hitCount - 1) + execMs)
                                    / it->second.hitCount;
            it->second.lastUsed = now();
            return;
        }
        // Evict LRU if full
        if ((int)cache_.size() >= maxPlans) evictLRU();

        CachedPlan p;
        p.fingerprint = fp;
        p.tableName = table;
        p.planDesc = planDesc;
        p.lastUsed = now();
        p.hitCount = 1;
        p.avgExecMs = execMs;
        cache_[fp] = p;
    }

    CachedPlan* lookup(const std::string& fp) {
        auto it = cache_.find(fp);
        if (it == cache_.end()) return nullptr;
        it->second.hitCount++;
        it->second.lastUsed = now();
        return &it->second;
    }

    void invalidate(const std::string& table) {
        std::vector<std::string> toRemove;
        for (auto& [fp, p] : cache_)
            if (p.tableName == table) toRemove.push_back(fp);
        for (auto& k : toRemove) cache_.erase(k);
    }

    void flush() { cache_.clear(); }

    std::vector<CachedPlan> all() const {
        std::vector<CachedPlan> result;
        for (auto& [fp, p] : cache_) result.push_back(p);
        std::sort(result.begin(), result.end(),
            [](const CachedPlan& a, const CachedPlan& b){ return a.hitCount > b.hitCount; });
        return result;
    }

    size_t size() const { return cache_.size(); }

private:
    std::unordered_map<std::string, CachedPlan> cache_;

    static long long now() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    void evictLRU() {
        if (cache_.empty()) return;
        auto oldest = cache_.begin();
        for (auto it = cache_.begin(); it != cache_.end(); ++it)
            if (it->second.lastUsed < oldest->second.lastUsed) oldest = it;
        cache_.erase(oldest);
    }
};

} // namespace milansql
