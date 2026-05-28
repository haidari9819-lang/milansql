#pragma once
// ============================================================
// binlog.hpp — Binary Log Writer/Reader for MilanSQL
// Phase 59: Master/Slave Replication
//
// Format (one entry per line):
//   <pos>|<YYYY-MM-DDTHH:MM:SS>|<sql statement>
// ============================================================

#include <string>
#include <vector>
#include <fstream>
#include <mutex>
#include <chrono>
#include <sstream>
#include <ctime>
#include <stdexcept>

namespace milansql {

struct BinlogEntry {
    long long   pos;
    std::string timestamp;
    std::string sql;
};

class BinlogWriter {
public:
    explicit BinlogWriter(const std::string& path = "database.binlog")
        : path_(path), pos_(0)
    {
        // Count existing entries to resume position
        std::ifstream f(path_);
        if (!f) return;
        std::string line;
        while (std::getline(f, line)) {
            if (!line.empty()) ++pos_;
        }
    }

    // Append one SQL statement; returns the new position.
    long long write(const std::string& sql) {
        std::lock_guard<std::mutex> lk(mu_);
        ++pos_;
        std::ofstream f(path_, std::ios::app);
        if (!f) throw std::runtime_error("Cannot open binlog: " + path_);
        f << pos_ << '|' << currentTimestamp() << '|' << sql << '\n';
        f.flush();
        return pos_;
    }

    long long getCurrentPos() const {
        std::lock_guard<std::mutex> lk(mu_);
        return pos_;
    }

    // Return all entries with pos > fromPos (for slave sync).
    std::vector<BinlogEntry> readFrom(long long fromPos) const {
        std::lock_guard<std::mutex> lk(mu_);
        return parseFile(fromPos);
    }

    // Return last n entries (for SHOW BINLOG).
    std::vector<BinlogEntry> readLast(int n) const {
        std::lock_guard<std::mutex> lk(mu_);
        auto all = parseFile(0);
        if (n > 0 && static_cast<int>(all.size()) > n)
            all.erase(all.begin(), all.end() - n);
        return all;
    }

private:
    std::string        path_;
    mutable std::mutex mu_;
    long long          pos_;

    std::vector<BinlogEntry> parseFile(long long fromPos) const {
        std::vector<BinlogEntry> result;
        std::ifstream f(path_);
        if (!f) return result;
        std::string line;
        while (std::getline(f, line)) {
            if (line.empty()) continue;
            auto p1 = line.find('|');
            if (p1 == std::string::npos) continue;
            auto p2 = line.find('|', p1 + 1);
            if (p2 == std::string::npos) continue;
            long long pos = 0;
            try { pos = std::stoll(line.substr(0, p1)); }
            catch (...) { continue; }
            if (pos > fromPos) {
                BinlogEntry e;
                e.pos       = pos;
                e.timestamp = line.substr(p1 + 1, p2 - p1 - 1);
                e.sql       = line.substr(p2 + 1);
                result.push_back(std::move(e));
            }
        }
        return result;
    }

    static std::string currentTimestamp() {
        auto now = std::chrono::system_clock::now();
        auto t   = std::chrono::system_clock::to_time_t(now);
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
        return std::string(buf);
    }
};

} // namespace milansql
