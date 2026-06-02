#pragma once
// ============================================================
// runtime_config.hpp — Hot Config Reload (Phase 109)
// SET CONFIG key = value  /  RELOAD CONFIG  /  SHOW CONFIG
// Persisted in database.config (key=value lines)
// ============================================================

#include <string>
#include <map>
#include <vector>
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>

namespace milansql {

struct ConfigEntry {
    std::string key;
    std::string value;
    std::string description;
};

class RuntimeConfig {
public:
    RuntimeConfig() { initDefaults(); }

    // ── Public API ────────────────────────────────────────────

    void set(const std::string& key, const std::string& value) {
        std::string k = normalize(key);
        if (configs_.count(k)) {
            configs_[k].value = value;
        } else {
            configs_[k] = ConfigEntry{k, value, ""};
        }
        save();
    }

    std::string get(const std::string& key) const {
        std::string k = normalize(key);
        auto it = configs_.find(k);
        return (it != configs_.end()) ? it->second.value : "";
    }

    bool has(const std::string& key) const {
        return configs_.count(normalize(key)) > 0;
    }

    // Reload from file — picks up external edits without restart
    void reload() {
        initDefaults();
        load();
    }

    void save() const {
        std::ofstream f(filePath_);
        if (!f.is_open()) return;
        for (const auto& [k, e] : configs_)
            f << k << "=" << e.value << "\n";
    }

    void load() {
        std::ifstream f(filePath_);
        if (!f.is_open()) return;
        std::string line;
        while (std::getline(f, line)) {
            if (line.empty() || line[0] == '#') continue;
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string k = line.substr(0, eq);
            std::string v = line.substr(eq + 1);
            configs_[normalize(k)].value = v;
        }
    }

    std::string showConfig() const {
        std::ostringstream oss;
        oss << "\n+-----------------------------+--------------------------+----------------------------------------+\n";
        oss << "| Parameter                   | Value                    | Description                            |\n";
        oss << "+-----------------------------+--------------------------+----------------------------------------+\n";
        for (const auto& [k, e] : configs_) {
            std::string kp = k; kp.resize(27, ' ');
            std::string vp = e.value; vp.resize(24, ' ');
            std::string dp = e.description; if (dp.size() > 38) dp = dp.substr(0, 35) + "..."; dp.resize(38, ' ');
            oss << "| " << kp << " | " << vp << " | " << dp << " |\n";
        }
        oss << "+-----------------------------+--------------------------+----------------------------------------+\n";
        oss << "  " << configs_.size() << " configuration parameters\n\n";
        return oss.str();
    }

    // Apply config values to external singletons after load/set
    // (callers pass references to relevant globals)
    long long getInt(const std::string& key, long long def = 0) const {
        std::string v = get(key);
        if (v.empty()) return def;
        try { return std::stoll(v); } catch (...) { return def; }
    }

    bool getBool(const std::string& key, bool def = false) const {
        std::string v = get(key);
        if (v.empty()) return def;
        for (auto& c : v) { char lc = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); c = lc; }
        return v == "on" || v == "true" || v == "1" || v == "yes";
    }

private:
    static constexpr const char* filePath_ = "database.config";

    std::map<std::string, ConfigEntry> configs_;

    static std::string normalize(const std::string& s) {
        std::string r = s;
        // trim
        while (!r.empty() && (r.front() == ' ' || r.front() == '\t')) r.erase(r.begin());
        while (!r.empty() && (r.back()  == ' ' || r.back()  == '\t')) r.pop_back();
        for (auto& c : r) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return r;
    }

    void initDefaults() {
        auto add = [&](const std::string& k, const std::string& v, const std::string& desc) {
            configs_[k] = ConfigEntry{k, v, desc};
        };
        add("buffer_pool_size",        "128",  "Buffer pool size in MB");
        add("max_connections",         "100",  "Maximum client connections");
        add("auto_vacuum",             "ON",   "Enable automatic VACUUM");
        add("auto_vacuum_threshold",   "100",  "Rows deleted before auto-vacuum");
        add("auto_checkpoint",         "ON",   "Enable WAL auto-checkpoint");
        add("query_cache",             "ON",   "Enable query result cache");
        add("query_cache_size",        "100",  "Max cached query results");
        add("statement_cache_size",    "256",  "Max LRU statement cache entries");
        add("parallel_threshold",      "1000", "Min rows for parallel execution");
        add("max_parallel_workers",    "4",    "Max parallel query workers");
        add("pool_mode",               "SESSION", "Connection pool mode");
        add("pool_max_connections",    "10",   "Max pool connections");
        add("log_queries",             "OFF",  "Log all SQL queries");
        add("log_slow_queries",        "OFF",  "Log queries slower than threshold");
        add("slow_query_threshold_ms", "1000", "Slow query threshold in ms");
        add("compression",             "OFF",  "Enable page compression");
    }
};

// ── Global singleton ──────────────────────────────────────────
inline RuntimeConfig& g_runtimeConfig() {
    static RuntimeConfig cfg;
    return cfg;
}

} // namespace milansql
