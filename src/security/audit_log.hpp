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
    std::string op;           // event type: DDL, DML, AUTH, ADMIN, SECURITY
    std::string action;       // specific action: CREATE_TABLE, LOGIN, etc.
    std::string table;        // object name
    int64_t     rows     = 0;
    double      duration = 0.0;
    std::string query;        // full SQL or description
    bool        success  = true;
    std::string prevHash;     // hash-chain
    std::string entryHash;    // pseudohash(prevHash + entry data)
};

// FNV-1a based pseudohash for audit chain (tamper detection)
inline std::string auditSha256(const std::string& data) {
    uint64_t h1 = 0xcbf29ce484222325ULL, h2 = 0x100000001b3ULL;
    for (size_t i = 0; i < data.size(); ++i) {
        h1 ^= (uint8_t)data[i]; h1 *= 0x100000001b3ULL;
        h2 ^= (uint8_t)data[(i * 7 + 3) % data.size()]; h2 *= h1 ^ 0xcbf29ce484222325ULL;
    }
    char buf[33];
    snprintf(buf, sizeof(buf), "%016llx%016llx",
             (unsigned long long)h1, (unsigned long long)h2);
    return std::string(buf, 32);
}

class AuditLogger {
    std::deque<AuditEntry> buffer_;
    bool        enabled_    = false;
    std::string logFile_;
    AuditLevel  level_      = AuditLevel::DML;
    bool        anonymize_  = false;
    bool        rotation_   = false;
    std::string lastHash_;
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
    // Phase 5.2: Log admin/security events
    void logAdmin(const std::string& action, const std::string& user,
                  const std::string& ip, const std::string& object,
                  const std::string& query, bool success = true) {
        if (!enabled_) return;
        AuditEntry e;
        time_t t = time(nullptr);
        char buf[24]; std::tm ti = milansql::safe_localtime(&t);
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &ti);
        e.timestamp = buf;
        e.user      = anonymize_ ? hashPseudonym(user) : user;
        e.ip        = ip;
        e.op        = "ADMIN";
        e.action    = action;
        e.table     = object;
        e.query     = query;
        e.success   = success;
        computeHash_(e);
        if (buffer_.size() >= MAX_ENTRIES) buffer_.pop_front();
        buffer_.push_back(e);
    }

    // Phase 5.2: Verify hash-chain integrity
    struct VerifyResult { bool valid; int checked; int broken; std::string firstBroken; };
    VerifyResult verifyChain() const {
        VerifyResult res{true, 0, 0, ""};
        std::string prevHash = "";
        for (const auto& e : buffer_) {
            ++res.checked;
            std::string data = e.timestamp + e.user + e.ip + e.op + e.action
                             + e.table + e.query + std::to_string(e.rows)
                             + std::to_string((int)(e.duration*1000))
                             + (e.success ? "1" : "0") + prevHash;
            std::string expected = auditSha256(data);
            if (!e.entryHash.empty() && e.entryHash != expected) {
                res.valid = false; ++res.broken;
                if (res.firstBroken.empty()) res.firstBroken = e.timestamp + " " + e.action;
            }
            prevHash = e.entryHash.empty() ? expected : e.entryHash;
        }
        return res;
    }

    // Phase 5.2: Export entries as JSON
    std::string exportJson(int limit = -1) const {
        std::string j = "[";
        int n = 0; bool first = true;
        for (const auto& e : buffer_) {
            if (limit > 0 && n >= limit) break;
            if (!first) j += ",";
            j += std::string(R"({"timestamp":")") + e.timestamp;
            j += std::string(R"(","user":")") + e.user;
            j += std::string(R"(","ip":")") + e.ip;
            j += std::string(R"(","event_type":")") + e.op;
            j += std::string(R"(","action":")") + e.action;
            j += std::string(R"(","object":")") + e.table;
            j += std::string(R"(","query":")") + jsonEscAudit(e.query);
            j += std::string(R"(","success":)") + std::string(e.success ? "true" : "false");
            j += std::string(R"(,"hash":")") + e.entryHash + R"("})";
            first = false; ++n;
        }
        return j + "]";
    }

    const std::deque<AuditEntry>& entries() const { return buffer_; }

private:
    void computeHash_(AuditEntry& e) {
        std::string data = e.timestamp + e.user + e.ip + e.op + e.action
                         + e.table + e.query + std::to_string(e.rows)
                         + std::to_string((int)(e.duration*1000))
                         + (e.success ? "1" : "0") + lastHash_;
        e.prevHash  = lastHash_;
        e.entryHash = auditSha256(data);
        lastHash_   = e.entryHash;
    }

    static std::string jsonEscAudit(const std::string& s) {
        std::string r; r.reserve(s.size() + 8);
        for (unsigned char ch : s) {
            if      (ch == 34) { r += char(92); r += char(34); }
            else if (ch == 92) { r += char(92); r += char(92); }
            else if (ch == 10) { r += char(92); r += char(110); }
            else if (ch == 13) { r += char(92); r += char(114); }
            else r += (char)ch;
        }
        return r;
    }
};

} // namespace milansql