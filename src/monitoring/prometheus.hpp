#pragma once
// ============================================================
// monitoring/prometheus.hpp — Prometheus text-format exporter
// Phase 102: Prometheus Metrics
// ============================================================

#include <string>
#include <map>
#include <vector>
#include <mutex>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <cmath>
#include <algorithm>

namespace milansql {

class PrometheusExporter {
public:
    // Increment a counter by 1 (with optional labels like {type="SELECT"})
    void increment(const std::string& name, const std::string& labels = "") {
        std::lock_guard<std::mutex> lk(mu_);
        counters_[makeKey(name, labels)]++;
    }

    // Set a gauge to a value
    void set(const std::string& name, double value, const std::string& labels = "") {
        std::lock_guard<std::mutex> lk(mu_);
        gauges_[makeKey(name, labels)] = value;
    }

    // Observe a value for a histogram (tracks p50/p95/p99)
    void observe(const std::string& name, double value) {
        std::lock_guard<std::mutex> lk(mu_);
        auto& vec = histograms_[name];
        // Insert in sorted position
        auto it = std::lower_bound(vec.begin(), vec.end(), value);
        vec.insert(it, value);
    }

    // Register a metric with help text (optional, called lazily)
    void registerHelp(const std::string& name, const std::string& help, const std::string& type) {
        std::lock_guard<std::mutex> lk(mu_);
        help_[name] = {help, type};
    }

    // Export all metrics in Prometheus text format
    std::string exportMetrics() const {
        std::lock_guard<std::mutex> lk(mu_);
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(6);

        // ── Counters ──────────────────────────────────────────
        // Group by base name
        std::map<std::string, std::vector<std::pair<std::string,long long>>> cGroups;
        for (const auto& [key, val] : counters_) {
            // Extract base name (before '{')
            size_t lb = key.find('{');
            std::string base = (lb == std::string::npos) ? key : key.substr(0, lb);
            cGroups[base].push_back({key, val});
        }
        for (const auto& [base, entries] : cGroups) {
            auto hit = help_.find(base);
            if (hit != help_.end()) {
                oss << "# HELP " << base << " " << hit->second.first << "\n";
                oss << "# TYPE " << base << " " << hit->second.second << "\n";
            } else {
                oss << "# TYPE " << base << " counter\n";
            }
            for (const auto& [key, val] : entries) {
                oss << key << " " << val << "\n";
            }
        }

        // ── Histograms as summary ─────────────────────────────
        for (const auto& [name, sorted] : histograms_) {
            auto hit = help_.find(name);
            if (hit != help_.end()) {
                oss << "# HELP " << name << " " << hit->second.first << "\n";
                oss << "# TYPE " << name << " summary\n";
            } else {
                oss << "# TYPE " << name << " summary\n";
            }
            double p50 = percentile(sorted, 0.50);
            double p95 = percentile(sorted, 0.95);
            double p99 = percentile(sorted, 0.99);
            oss << name << "{quantile=\"0.5\"} "  << p50 << "\n";
            oss << name << "{quantile=\"0.95\"} " << p95 << "\n";
            oss << name << "{quantile=\"0.99\"} " << p99 << "\n";
            oss << name << "_count " << sorted.size() << "\n";
            double sum = 0.0;
            for (double v : sorted) sum += v;
            oss << name << "_sum " << sum << "\n";
        }

        // ── Gauges ────────────────────────────────────────────
        std::map<std::string, std::vector<std::pair<std::string,double>>> gGroups;
        for (const auto& [key, val] : gauges_) {
            size_t lb = key.find('{');
            std::string base = (lb == std::string::npos) ? key : key.substr(0, lb);
            gGroups[base].push_back({key, val});
        }
        for (const auto& [base, entries] : gGroups) {
            auto hit = help_.find(base);
            if (hit != help_.end()) {
                oss << "# HELP " << base << " " << hit->second.first << "\n";
                oss << "# TYPE " << base << " gauge\n";
            } else {
                oss << "# TYPE " << base << " gauge\n";
            }
            for (const auto& [key, val] : entries) {
                oss << key << " " << val << "\n";
            }
        }

        return oss.str();
    }

    // Reset query duration histogram samples (keep last N)
    void trimSamples(int maxSamples = 10000) {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& [name, vec] : histograms_) {
            if ((int)vec.size() > maxSamples) {
                // Keep last maxSamples (already sorted, so keep highest)
                vec.erase(vec.begin(), vec.begin() + (int)vec.size() - maxSamples);
            }
        }
    }

private:
    mutable std::mutex mu_;

    // counters: name+labels → value
    std::map<std::string, long long> counters_;

    // gauges: name+labels → value
    std::map<std::string, double> gauges_;

    // histograms: name → sorted list of observed values
    std::map<std::string, std::vector<double>> histograms_;

    // help text: name → {help, type}
    std::map<std::string, std::pair<std::string,std::string>> help_;

    std::string makeKey(const std::string& name, const std::string& labels) const {
        return labels.empty() ? name : name + labels;
    }

    double percentile(const std::vector<double>& sorted, double p) const {
        if (sorted.empty()) return 0.0;
        size_t idx = static_cast<size_t>(p * static_cast<double>(sorted.size()));
        if (idx >= sorted.size()) idx = sorted.size() - 1;
        return sorted[idx];
    }
};

// Global singleton
inline PrometheusExporter& g_prometheus() {
    static PrometheusExporter inst;
    return inst;
}

// Register default help strings
inline void prometheusRegisterDefaults() {
    g_prometheus().registerHelp("milansql_queries_total",
        "Total SQL queries executed", "counter");
    g_prometheus().registerHelp("milansql_query_duration_ms",
        "Query duration percentiles", "summary");
    g_prometheus().registerHelp("milansql_connections_active",
        "Currently active connections", "gauge");
    g_prometheus().registerHelp("milansql_table_rows",
        "Number of rows per table", "gauge");
    g_prometheus().registerHelp("milansql_uptime_seconds",
        "Server uptime in seconds", "gauge");
    g_prometheus().registerHelp("milansql_buffer_pool_size_mb",
        "Buffer pool size in megabytes", "gauge");
}

} // namespace milansql
