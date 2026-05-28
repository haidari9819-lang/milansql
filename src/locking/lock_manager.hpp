#pragma once

#include <string>
#include <map>
#include <vector>
#include <mutex>
#include <thread>
#include <sstream>

// ============================================================
// lock_manager.hpp — MilanSQL Phase 65
// Row-level + Table-level locking für SELECT FOR UPDATE / LOCK TABLE
// ============================================================

namespace milansql {

enum class LockType { READ, WRITE };

class LockManager {
public:
    // Acquire a row-level WRITE lock for the current thread.
    // Returns true if acquired, false if another thread already holds it.
    bool acquireRowLock(const std::string& table, const std::string& rowKey) {
        std::string key = table + ":" + rowKey;
        auto tid = std::this_thread::get_id();
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = rowLocks_.find(key);
        if (it == rowLocks_.end()) {
            rowLocks_[key] = tid;
            return true;
        }
        return it->second == tid;  // same thread re-acquires
    }

    // Acquire a table-level READ or WRITE lock for the current thread.
    // Returns true if acquired, false if another thread holds an incompatible lock.
    bool acquireTableLock(const std::string& table, LockType type) {
        auto tid = std::this_thread::get_id();
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = tableLocks_.find(table);
        if (it == tableLocks_.end()) {
            tableLocks_[table] = {tid, type};
            return true;
        }
        if (it->second.threadId == tid) {
            it->second.type = type;  // upgrade lock type if same thread
            return true;
        }
        // Two concurrent READs are compatible
        if (type == LockType::READ && it->second.type == LockType::READ) {
            tableLocks_[table] = {tid, type};
            return true;
        }
        return false;  // incompatible lock held by another thread
    }

    // Release all row locks held by the given thread.
    void releaseRowLocks(std::thread::id tid) {
        std::lock_guard<std::mutex> lk(mutex_);
        for (auto it = rowLocks_.begin(); it != rowLocks_.end(); ) {
            if (it->second == tid) it = rowLocks_.erase(it);
            else ++it;
        }
    }

    // Release all table locks held by the given thread.
    void releaseTableLocks(std::thread::id tid) {
        std::lock_guard<std::mutex> lk(mutex_);
        for (auto it = tableLocks_.begin(); it != tableLocks_.end(); ) {
            if (it->second.threadId == tid) it = tableLocks_.erase(it);
            else ++it;
        }
    }

    // Release all locks (row + table) held by the given thread.
    void releaseAllLocks(std::thread::id tid) {
        std::lock_guard<std::mutex> lk(mutex_);
        for (auto it = rowLocks_.begin(); it != rowLocks_.end(); ) {
            if (it->second == tid) it = rowLocks_.erase(it);
            else ++it;
        }
        for (auto it = tableLocks_.begin(); it != tableLocks_.end(); ) {
            if (it->second.threadId == tid) it = tableLocks_.erase(it);
            else ++it;
        }
    }

    // Returns true if a DML write on `table` is allowed for the current thread:
    // no READ or WRITE lock held by another thread.
    bool checkWriteAllowed(const std::string& table) {
        auto tid = std::this_thread::get_id();
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = tableLocks_.find(table);
        if (it == tableLocks_.end()) return true;
        return it->second.threadId == tid;
    }

    // Returns true if a SELECT on `table` is allowed:
    // no WRITE lock held by another thread.
    bool checkReadAllowed(const std::string& table) {
        auto tid = std::this_thread::get_id();
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = tableLocks_.find(table);
        if (it == tableLocks_.end()) return true;
        if (it->second.type == LockType::READ) return true;
        return it->second.threadId == tid;
    }

    // SHOW LOCKS — returns human-readable lines for all active locks.
    std::vector<std::string> showLocks() const {
        std::lock_guard<std::mutex> lk(mutex_);
        std::vector<std::string> lines;
        for (const auto& kv : rowLocks_) {
            std::ostringstream oss;
            oss << "ROW  | " << kv.first << " | WRITE | thread:" << kv.second;
            lines.push_back(oss.str());
        }
        for (const auto& kv : tableLocks_) {
            std::ostringstream oss;
            oss << "TBL  | " << kv.first << " | "
                << (kv.second.type == LockType::WRITE ? "WRITE" : "READ ")
                << " | thread:" << kv.second.threadId;
            lines.push_back(oss.str());
        }
        return lines;
    }

private:
    struct TableLockEntry {
        std::thread::id threadId;
        LockType        type;
    };

    mutable std::mutex                           mutex_;
    std::map<std::string, std::thread::id>       rowLocks_;    // "table:rowKey" → threadId
    std::map<std::string, TableLockEntry>        tableLocks_;  // tableName → {threadId, type}
};

inline LockManager g_lockManager;

} // namespace milansql
