#pragma once
#include <string>
#include <deque>
#include <vector>
#include <ctime>
#include <fstream>

namespace milansql {

struct AuditEntry {
    std::string timestamp;
    std::string user;
    std::string ip;
    std::string op;
    std::string table;
    int64_t     rows     = 0;
    double      duration = 0.0;
};

class AuditLogger {
    std::deque<AuditEntry> buffer_;
    bool        enabled_  = false;
    std::string logFile_;
    static const size_t MAX_ENTRIES = 10000;

public:
    bool isEnabled() const { return enabled_; }
    void setEnabled(bool on) { enabled_ = on; }
    void setLogFile(const std::string& f) { logFile_ = f; }

    void log(AuditEntry entry) {
        if (!enabled_) return;
        if (entry.timestamp.empty()) {
            time_t t = time(nullptr);
            char buf[24]; struct tm* ti = localtime(&t);
            strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", ti);
            entry.timestamp = buf;
        }
        if (buffer_.size() >= MAX_ENTRIES) buffer_.pop_front();
        buffer_.push_back(std::move(entry));
    }

    void log(const std::string& op, const std::string& table,
             int64_t rows = 0, double dur = 0,
             const std::string& user = "root",
             const std::string& ip   = "127.0.0.1") {
        AuditEntry e;
        e.op = op; e.table = table; e.rows = rows;
        e.duration = dur; e.user = user; e.ip = ip;
        log(std::move(e));
    }

    const std::deque<AuditEntry>& getEntries() const { return buffer_; }

    std::vector<AuditEntry> getEntriesWhere(const std::string& field,
                                             const std::string& value) const {
        std::vector<AuditEntry> res;
        for (const auto& e : buffer_) {
            if      (field == "op"    && e.op    == value) res.push_back(e);
            else if (field == "user"  && e.user  == value) res.push_back(e);
            else if (field == "table" && e.table == value) res.push_back(e);
            else if (field == "ip"    && e.ip    == value) res.push_back(e);
        }
        return res;
    }

    std::vector<AuditEntry> getLimited(int limit) const {
        std::vector<AuditEntry> res;
        size_t start = (buffer_.size() > (size_t)limit)
                       ? buffer_.size() - (size_t)limit : 0;
        for (size_t i = start; i < buffer_.size(); ++i)
            res.push_back(buffer_[i]);
        return res;
    }

    void flush() { buffer_.clear(); }
};

} // namespace milansql
