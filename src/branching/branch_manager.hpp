#pragma once
// ============================================================
// branch_manager.hpp — Database Branching v12.0.3 (File-Based Isolation)
// Each branch is a copy of database.milan under branches/<name>/database.milan
// CREATE BRANCH  → save + mkdir + copy file
// USE BRANCH     → save current + clearAll + setPath + reload
// MERGE BRANCH   → load src into temp engine → LWW into dst
// DROP BRANCH    → rm -rf branches/<name>/
// ============================================================

#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <filesystem>
#include <fstream>
#include <stdexcept>

// storage.hpp includes engine.hpp; both are already in the include path
#include "storage/storage.hpp"
#include "engine/engine.hpp"

namespace milansql {

struct BranchInfo {
    std::string name;
    std::string parent;
    std::string created_at;
    std::string status; // "active", "merged", "dropped"
};

// Lightweight snapshot of one table's schema + data (for merge)
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

    // Call once from MilanHttpServer::initEngine() after loading the database
    void init(MilanBinaryStorage* storage, Engine* engine, const std::string& mainPath) {
        storage_  = storage;
        engine_   = engine;
        mainPath_ = mainPath;
        currentBranch_ = "main"; // always start on main after restart

        // Auto-discover existing branch directories so SHOW BRANCHES
        // reflects filesystem state across server restarts.
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::path branchesDir = "branches";
        if (fs::exists(branchesDir, ec) && fs::is_directory(branchesDir, ec)) {
            for (const auto& entry : fs::directory_iterator(branchesDir, ec)) {
                if (!entry.is_directory()) continue;
                std::string brName = entry.path().filename().string();
                if (brName.empty() || brName == "main") continue;
                if (branches_.count(brName)) continue; // already known
                // Check if branch db file exists
                fs::path dbFile = entry.path() / "database.milan";
                if (!fs::exists(dbFile, ec)) continue;
                BranchInfo bi;
                bi.name   = brName;
                bi.parent = "main";
                bi.status = "active";
                bi.created_at = "restored";
                branches_[brName] = bi;
            }
        }
    }

    std::string currentBranch() const { return currentBranch_; }

    bool hasBranch(const std::string& name) const {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = branches_.find(name);
        return (it != branches_.end() && it->second.status == "active");
    }

    // ── CREATE BRANCH name FROM from ─────────────────────────
    bool createBranch(const std::string& name, const std::string& from,
                      std::string& error, Engine* fb = nullptr) {
        std::lock_guard<std::mutex> lk(mu_);
        if (name == "main") {
            error = "Cannot create branch named 'main'";
            return false;
        }
        {
            auto it = branches_.find(name);
            if (it != branches_.end() && it->second.status == "active") {
                error = "Branch '" + name + "' already exists";
                return false;
            }
        }
        {
            auto fit = branches_.find(from);
            if (fit == branches_.end() || fit->second.status != "active") {
                error = "Source branch '" + from + "' does not exist";
                return false;
            }
        }

        // --- In-memory fallback (tests / no init()) ---
        if (!storage_) {
            Engine* eng = fb ? fb : engine_;
            if (!eng) { error = "BranchManager not initialized"; return false; }
            snapshots_[name] = (from == "main" || !snapshots_.count(from))
                ? snapshotEngine(*eng)
                : snapshots_[from];
            BranchInfo bi2;
            bi2.name = name; bi2.parent = from;
            bi2.created_at = nowString(); bi2.status = "active";
            branches_[name] = bi2;
            return true;
        }
        // Save current engine state to the current branch's file
        try { storage_->save(*engine_); } catch (...) {}

        // Source file to copy from
        std::string srcPath = branchFilePath(from);
        std::string dstPath = branchFilePath(name);

        // Create destination directory
        try {
            namespace fs = std::filesystem;
            fs::create_directories(fs::path(dstPath).parent_path());
            fs::copy_file(srcPath, dstPath, fs::copy_options::overwrite_existing);
        } catch (const std::exception& ex) {
            error = "File copy failed: " + std::string(ex.what());
            return false;
        }

        BranchInfo bi;
        bi.name       = name;
        bi.parent     = from;
        bi.created_at = nowString();
        bi.status     = "active";
        branches_[name] = bi;
        return true;
    }

    // ── USE BRANCH name ───────────────────────────────────────
    bool useBranch(const std::string& name, std::string& error, Engine* fb = nullptr) {
        if (!hasBranchNoLock(name)) {
            error = "Branch '" + name + "' does not exist";
            return false;
        }
        // --- In-memory fallback ---
        {
            Engine* eng = fb ? fb : engine_;
            if (!storage_ && eng) {
                if (currentBranch_ != "main" && currentBranch_ != name)
                    snapshots_[currentBranch_] = snapshotEngine(*eng);
                if (name == "main") {
                    if (snapshots_.count("__main_snap__")) {
                        eng->clearAllTables();
                        restoreSnapshot(snapshots_["__main_snap__"], *eng);
                    }
                } else if (snapshots_.count(name)) {
                    if (currentBranch_ == "main")
                        snapshots_["__main_snap__"] = snapshotEngine(*eng);
                    eng->clearAllTables();
                    restoreSnapshot(snapshots_[name], *eng);
                }
                currentBranch_ = name;
                return true;
            }
        }
        if (!storage_ || !engine_) {
            error = "BranchManager not initialized";
            return false;
        }

        // Save current state to current branch's file
        try { storage_->save(*engine_); } catch (...) {}

        // Switch to new branch file
        std::string newPath = branchFilePath(name);

        // Verify the branch file exists (it might not if we just created the branch
        // but the copy failed silently — double-check)
        {
            std::ifstream check(newPath);
            if (!check.good()) {
                // Try to copy from parent or main as fallback
                std::string fromPath = mainPath_;
                auto it = branches_.find(name);
                if (it != branches_.end() && !it->second.parent.empty()) {
                    fromPath = branchFilePath(it->second.parent);
                }
                try {
                    namespace fs = std::filesystem;
                    fs::create_directories(fs::path(newPath).parent_path());
                    fs::copy_file(fromPath, newPath, fs::copy_options::overwrite_existing);
                } catch (...) {}
            }
        }

        // Clear engine and reload from branch file
        engine_->clearAllTables();
        storage_->setPath(newPath);
        try {
            storage_->loadWithCount(*engine_);
            for (const auto& [tn, tbl] : engine_->getTables()) {
            }
        } catch (const std::exception& ex) {
            error = "Failed to load branch file: " + std::string(ex.what());
            // Restore main as fallback
            storage_->setPath(mainPath_);
            try { storage_->loadWithCount(*engine_); } catch (...) {}
            currentBranch_ = "main";
            return false;
        }

        currentBranch_ = name;
        return true;
    }

    // ── MERGE BRANCH src INTO dst ─────────────────────────────
    // LWW: tables from src overwrite tables in dst.
    bool mergeBranch(const std::string& src, const std::string& dst,
                     std::string& error, Engine* fb = nullptr) {
        std::lock_guard<std::mutex> lk(mu_);
        {
            auto sit = branches_.find(src);
            if (sit == branches_.end() || sit->second.status != "active") {
                error = "Source branch '" + src + "' does not exist";
                return false;
            }
        }
        {
            auto dit = branches_.find(dst);
            if (dit == branches_.end() || dit->second.status != "active") {
                error = "Destination branch '" + dst + "' does not exist";
                return false;
            }
        }
        // --- In-memory fallback ---
        {
            Engine* eng = fb ? fb : engine_;
            if (!storage_ && eng) {
                if (dst == "main" || dst == currentBranch_) {
                    if (snapshots_.count(src))
                        restoreSnapshot(snapshots_[src], *eng);
                } else if (snapshots_.count(src) && snapshots_.count(dst)) {
                    for (auto& kv : snapshots_[src])
                        snapshots_[dst][kv.first] = kv.second;
                }
                branches_[src].status = "merged";
                snapshots_.erase(src);
                return true;
            }
        }
        if (!storage_ || !engine_) {
            error = "BranchManager not initialized";
            return false;
        }

        std::string srcPath = branchFilePath(src);
        std::string dstPath = branchFilePath(dst);

        if (dst == currentBranch_) {
            // Merging into the currently active branch: load src into temp engine,
            // then apply LWW into the live engine.
            Engine tmpEng;
            MilanBinaryStorage tmpStorage(srcPath);
            try {
                tmpStorage.loadWithCount(tmpEng);
            } catch (const std::exception& ex) {
                error = "Cannot load source branch: " + std::string(ex.what());
                return false;
            }
            // LWW: for each table in src, overwrite in dst engine
            applyLWW(tmpEng, *engine_);
            // Persist the updated dst
            try { storage_->save(*engine_); } catch (...) {}
        } else {
            // dst is not active: both are files, do file-based LWW
            Engine tmpSrc, tmpDst;
            MilanBinaryStorage storageSrc(srcPath), storageDst(dstPath);
            try { storageSrc.loadWithCount(tmpSrc); } catch (const std::exception& ex) {
                error = "Cannot load source branch: " + std::string(ex.what());
                return false;
            }
            try { storageDst.loadWithCount(tmpDst); } catch (const std::exception& ex) {
                error = "Cannot load destination branch: " + std::string(ex.what());
                return false;
            }
            applyLWW(tmpSrc, tmpDst);
            try { storageDst.save(tmpDst); } catch (const std::exception& ex) {
                error = "Cannot save merged branch: " + std::string(ex.what());
                return false;
            }
        }

        branches_[src].status = "merged";
        return true;
    }

    // ── DROP BRANCH name ──────────────────────────────────────
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
        if (name == currentBranch_) {
            error = "Cannot drop the currently active branch; switch to another branch first";
            return false;
        }
        // Delete branch directory
        try {
            namespace fs = std::filesystem;
            std::string branchDir = "branches/" + name;
            fs::remove_all(branchDir);
        } catch (const std::exception& ex) {
            error = "Failed to delete branch files: " + std::string(ex.what());
            return false;
        }
        it->second.status = "dropped";
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

    // Legacy: snapshot helpers (kept for compatibility)
    static std::map<std::string, TableSnapshot> snapshotEngine(const Engine& engine) {
        std::map<std::string, TableSnapshot> snap;
        for (auto& kv : engine.getTables()) {
            TableSnapshot ts;
            ts.tableName = kv.first;
            ts.columns   = kv.second.columns();
            for (auto& r : kv.second.rows()) {
                if (r.xmax == 0) ts.rows.push_back(r);
            }
            snap[kv.first] = std::move(ts);
        }
        return snap;
    }

    static void restoreSnapshot(const std::map<std::string, TableSnapshot>& snap,
                                Engine& engine) {
        for (auto& kv : snap) {
            auto& ts = kv.second;
            bool exists = engine.getTables().count(ts.tableName) > 0;
            if (!exists) {
                try {
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

private:
    mutable std::mutex mu_;
    std::map<std::string, BranchInfo> branches_;
    std::string currentBranch_ = "main";

    // In-memory snapshots (fallback when storage_ not initialized)
    std::map<std::string, std::map<std::string, TableSnapshot>> snapshots_;

    // Set by init()
    MilanBinaryStorage* storage_ = nullptr;
    Engine*             engine_  = nullptr;
    std::string         mainPath_;

    // ── Helpers ──────────────────────────────────────────────
    std::string branchFilePath(const std::string& name) const {
        if (name == "main") return mainPath_;
        return "branches/" + name + "/database.milan";
    }

    bool hasBranchNoLock(const std::string& name) const {
        auto it = branches_.find(name);
        return (it != branches_.end() && it->second.status == "active");
    }

    // Apply LWW: for each table in src, overwrite/create in dst
    static void applyLWW(const Engine& src, Engine& dst) {
        auto snap = snapshotEngine(src);
        restoreSnapshot(snap, dst);
    }

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
