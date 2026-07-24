#pragma once
// ============================================================
// logger/logger.hpp — Structured JSON logging
// Phase 1.2: Structured Logging (Billion-Scale Roadmap)
// ============================================================

#include <string>
#include <fstream>
#include <sstream>
#include <mutex>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <ctime>
#include <filesystem>

namespace milansql {

enum class LogLevel { DEBUG = 0, INFO = 1, WARN = 2, ERROR_LVL = 3 };

struct StructuredLogger {
    std::atomic<int> level{1};  // INFO
    std::atomic<bool> json_format{true};
    std::ofstream log_file;
    std::mutex log_mu;
    std::string log_path = "/var/log/milansql/milansql.log";
    std::string slow_log_path = "/var/log/milansql/slow_queries.log";
    std::ofstream slow_file;
    std::atomic<int> slow_threshold_ms{100};

    void open() {
        std::lock_guard<std::mutex> lk(log_mu);
        try {
            std::filesystem::create_directories("/var/log/milansql");
            log_file.open(log_path, std::ios::app);
            slow_file.open(slow_log_path, std::ios::app);
            if (!log_file.is_open()) throw std::runtime_error("cannot open log");
        } catch (...) {
            try {
                std::filesystem::create_directories("/opt/milansql/logs");
                log_path = "/opt/milansql/logs/milansql.log";
                slow_log_path = "/opt/milansql/logs/slow_queries.log";
                log_file.open(log_path, std::ios::app);
                slow_file.open(slow_log_path, std::ios::app);
            } catch (...) {
                // Fallback: log to stderr only
            }
        }
    }

    std::string now_iso() {
        auto now = std::chrono::system_clock::now();
        auto tt = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count() % 1000;
        std::ostringstream ss;
        struct tm tmBuf;
#if defined(_WIN32)
        gmtime_s(&tmBuf, &tt);
        ss << std::put_time(&tmBuf, "%Y-%m-%dT%H:%M:%S");
#else
        gmtime_r(&tt, &tmBuf);
        ss << std::put_time(&tmBuf, "%Y-%m-%dT%H:%M:%S");
#endif
        ss << "." << std::setfill('0') << std::setw(3) << ms << "Z";
        return ss.str();
    }

    // Escape a string for JSON (simple version)
    static std::string esc(const std::string& s) {
        std::string r;
        r.reserve(s.size() + 4);
        for (unsigned char c : s) {
            if      (c == '"')  r += "\\\"";
            else if (c == '\\') r += "\\\\";
            else if (c == '\n') r += "\\n";
            else if (c == '\r') r += "\\r";
            else if (c == '\t') r += "\\t";
            else if (c < 0x20) { char buf[8]; std::snprintf(buf, sizeof(buf), "\\u%04x", (unsigned)c); r += buf; }
            else r += static_cast<char>(c);
        }
        return r;
    }

    void log(LogLevel lvl, const std::string& msg,
             const std::string& corr_id = "",
             double dur_ms = -1.0,
             const std::string& query_type = "",
             const std::string& table = "",
             int rows = -1,
             const std::string& user = "") {
        if (static_cast<int>(lvl) < level.load()) return;
        static const char* lvl_names[] = {"DEBUG","INFO","WARN","ERROR"};
        std::lock_guard<std::mutex> lk(log_mu);
        std::ostringstream out;
        if (json_format.load()) {
            out << "{\"timestamp\":\"" << now_iso() << "\""
                << ",\"level\":\"" << lvl_names[static_cast<int>(lvl)] << "\""
                << ",\"message\":\"" << esc(msg) << "\"";
            if (!corr_id.empty()) out << ",\"correlation_id\":\"" << esc(corr_id) << "\"";
            if (dur_ms >= 0) out << ",\"duration_ms\":" << std::fixed << std::setprecision(2) << dur_ms;
            if (!query_type.empty()) out << ",\"query_type\":\"" << esc(query_type) << "\"";
            if (!table.empty()) out << ",\"table\":\"" << esc(table) << "\"";
            if (rows >= 0) out << ",\"rows\":" << rows;
            if (!user.empty()) out << ",\"user\":\"" << esc(user) << "\"";
            out << "}\n";
        } else {
            out << now_iso() << " [" << lvl_names[static_cast<int>(lvl)] << "] " << msg;
            if (dur_ms >= 0) out << " (" << std::fixed << std::setprecision(2) << dur_ms << "ms)";
            out << "\n";
        }
        if (log_file.is_open()) log_file << out.str() << std::flush;
        else std::cerr << out.str();
    }

    void log_slow_query(const std::string& sql, double dur_ms,
                        const std::string& user, const std::string& plan = "") {
        if (dur_ms < static_cast<double>(slow_threshold_ms.load())) return;
        std::lock_guard<std::mutex> lk(log_mu);
        std::ostringstream out;
        out << "{\"timestamp\":\"" << now_iso() << "\""
            << ",\"level\":\"WARN\""
            << ",\"message\":\"Slow query\""
            << ",\"duration_ms\":" << std::fixed << std::setprecision(2) << dur_ms
            << ",\"user\":\"" << esc(user) << "\""
            << ",\"sql\":\"" << esc(sql.substr(0, 200)) << "\"";
        if (!plan.empty()) out << ",\"plan\":\"" << esc(plan.substr(0, 100)) << "\"";
        out << "}\n";
        if (slow_file.is_open()) slow_file << out.str() << std::flush;
        else std::cerr << out.str();
    }

    static StructuredLogger& global() {
        static StructuredLogger inst;
        return inst;
    }
};

} // namespace milansql
