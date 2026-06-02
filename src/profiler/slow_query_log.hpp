#pragma once
#include <string>
#include <vector>
#include <deque>
#include <algorithm>
#include <chrono>
#include <sstream>
#include <unordered_map>

// ============================================================
// slow_query_log.hpp — Phase 120: Slow Query Log + Fingerprinting
// ============================================================

namespace milansql {

struct SlowQueryEntry {
    std::string sql;
    std::string fingerprint;
    double durationMs = 0.0;
    long long timestamp = 0; // epoch ms
    int calls = 1;
    double totalMs = 0.0;
    double minMs = 0.0;
    double maxMs = 0.0;
    double avgMs = 0.0;
};

class SlowQueryLog {
public:
    bool enabled = false;
    double thresholdMs = 100.0;

    // Normalize SQL to a fingerprint: replace literals with ?
    static std::string fingerprint(const std::string& sql) {
        std::string result = sql;
        // Replace string literals 'value' → '?'
        {
            std::string out;
            bool inStr = false;
            for (size_t i = 0; i < result.size(); i++) {
                if (!inStr && result[i] == '\'') {
                    inStr = true;
                    out += "'?'";
                    // skip to closing quote
                    i++;
                    while (i < result.size() && result[i] != '\'') i++;
                } else {
                    out += result[i];
                }
            }
            result = out;
        }
        // Replace numbers with ?
        std::string out2;
        size_t i = 0;
        while (i < result.size()) {
            if (std::isdigit((unsigned char)result[i]) &&
                (i == 0 || (!std::isalnum((unsigned char)result[i-1]) && result[i-1] != '_'))) {
                out2 += '?';
                while (i < result.size() && (std::isdigit((unsigned char)result[i]) || result[i] == '.')) i++;
            } else {
                out2 += result[i++];
            }
        }
        result = out2;
        // Normalize whitespace
        std::string final_result;
        bool lastSpace = false;
        for (char c : result) {
            if (std::isspace((unsigned char)c)) {
                if (!lastSpace) { final_result += ' '; lastSpace = true; }
            } else {
                final_result += c; lastSpace = false;
            }
        }
        // Trim
        while (!final_result.empty() && final_result.back() == ' ') final_result.pop_back();
        if (!final_result.empty() && final_result.front() == ' ') final_result = final_result.substr(1);
        return final_result;
    }

    void add(const std::string& sql, double durationMs_val) {
        if (!enabled) return;
        if (durationMs_val < thresholdMs) return;

        auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        std::string fp = fingerprint(sql);

        // Check aggregated map
        auto it = aggregated_.find(fp);
        if (it != aggregated_.end()) {
            it->second.calls++;
            it->second.totalMs += durationMs_val;
            it->second.minMs = std::min(it->second.minMs, durationMs_val);
            it->second.maxMs = std::max(it->second.maxMs, durationMs_val);
            it->second.avgMs = it->second.totalMs / it->second.calls;
            it->second.durationMs = durationMs_val; // last duration
            it->second.timestamp = now;
        } else {
            SlowQueryEntry e;
            e.sql = sql;
            e.fingerprint = fp;
            e.durationMs = durationMs_val;
            e.timestamp = now;
            e.calls = 1;
            e.totalMs = durationMs_val;
            e.minMs = durationMs_val;
            e.maxMs = durationMs_val;
            e.avgMs = durationMs_val;
            aggregated_[fp] = e;
        }

        // Ring buffer for raw entries (last 1000)
        SlowQueryEntry raw;
        raw.sql = sql;
        raw.fingerprint = fp;
        raw.durationMs = durationMs_val;
        raw.timestamp = now;
        raw.calls = 1;
        raw.totalMs = durationMs_val;
        raw.minMs = durationMs_val;
        raw.maxMs = durationMs_val;
        raw.avgMs = durationMs_val;
        entries_.push_back(raw);
        if (entries_.size() > 1000) entries_.pop_front();
    }

    // Show slow queries sorted by duration desc, limited
    std::vector<SlowQueryEntry> showSlowQueries(int limit = 100) const {
        std::vector<SlowQueryEntry> result;
        for (auto& kv : aggregated_) result.push_back(kv.second);
        std::sort(result.begin(), result.end(), [](const SlowQueryEntry& a, const SlowQueryEntry& b){
            return a.durationMs > b.durationMs;
        });
        if (limit > 0 && (int)result.size() > limit) result.resize((size_t)limit);
        return result;
    }

    std::vector<SlowQueryEntry> showTopByTime(int limit = 10) const {
        auto r = showSlowQueries(0);
        std::sort(r.begin(), r.end(), [](const SlowQueryEntry& a, const SlowQueryEntry& b){
            return a.avgMs > b.avgMs;
        });
        if (limit > 0 && (int)r.size() > limit) r.resize((size_t)limit);
        return r;
    }

    std::vector<SlowQueryEntry> showTopByCalls(int limit = 10) const {
        auto r = showSlowQueries(0);
        std::sort(r.begin(), r.end(), [](const SlowQueryEntry& a, const SlowQueryEntry& b){
            return a.calls > b.calls;
        });
        if (limit > 0 && (int)r.size() > limit) r.resize((size_t)limit);
        return r;
    }

    std::vector<SlowQueryEntry> showTopByTotal(int limit = 10) const {
        auto r = showSlowQueries(0);
        std::sort(r.begin(), r.end(), [](const SlowQueryEntry& a, const SlowQueryEntry& b){
            return a.totalMs > b.totalMs;
        });
        if (limit > 0 && (int)r.size() > limit) r.resize((size_t)limit);
        return r;
    }

    // Index recommendations: queries with sequential scans
    std::vector<std::string> indexRecommendations() const {
        std::vector<std::string> recs;
        for (auto& kv : aggregated_) {
            const SlowQueryEntry& e = kv.second;
            std::string upper = e.fingerprint;
            std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);
            if (upper.find("WHERE") != std::string::npos &&
                upper.find("INDEX") == std::string::npos &&
                e.avgMs > thresholdMs * 2) {
                std::string rec = "Slow query may benefit from an index: " + e.fingerprint;
                recs.push_back(rec);
            }
        }
        return recs;
    }

    void flush() {
        entries_.clear();
        aggregated_.clear();
    }

    size_t size() const { return aggregated_.size(); }

private:
    std::deque<SlowQueryEntry> entries_;
    std::unordered_map<std::string, SlowQueryEntry> aggregated_;
};

} // namespace milansql
