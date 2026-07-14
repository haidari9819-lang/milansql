#pragma once
#include <string>
#include <deque>
#include <vector>
#include <ctime>
#include <fstream>
#include <functional>
#include "../utils/date_utils.hpp"

namespace milansql {

enum class AuditLevel { OFF, DDL, DML, ALL };

inline std::string auditLevelToString(AuditLevel lv) {
    switch (lv) {
    case AuditLevel::OFF: return "OFF";
    case AuditLevel::DDL: return "DDL";
    case AuditLevel::DML: return "DML";
    case AuditLevel::ALL: return "ALL";
    }
    return "DML";
}

inline AuditLevel auditLevelFromString(const std::string& s) {
    if (s == "OFF") return AuditLevel::OFF;
    if (s == "DDL") return AuditLevel::DDL;
    if (s == "ALL") return AuditLevel::ALL;
    return AuditLevel::DML;
}

inline bool isDdlOp(const std::string& op) {
    return op == "CREATE_TABLE" || op == "DROP_TABLE" || op == "ALTER_TABLE"
        || op == "CREATE_INDEX" || op == "DROP_INDEX";
}
inline bool isDmlOp(const std::string& op) {
    return op == "INSERT" || op == "UPDATE" || op == "DELETE" || op == "TRUNCATE";
}

// Simple hash pseudonym for GDPR anonymization
inline std::string hashPseudonym(const std::string& user) {
    unsigned long h = 5381;
    for (char c : user) h = h * 33 + (unsigned char)c;
    char buf[32];
    snprintf(buf, sizeof(buf), "user_%08lx", h & 0xFFFFFFFF);
    return std::string(buf);
}

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
    bool        enabled_    = false;
    std::string logFile_;
    AuditLevel  level_      = AuditLevel::DML;
    bool        anonymize_  = false;
    bool        rotation_   = false;
    static const size_t MAX_ENTRIES = 10000;

    void rotateIfNeeded() {
        if (!rotation_ || logFile_.empty()) return;
        if (buffer_.size() < MAX_ENTRIES) return;
        // Write oldest 1000 entries to file, then remove them
        std::ofstream ofs(logFile_, std::ios::app);
        if (!ofs.is_open()) return;
        size_t toWrite = 1000;
        if (toWrite > buffer_.size()) toWrite = buffer_.size();
        for (size_t i = 0; i < toWrite; ++i) {
            const auto& e = buffer_[i];
            ofs << e.timestamp << "\t" << e.user << "\t" << e.ip << "\t"
                << e.op << "\t" << e.table << "\t" << e.rows << "\t"
                << e.duration << "\n";
        }
        ofs.close();
        for (size_t i = 0; i < toWrite; ++i) buffer_.pop_front();
    }

    bool shouldLog(const std::string& op) const {
        if (level_ == AuditLevel::OFF) return false;
        if (level_ == AuditLevel::ALL) return true;
        if (level_ == AuditLevel::DDL) return isDdlOp(op);
        // DML: DDL + DML ops
        return isDdlOp(op) || isDmlOp(op);
    }

public:
    bool isEnabled() const { return enabled_; }
    void setEnabled(bool on) { enabled_ = on; }
    void setLogFile(const std::string& f) { logFile_ = f; }
    std::string getLogFile() const { return logFile_; }
    AuditLevel getLevel() const { return level_; }
    void setLevel(AuditLevel lv) { level_ = lv; }
    bool isAnonymize() const { return anonymize_; }
    void setAnonymize(bool on) { anonymize_ = on; }
    bool isRotation() const { return rotation_; }
    void setRotation(bool on) { rotation_ = on; }
    size_t entryCount() const { return buffer_.size(); }

    void log(AuditEntry entry) {
        if (!enabled_) return;
        if (!shouldLog(entry.op)) return;
        if (entry.timestamp.empty()) {
            time_t t = time(nullptr);
            char buf[24]; std::tm ti = milansql::safe_localtime(&t);
            strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &ti);
            entry.timestamp = buf;
        }
        if (anonymize_) {
            entry.user = hashPseudonym(entry.user);
        }
        rotateIfNeeded();
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

    // GDPR: Delete entries for a specific user
    void deleteByUser(const std::string& user) {
        std::deque<AuditEntry> filtered;
        for (auto& e : buffer_) {
            if (e.user != user) filtered.push_back(std::move(e));
        }
        buffer_ = std::move(filtered);
    }

    // GDPR: Delete entries matching a field/value
    void deleteWhere(const std::string& field, const std::string& value) {
        std::deque<AuditEntry> filtered;
        for (auto& e : buffer_) {
            bool match = false;
            if      (field == "op"    && e.op    == value) match = true;
            else if (field == "user"  && e.user  == value) match = true;
            else if (field == "table" && e.table == value) match = true;
            else if (field == "ip"    && e.ip    == value) match = true;
            if (!match) filtered.push_back(std::move(e));
        }
        buffer_ = std::move(filtered);
    }
};

} // namespace milansql
