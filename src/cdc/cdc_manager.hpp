#pragma once
// ============================================================
// cdc_manager.hpp — Change Data Capture (Phase 98)
// ============================================================

#include <string>
#include <vector>
#include <map>
#include <set>
#include <sstream>
#include <mutex>
#include <atomic>
#include <chrono>
#include <ctime>
#include <iomanip>

namespace milansql {

struct CdcEvent {
    char        op;           // 'I'=insert, 'U'=update, 'D'=delete
    long long   seq;
    std::string tableName;
    std::string oldData;      // JSON-like: {col:val,...} or "NULL"
    std::string newData;      // JSON-like: {col:val,...} or "NULL"
    std::string ts;           // timestamp string
};

class CdcManager {
public:
    void enableTable(const std::string& table) {
        std::lock_guard<std::mutex> lk(mu_);
        enabled_.insert(table);
    }
    void disableTable(const std::string& table) {
        std::lock_guard<std::mutex> lk(mu_);
        enabled_.erase(table);
    }
    bool isEnabled(const std::string& table) const {
        std::lock_guard<std::mutex> lk(mu_);
        return enabled_.count(table) > 0;
    }

    void recordInsert(const std::string& table,
                      const std::vector<std::string>& colNames,
                      const std::vector<std::string>& values) {
        std::lock_guard<std::mutex> lk(mu_);
        if (!enabled_.count(table)) return;
        CdcEvent ev;
        ev.op        = 'I';
        ev.seq       = nextSeq_++;
        ev.tableName = table;
        ev.oldData   = "NULL";
        ev.newData   = rowToJson(colNames, values);
        ev.ts        = nowStr();
        log_[table].push_back(std::move(ev));
    }

    void recordUpdate(const std::string& table,
                      const std::vector<std::string>& colNames,
                      const std::vector<std::string>& oldValues,
                      const std::vector<std::string>& newValues) {
        std::lock_guard<std::mutex> lk(mu_);
        if (!enabled_.count(table)) return;
        CdcEvent ev;
        ev.op        = 'U';
        ev.seq       = nextSeq_++;
        ev.tableName = table;
        ev.oldData   = rowToJson(colNames, oldValues);
        ev.newData   = rowToJson(colNames, newValues);
        ev.ts        = nowStr();
        log_[table].push_back(std::move(ev));
    }

    void recordDelete(const std::string& table,
                      const std::vector<std::string>& colNames,
                      const std::vector<std::string>& values) {
        std::lock_guard<std::mutex> lk(mu_);
        if (!enabled_.count(table)) return;
        CdcEvent ev;
        ev.op        = 'D';
        ev.seq       = nextSeq_++;
        ev.tableName = table;
        ev.oldData   = rowToJson(colNames, values);
        ev.newData   = "NULL";
        ev.ts        = nowStr();
        log_[table].push_back(std::move(ev));
    }

    std::vector<CdcEvent> readEvents(const std::string& table, long long afterSeq = -1) const {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = log_.find(table);
        if (it == log_.end()) return {};
        if (afterSeq < 0) return it->second;
        std::vector<CdcEvent> result;
        for (const auto& ev : it->second)
            if (ev.seq > afterSeq)
                result.push_back(ev);
        return result;
    }

    std::string showStatus() const {
        std::lock_guard<std::mutex> lk(mu_);
        std::ostringstream oss;
        oss << "CDC Status:\n";
        if (enabled_.empty()) {
            oss << "  No tables with CDC enabled.\n";
        } else {
            for (const auto& t : enabled_) {
                long long evCount = 0;
                auto it = log_.find(t);
                if (it != log_.end())
                    evCount = static_cast<long long>(it->second.size());
                oss << "  " << t << ": CDC ENABLED, " << evCount << " event(s)\n";
            }
        }
        // Also list disabled tables that have events
        for (const auto& [t, evts] : log_) {
            if (!enabled_.count(t)) {
                oss << "  " << t << ": CDC DISABLED, " << evts.size() << " event(s) in log\n";
            }
        }
        return oss.str();
    }

    struct VirtualTable {
        std::vector<std::string> colNames{"op", "seq", "table", "old_data", "new_data", "ts"};
        std::vector<std::vector<std::string>> rows;
    };

    VirtualTable buildVirtualTable(const std::string& tableName, long long afterSeq = -1) const {
        VirtualTable vt;
        auto events = readEvents(tableName, afterSeq);
        for (const auto& ev : events) {
            std::vector<std::string> row;
            row.push_back(std::string(1, ev.op));
            row.push_back(std::to_string(ev.seq));
            row.push_back(ev.tableName);
            row.push_back(ev.oldData);
            row.push_back(ev.newData);
            row.push_back(ev.ts);
            vt.rows.push_back(std::move(row));
        }
        return vt;
    }

private:
    mutable std::mutex mu_;
    std::set<std::string> enabled_;
    std::map<std::string, std::vector<CdcEvent>> log_;
    long long nextSeq_{1};

    static std::string rowToJson(const std::vector<std::string>& cols,
                                  const std::vector<std::string>& vals) {
        std::ostringstream oss;
        oss << "{";
        for (size_t i = 0; i < cols.size(); ++i) {
            if (i > 0) oss << ",";
            oss << cols[i] << ":";
            std::string v = (i < vals.size()) ? vals[i] : "NULL";
            // strip surrounding single quotes for display
            if (v.size() >= 2 && v.front() == '\'' && v.back() == '\'')
                v = v.substr(1, v.size() - 2);
            oss << v;
        }
        oss << "}";
        return oss.str();
    }

    static std::string nowStr() {
        auto now = std::chrono::system_clock::now();
        auto t   = std::chrono::system_clock::to_time_t(now);
        std::ostringstream oss;
        std::tm tm_buf{};
#ifdef _WIN32
        gmtime_s(&tm_buf, &t);
#else
        gmtime_r(&t, &tm_buf);
#endif
        oss << std::put_time(&tm_buf, "%Y-%m-%dT%H:%M:%SZ");
        return oss.str();
    }
};

} // namespace milansql
