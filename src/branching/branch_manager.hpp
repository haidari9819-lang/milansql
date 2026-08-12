#pragma once
// ============================================================
// branch_manager.hpp — Database Branching (Phase 3.1)
// Git-like branching: snapshot of columns+rows per branch.
// Table is move-only, so we store column defs + rows separately.
// "main" branch = live engine tables (cannot be dropped).
// ============================================================

#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <chrono>
#include <sstream>
#include <iomanip>

#include "engine/engine.hpp"

namespace milansql {

struct BranchInfo {
    std::string name;
    std::string parent;
    std::string created_at;
    std::string status; // "active", "merged", "dropped"
};

// Lightweight snapshot of one table's schema + data
struct TableSnapshot {
    std::string              tableName;
    std::vector<Column>      columns;
    std::vector<Row>         rows;
};

class BranchManager {
public:
    static BranchManager& global() {
        static BranchManager inst;
        return inst;
    }

    BranchManager() {
        BranchInfo main_branch;
        main_branch.name       = "main";
        main_branch.parent     = "";
        main_branch.created_at = nowString();
        main_branch.status     = "active";
        branches_["main"] = main_branch;
    }

    // Take a snapshot of the engine's tables (columns + rows)
    static std::map<std::string, TableSnapshot> snapshotEngine(const Engine& engine) {
        std::map<std::string, TableSnapshot> snap;
        for (auto& kv : engine.getTables()) {
            TableSnapshot ts;
            ts.tableName = kv.first;
            ts.columns   = kv.second.columns();
            // Copy visible rows (xmax==0 = alive)
            for (auto& r : kv.second.rows()) {
                if (r.xmax == 0) {
                    ts.rows.push_back(r);
                }
            }
            snap[kv.first] = std::move(ts);
        }
        return snap;
    }

    // Restore snapshot tables back into engine
    static void restoreSnapshot(const std::map<std::string, TableSnapshot>& snap,
                                Engine& engine) {
        for (auto& kv : snap) {
            auto& ts = kv.second;
            // Create table if it doesn't exist, otherwise truncate
            bool exists = engine.getTables().count(ts.tableName) > 0;
            if (!exists) {
                try {
                    // Copy columns (Column is copyable)
                    std::vector<Column> cols = ts.columns;
                    engine.createTable(ts.tableName, std::move(cols), {}, "");
                } catch (...) {}
            } else {
                try { engine.truncateTable(ts.tableName); } catch (...) {}
            }
            for (auto& row : ts.rows) {
                try { engine.insertRow(ts.tableName, row.values); } catch (...) {}
            }
        }
    }

    bool createBranch(const std::string& name, const std::string& from,
                      const Engine& engine, std::string& error) {
        std::lock_guard<std::mutex> lk(mu_);
        if (name == "main") {
            error = "Cannot create branch named 'main'";
            return false;
        }
        auto it = branches_.find(name);
        if (it != branches_.end() && it->second.status == "active") {
            error = "Branch '" + name + "' already exists";
            return false;
        }
        auto fit = branches_.find(from);
        if (fit == branches_.end() || fit->second.status != "active") {
            error = "Source branch '" + from + "' does not exist";
            return false;
        }

        BranchInfo bi;
        bi.name       = name;
        bi.parent     = from;
        bi.created_at = nowString();
        bi.status     = "active";
        branches_[name] = bi;

        if (from == "main") {
            snapshots_[name] = snapshotEngine(engine);
        } else if (snapshots_.count(from)) {
            snapshots_[name] = snapshots_[from];
        } else {
            snapshots_[name] = snapshotEngine(engine);
        }
        return true;
    }

    bool dropBranch(const std::string& name, std::string& error) {
        std::lock_guard<std::mutex> lk(mu_);
        if (name == "main") {
            error = "Cannot drop branch 'main'";
            return false;
        }
        auto it = branches_.find(name);
        if (it == branches_.end() || it->second.status != "active") {
            error = "Branch '" + name + "' does not exist";
            return false;
        }
        it->second.status = "dropped";
        snapshots_.erase(name);
        return true;
    }

    bool mergeBranch(const std::string& src, const std::string& dst,
                     Engine& engine, std::string& error) {
        std::lock_guard<std::mutex> lk(mu_);
        auto sit = branches_.find(src);
        if (sit == branches_.end() || sit->second.status != "active") {
            error = "Source branch '" + src + "' does not exist";
            return false;
        }
        auto dit = branches_.find(dst);
        if (dit == branches_.end() || dit->second.status != "active") {
            error = "Destination branch '" + dst + "' does not exist";
            return false;
        }

        if (dst == "main") {
            if (snapshots_.count(src)) {
                restoreSnapshot(snapshots_[src], engine);
            }
        } else {
            if (snapshots_.count(src) && snapshots_.count(dst)) {
                for (auto& kv : snapshots_[src]) {
                    snapshots_[dst][kv.first] = kv.second;
                }
            }
        }

        sit->second.status = "merged";
        snapshots_.erase(src);
        return true;
    }

    std::vector<BranchInfo> listBranches() const {
        std::lock_guard<std::mutex> lk(mu_);
        std::vector<BranchInfo> result;
        for (auto& kv : branches_) {
            if (kv.second.status != "dropped") {
                result.push_back(kv.second);
            }
        }
        return result;
    }

    bool hasBranch(const std::string& name) const {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = branches_.find(name);
        return (it != branches_.end() && it->second.status == "active");
    }

    std::string currentBranch() const {
        return currentBranch_;
    }

    void useBranch(const std::string& name) {
        currentBranch_ = name;
    }

private:
    mutable std::mutex mu_;
    std::map<std::string, BranchInfo> branches_;
    std::map<std::string, std::map<std::string, TableSnapshot>> snapshots_;
    std::string currentBranch_ = "main";

    static std::string nowString() {
        auto now = std::chrono::system_clock::now();
        auto t   = std::chrono::system_clock::to_time_t(now);
        std::ostringstream oss;
        std::tm tm_buf{};
#ifdef _WIN32
        gmtime_s(&tm_buf, &t);
#else
        gmtime_r(&t, &tm_buf);
#endif
        oss << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S");
        return oss.str();
    }
};

} // namespace milansql
