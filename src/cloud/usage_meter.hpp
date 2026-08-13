#pragma once
#include <string>
#include <unordered_map>
#include <mutex>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <chrono>
#include <ctime>

// Requires cloud_instance.hpp for CloudPlan / getPlanLimits
#include "cloud_instance.hpp"

namespace milansql {

// ── Per-instance usage record ─────────────────────────────────────────────────
struct InstanceUsage {
    std::string instanceId;

    // Query counters
    long long queryCountDaily = 0;
    long long queryCountTotal = 0;

    // Storage
    long long storageBytes = 0;

    // Network bandwidth (bytes)
    long long bandwidthInBytes  = 0;
    long long bandwidthOutBytes = 0;

    // Last daily reset (ISO date YYYY-MM-DD)
    std::string lastResetDate;
};

// ── Limit check result ────────────────────────────────────────────────────────
struct LimitCheckResult {
    bool        allowed;
    std::string reason;
    int         percentUsed; // 0-100+ (can exceed 100 if over limit)
};

// ── Helpers ───────────────────────────────────────────────────────────────────
namespace usage_detail {

inline std::string todayDate() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    char buf[16];
    struct tm tm_buf;
#ifdef _WIN32
    gmtime_s(&tm_buf, &t);
#else
    gmtime_r(&t, &tm_buf);
#endif
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm_buf);
    return std::string(buf);
}

inline std::string usageToLine(const InstanceUsage& u) {
    return u.instanceId + "|" +
           std::to_string(u.queryCountDaily) + "|" +
           std::to_string(u.queryCountTotal) + "|" +
           std::to_string(u.storageBytes)    + "|" +
           std::to_string(u.bandwidthInBytes) + "|" +
           std::to_string(u.bandwidthOutBytes) + "|" +
           u.lastResetDate;
}

inline InstanceUsage lineToUsage(const std::string& line) {
    InstanceUsage u;
    std::vector<std::string> parts;
    std::istringstream ss(line);
    std::string tok;
    while (std::getline(ss, tok, '|')) parts.push_back(tok);
    if (parts.size() < 7) return u;
    u.instanceId        = parts[0];
    u.queryCountDaily   = std::stoll(parts[1]);
    u.queryCountTotal   = std::stoll(parts[2]);
    u.storageBytes      = std::stoll(parts[3]);
    u.bandwidthInBytes  = std::stoll(parts[4]);
    u.bandwidthOutBytes = std::stoll(parts[5]);
    u.lastResetDate     = parts[6];
    return u;
}

inline std::string usageToJson(const InstanceUsage& u) {
    std::ostringstream j;
    j << "{"
      << "\"instanceId\":\""       << u.instanceId            << "\","
      << "\"queryCountDaily\":"    << u.queryCountDaily        << ","
      << "\"queryCountTotal\":"    << u.queryCountTotal        << ","
      << "\"storageBytes\":"       << u.storageBytes           << ","
      << "\"bandwidthInBytes\":"   << u.bandwidthInBytes       << ","
      << "\"bandwidthOutBytes\":"  << u.bandwidthOutBytes      << ","
      << "\"lastResetDate\":\""    << u.lastResetDate          << "\""
      << "}";
    return j.str();
}

} // namespace usage_detail

// ── UsageMeter ────────────────────────────────────────────────────────────────
class UsageMeter {
public:
    static UsageMeter& instance() {
        static UsageMeter m;
        return m;
    }
public:
    explicit UsageMeter(const std::string& dataDir = "/opt/milansql/data")
        : dataDir_(dataDir) {
        filePath_ = dataDir_ + "/cloud_usage.data";
        loadFromDisk();
    }

    // Record a query execution for an instance
    // bytesIn/Out are optional network bytes transferred
    void recordQuery(const std::string& instanceId,
                     long long bytesIn  = 0,
                     long long bytesOut = 0) {
        std::lock_guard<std::mutex> lk(mu_);
        auto& u = getOrCreate(instanceId);
        maybeDailyReset(u);
        u.queryCountDaily++;
        u.queryCountTotal++;
        u.bandwidthInBytes  += bytesIn;
        u.bandwidthOutBytes += bytesOut;
        saveToDisk();
    }

    // Update storage usage for an instance (absolute bytes)
    void recordStorage(const std::string& instanceId, long long bytes) {
        std::lock_guard<std::mutex> lk(mu_);
        auto& u = getOrCreate(instanceId);
        u.storageBytes = bytes;
        saveToDisk();
    }

    // Check whether the instance is within its plan limits
    LimitCheckResult checkLimit(const std::string& instanceId, CloudPlan plan) {
        std::lock_guard<std::mutex> lk(mu_);
        auto& u = getOrCreate(instanceId);
        maybeDailyReset(u);

        PlanLimits limits = getPlanLimits(plan);
        LimitCheckResult result{true, "ok", 0};

        // Check daily query limit
        if (limits.queriesPerDay > 0) {
            int pct = static_cast<int>((u.queryCountDaily * 100) / limits.queriesPerDay);
            result.percentUsed = pct;
            if (u.queryCountDaily >= limits.queriesPerDay) {
                result.allowed     = false;
                result.reason      = "Daily query limit reached (" +
                                     std::to_string(limits.queriesPerDay) + " queries/day)";
                result.percentUsed = pct;
                return result;
            }
        }

        // Check storage limit
        if (limits.storageBytes > 0 && u.storageBytes > limits.storageBytes) {
            int pct = static_cast<int>((u.storageBytes * 100) / limits.storageBytes);
            result.allowed     = false;
            result.reason      = "Storage limit exceeded";
            result.percentUsed = pct;
            return result;
        }

        // Compute overall percent used (max of query% and storage%)
        if (limits.queriesPerDay > 0) {
            result.percentUsed = static_cast<int>(
                (u.queryCountDaily * 100) / limits.queriesPerDay);
        } else if (limits.storageBytes > 0 && u.storageBytes > 0) {
            result.percentUsed = static_cast<int>(
                (u.storageBytes * 100) / limits.storageBytes);
        }

        return result;
    }

    // Return usage as JSON
    std::string getUsageJson(const std::string& instanceId) {
        std::lock_guard<std::mutex> lk(mu_);
        auto& u = getOrCreate(instanceId);
        maybeDailyReset(u);
        return usage_detail::usageToJson(u);
    }

    // Reset daily query counters for all instances (call at midnight / via cron)
    void resetDailyCounters() {
        std::lock_guard<std::mutex> lk(mu_);
        std::string today = usage_detail::todayDate();
        for (auto& [k, v] : usage_) {
            v.queryCountDaily = 0;
            v.lastResetDate   = today;
        }
        saveToDisk();
    }

    // Get raw usage record (no lock — caller must hold mu_ or use snapshot)
    InstanceUsage getUsage(const std::string& instanceId) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = usage_.find(instanceId);
        if (it == usage_.end()) {
            InstanceUsage u;
            u.instanceId    = instanceId;
            u.lastResetDate = usage_detail::todayDate();
            return u;
        }
        return it->second;
    }

private:
    std::string filePath_;
    std::string dataDir_;
    std::mutex  mu_;
    std::unordered_map<std::string, InstanceUsage> usage_;

    InstanceUsage& getOrCreate(const std::string& instanceId) {
        auto it = usage_.find(instanceId);
        if (it == usage_.end()) {
            InstanceUsage u;
            u.instanceId    = instanceId;
            u.lastResetDate = usage_detail::todayDate();
            usage_[instanceId] = u;
            return usage_[instanceId];
        }
        return it->second;
    }

    // Must be called with mu_ held
    void maybeDailyReset(InstanceUsage& u) {
        std::string today = usage_detail::todayDate();
        if (u.lastResetDate != today) {
            u.queryCountDaily = 0;
            u.lastResetDate   = today;
        }
    }

    void loadFromDisk() {
        std::ifstream f(filePath_);
        if (!f.is_open()) return;
        std::string line;
        while (std::getline(f, line)) {
            if (line.empty() || line[0] == '#') continue;
            InstanceUsage u = usage_detail::lineToUsage(line);
            if (!u.instanceId.empty())
                usage_[u.instanceId] = u;
        }
    }

    void saveToDisk() {
        std::ofstream f(filePath_, std::ios::trunc);
        if (!f.is_open()) return;
        f << "# MilanSQL Cloud Usage\n";
        for (auto& [k, v] : usage_)
            f << usage_detail::usageToLine(v) << "\n";
    }
};

} // namespace milansql
