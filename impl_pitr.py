#!/usr/bin/env python3
"""Phase 178: Point-in-Time Recovery (PITR)"""

# ============================================================
# STEP 1: Create pitr_manager.hpp
# ============================================================
pitr_hpp = r'''#pragma once
// ============================================================
// pitr_manager.hpp — Phase 178: Point-in-Time Recovery (PITR)
//
// WAL Archiving + Base Backup + Point-in-Time Restore
//
// Components:
//   WalArchiver     — copies WAL segments to archive directory
//   BaseBackup      — hot backup of database files
//   PitrRestore     — replay WAL up to timestamp/txId
//   PitrManager     — orchestrates all PITR operations
// ============================================================

#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <iostream>
#include <cstdint>
#include <ctime>
#include <cstdio>
#include <atomic>
#include <mutex>
#include <filesystem>
#include <algorithm>
#include <chrono>

namespace milansql {

// Forward declarations
class Engine;
struct BufferedOp;

namespace fs = std::filesystem;

// ── Timestamp helpers ──────────────────────────────────────────
inline int64_t pitr_now_epoch() {
    return static_cast<int64_t>(std::time(nullptr));
}

inline std::string pitr_epoch_to_str(int64_t epoch) {
    std::time_t t = static_cast<std::time_t>(epoch);
    char buf[32]{};
    struct tm tmBuf{};
#if defined(_WIN32)
    localtime_s(&tmBuf, &t);
#else
    localtime_r(&t, &tmBuf);
#endif
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmBuf);
    return buf;
}

inline int64_t pitr_str_to_epoch(const std::string& s) {
    struct tm tmBuf{};
    // Parse "YYYY-MM-DD HH:MM:SS"
    if (s.size() >= 19) {
        tmBuf.tm_year = std::stoi(s.substr(0, 4)) - 1900;
        tmBuf.tm_mon  = std::stoi(s.substr(5, 2)) - 1;
        tmBuf.tm_mday = std::stoi(s.substr(8, 2));
        tmBuf.tm_hour = std::stoi(s.substr(11, 2));
        tmBuf.tm_min  = std::stoi(s.substr(14, 2));
        tmBuf.tm_sec  = std::stoi(s.substr(17, 2));
        tmBuf.tm_isdst = -1;
        return static_cast<int64_t>(mktime(&tmBuf));
    }
    return 0;
}

// ── WAL Archive Entry (parsed from archive) ────────────────────
struct WalArchiveEntry {
    std::string filename;     // segment filename
    int64_t     timestamp;    // epoch of archival
    uint64_t    sizeBytes;
    uint64_t    startLsn;
    uint64_t    endLsn;
};

// ── Backup Metadata ────────────────────────────────────────────
struct BackupLabel {
    std::string backupDir;
    std::string timestamp;        // human-readable
    int64_t     epochTime;
    uint64_t    startLsn;
    uint64_t    walPosition;
    std::string version;
    uint64_t    sizeBytes;
    int         tableCount;
};

// ============================================================
// PitrManager — orchestrates WAL archiving, backup, restore
// ============================================================
class PitrManager {
public:
    // ── Configuration ──────────────────────────────────────────
    struct Config {
        bool        archiveEnabled{true};
        std::string archiveDir{"wal_archive"};
        int         retentionDays{30};
        bool        autoBackupEnabled{false};
        std::string autoBackupSchedule{"daily"};
        std::string backupBaseDir{"backups"};
    };

    PitrManager() = default;

    Config& config() { return config_; }
    const Config& config() const { return config_; }

    // ── WAL Archiving ──────────────────────────────────────────
    // Archive the current WAL file to the archive directory
    // Called after checkpoint or periodically
    std::string archiveCurrentWal(const std::string& walFile) {
        std::lock_guard<std::mutex> lk(mu_);
        if (!config_.archiveEnabled) return "WAL archiving is disabled";

        // Ensure archive directory exists
        std::error_code ec;
        fs::create_directories(config_.archiveDir, ec);
        if (ec) return "Failed to create archive dir: " + ec.message();

        // Check WAL file exists and has content
        std::ifstream src(walFile, std::ios::binary | std::ios::ate);
        if (!src || src.tellg() <= 0) return "WAL file empty or missing";
        auto walSize = src.tellg();
        src.seekg(0);

        // Generate archive segment name with timestamp
        int64_t now = pitr_now_epoch();
        std::string segName = "wal_" + std::to_string(now) + ".seg";
        std::string destPath = config_.archiveDir + "/" + segName;

        // Copy WAL to archive
        std::ofstream dst(destPath, std::ios::binary);
        if (!dst) return "Failed to create archive segment: " + destPath;
        dst << src.rdbuf();
        dst.flush();

        ++archivedSegments_;
        totalArchivedBytes_ += static_cast<uint64_t>(walSize);
        lastArchiveTime_ = pitr_epoch_to_str(now);

        return "OK: archived " + std::to_string(walSize) + " bytes to " + segName;
    }

    // Clean up old archive segments based on retention
    int cleanOldArchives() {
        std::lock_guard<std::mutex> lk(mu_);
        if (config_.retentionDays <= 0) return 0;

        int64_t cutoff = pitr_now_epoch() - (config_.retentionDays * 86400LL);
        int removed = 0;

        std::error_code ec;
        if (!fs::exists(config_.archiveDir, ec)) return 0;

        for (auto& entry : fs::directory_iterator(config_.archiveDir, ec)) {
            if (!entry.is_regular_file()) continue;
            std::string fname = entry.path().filename().string();
            // Parse timestamp from filename: wal_<epoch>.seg
            if (fname.size() > 4 && fname.substr(0, 4) == "wal_") {
                auto dotPos = fname.find('.');
                if (dotPos != std::string::npos) {
                    try {
                        int64_t segEpoch = std::stoll(fname.substr(4, dotPos - 4));
                        if (segEpoch < cutoff) {
                            fs::remove(entry.path(), ec);
                            if (!ec) ++removed;
                        }
                    } catch (...) {}
                }
            }
        }
        return removed;
    }

    // List archive segments
    std::vector<WalArchiveEntry> listArchiveSegments() const {
        std::vector<WalArchiveEntry> result;
        std::error_code ec;
        if (!fs::exists(config_.archiveDir, ec)) return result;

        for (auto& entry : fs::directory_iterator(config_.archiveDir, ec)) {
            if (!entry.is_regular_file()) continue;
            std::string fname = entry.path().filename().string();
            if (fname.size() > 4 && fname.substr(0, 4) == "wal_") {
                WalArchiveEntry ae;
                ae.filename = fname;
                ae.sizeBytes = static_cast<uint64_t>(entry.file_size(ec));
                auto dotPos = fname.find('.');
                if (dotPos != std::string::npos) {
                    try { ae.timestamp = std::stoll(fname.substr(4, dotPos - 4)); }
                    catch (...) { ae.timestamp = 0; }
                }
                result.push_back(ae);
            }
        }
        // Sort by timestamp
        std::sort(result.begin(), result.end(),
                  [](const WalArchiveEntry& a, const WalArchiveEntry& b) {
                      return a.timestamp < b.timestamp;
                  });
        return result;
    }

    // Get WAL archive status
    std::string showArchiveStatus() const {
        auto segments = listArchiveSegments();
        uint64_t totalSize = 0;
        for (const auto& s : segments) totalSize += s.sizeBytes;

        std::string out;
        out += "\n  WAL Archive Status\n";
        out += "  ─────────────────────────────────────────\n";
        out += "  Enabled        : " + std::string(config_.archiveEnabled ? "ON" : "OFF") + "\n";
        out += "  Archive Dir    : " + config_.archiveDir + "\n";
        out += "  Retention      : " + std::to_string(config_.retentionDays) + " days\n";
        out += "  Segments       : " + std::to_string(segments.size()) + "\n";
        out += "  Total Size     : " + formatBytes(totalSize) + "\n";
        if (!segments.empty()) {
            out += "  Oldest Segment : " + pitr_epoch_to_str(segments.front().timestamp) + "\n";
            out += "  Newest Segment : " + pitr_epoch_to_str(segments.back().timestamp) + "\n";
        }
        out += "  Last Archive   : " + (lastArchiveTime_.empty() ? "(never)" : lastArchiveTime_) + "\n";
        out += "\n";
        return out;
    }

    // ── Base Backup ────────────────────────────────────────────
    // Create a hot backup: copy database.milan + metadata to backupDir
    std::string createBaseBackup(const std::string& dbFile,
                                  const std::string& backupDir,
                                  uint64_t currentLsn,
                                  const std::string& version,
                                  int tableCount) {
        std::lock_guard<std::mutex> lk(mu_);

        // Create backup directory
        std::error_code ec;
        fs::create_directories(backupDir, ec);
        if (ec) return "ERROR: Failed to create backup dir: " + ec.message();

        // Copy database file
        std::string destDb = backupDir + "/database.milan";
        try {
            fs::copy_file(dbFile, destDb, fs::copy_options::overwrite_existing, ec);
            if (ec) return "ERROR: Failed to copy database: " + ec.message();
        } catch (const std::exception& e) {
            return std::string("ERROR: ") + e.what();
        }

        // Copy related files
        auto tryCopy = [&](const std::string& suffix) {
            std::string src = dbFile + suffix;
            if (fs::exists(src, ec)) {
                fs::copy_file(src, backupDir + "/database.milan" + suffix,
                              fs::copy_options::overwrite_existing, ec);
            }
        };
        tryCopy(".schemas");
        tryCopy(".users");
        tryCopy(".triggers");
        tryCopy(".auth");
        tryCopy(".partitions");
        tryCopy(".partitions.partmeta");

        // Calculate backup size
        uint64_t totalSize = 0;
        for (auto& entry : fs::recursive_directory_iterator(backupDir, ec)) {
            if (entry.is_regular_file()) totalSize += entry.file_size(ec);
        }

        // Write backup_label
        int64_t now = pitr_now_epoch();
        std::string labelPath = backupDir + "/backup_label";
        {
            std::ofstream label(labelPath);
            if (!label) return "ERROR: Failed to write backup_label";
            label << "backup_type:base\n";
            label << "timestamp:" << pitr_epoch_to_str(now) << "\n";
            label << "epoch:" << now << "\n";
            label << "start_lsn:" << currentLsn << "\n";
            label << "version:" << version << "\n";
            label << "table_count:" << tableCount << "\n";
            label << "size_bytes:" << totalSize << "\n";
            label << "backup_dir:" << backupDir << "\n";
        }

        return "OK: Backup created at " + backupDir + " (" +
               formatBytes(totalSize) + ", LSN=" + std::to_string(currentLsn) +
               ", " + pitr_epoch_to_str(now) + ")";
    }

    // List all backups
    std::vector<BackupLabel> listBackups() const {
        std::vector<BackupLabel> result;
        std::error_code ec;
        std::string baseDir = config_.backupBaseDir;
        if (!fs::exists(baseDir, ec)) return result;

        for (auto& entry : fs::directory_iterator(baseDir, ec)) {
            if (!entry.is_directory()) continue;
            std::string labelPath = entry.path().string() + "/backup_label";
            BackupLabel bl = readBackupLabel(labelPath);
            if (!bl.timestamp.empty()) {
                bl.backupDir = entry.path().string();
                result.push_back(bl);
            }
        }
        std::sort(result.begin(), result.end(),
                  [](const BackupLabel& a, const BackupLabel& b) {
                      return a.epochTime < b.epochTime;
                  });
        return result;
    }

    std::string showBackups() const {
        auto backups = listBackups();
        std::string out;
        out += "\n  Backups\n";
        out += "  ─────────────────────────────────────────\n";
        if (backups.empty()) {
            out += "  (no backups found)\n";
        } else {
            for (const auto& b : backups) {
                out += "  " + b.timestamp +
                       "  LSN=" + std::to_string(b.startLsn) +
                       "  " + formatBytes(b.sizeBytes) +
                       "  " + std::to_string(b.tableCount) + " tables" +
                       "  [" + b.backupDir + "]\n";
            }
        }
        out += "  Total: " + std::to_string(backups.size()) + " backups\n\n";
        return out;
    }

    // Delete a specific backup
    std::string deleteBackup(const std::string& backupDir) {
        std::error_code ec;
        if (!fs::exists(backupDir, ec)) return "ERROR: Backup not found: " + backupDir;
        std::string labelPath = backupDir + "/backup_label";
        if (!fs::exists(labelPath, ec)) return "ERROR: Not a valid backup (no backup_label): " + backupDir;
        fs::remove_all(backupDir, ec);
        if (ec) return "ERROR: Failed to delete: " + ec.message();
        return "OK: Backup deleted: " + backupDir;
    }

    // ── Point-in-Time Restore ──────────────────────────────────
    // Returns instructions/status — actual restore modifies engine state.
    // The restore process:
    // 1. Read backup_label from backupDir
    // 2. Load database.milan from backupDir into a fresh engine
    // 3. Collect all WAL archive segments after backup's start_lsn timestamp
    // 4. Replay WAL entries chronologically, stopping at target
    // 5. Return the restored engine state

    struct RestoreResult {
        bool        success{false};
        std::string message;
        int64_t     restoredToEpoch{0};
        uint64_t    replayedTxCount{0};
        uint64_t    replayedOpCount{0};
        uint64_t    skippedTxCount{0};
    };

    // Restore from backup dir, replaying WAL up to targetEpoch
    // If targetEpoch == 0, replay all available WAL
    // If targetTxId != 0, stop at that transaction instead
    RestoreResult restoreToPoint(Engine& engine,
                                  const std::string& backupDir,
                                  int64_t targetEpoch,
                                  uint64_t targetTxId) const;

    // ── Recovery Status ────────────────────────────────────────
    bool isRecovering() const { return recovering_.load(); }
    std::string recoveryProgress() const {
        std::lock_guard<std::mutex> lk(mu_);
        return recoveryProgress_;
    }

private:
    Config config_;
    mutable std::mutex mu_;
    std::atomic<bool> recovering_{false};
    mutable std::string recoveryProgress_;
    uint64_t archivedSegments_{0};
    uint64_t totalArchivedBytes_{0};
    std::string lastArchiveTime_;

    static std::string formatBytes(uint64_t bytes) {
        if (bytes < 1024) return std::to_string(bytes) + " B";
        if (bytes < 1024 * 1024) return std::to_string(bytes / 1024) + " KB";
        if (bytes < 1024ULL * 1024 * 1024) return std::to_string(bytes / (1024 * 1024)) + " MB";
        return std::to_string(bytes / (1024ULL * 1024 * 1024)) + " GB";
    }

    static BackupLabel readBackupLabel(const std::string& path) {
        BackupLabel bl{};
        std::ifstream f(path);
        if (!f) return bl;
        std::string line;
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            auto sep = line.find(':');
            if (sep == std::string::npos) continue;
            std::string key = line.substr(0, sep);
            std::string val = line.substr(sep + 1);
            if (key == "timestamp")    bl.timestamp = val;
            else if (key == "epoch")   { try { bl.epochTime = std::stoll(val); } catch (...) {} }
            else if (key == "start_lsn") { try { bl.startLsn = std::stoull(val); } catch (...) {} }
            else if (key == "version")   bl.version = val;
            else if (key == "size_bytes") { try { bl.sizeBytes = std::stoull(val); } catch (...) {} }
            else if (key == "table_count") { try { bl.tableCount = std::stoi(val); } catch (...) {} }
            else if (key == "backup_dir") bl.backupDir = val;
        }
        return bl;
    }
};

// ── Global singleton ───────────────────────────────────────────
inline PitrManager& g_pitrManager() {
    static PitrManager mgr;
    return mgr;
}

} // namespace milansql

// ── Engine-dependent implementation ────────────────────────────
// Must be included after engine.hpp
#include "../engine/engine.hpp"
#include "wal_recovery.hpp"
#include "../storage/storage.hpp"

namespace milansql {

inline PitrManager::RestoreResult PitrManager::restoreToPoint(
        Engine& engine,
        const std::string& backupDir,
        int64_t targetEpoch,
        uint64_t targetTxId) const {
    RestoreResult result;

    // Read backup label
    BackupLabel label = readBackupLabel(backupDir + "/backup_label");
    if (label.timestamp.empty()) {
        result.message = "ERROR: No valid backup_label found in " + backupDir;
        return result;
    }

    // Load base backup into engine
    std::string dbFile = backupDir + "/database.milan";
    if (!fs::exists(dbFile)) {
        result.message = "ERROR: database.milan not found in backup";
        return result;
    }

    {
        std::lock_guard<std::mutex> lk(mu_);
        recovering_.store(true);
        recoveryProgress_ = "Loading base backup from " + backupDir;
    }

    // Clear engine and load backup
    try {
        MilanStorage storage(dbFile);
        // Reset engine state
        Engine freshEngine;
        storage.loadWithCount(freshEngine);

        // Get all WAL archive segments sorted by timestamp
        auto segments = listArchiveSegments();

        // Filter: only segments after backup's epoch
        std::vector<WalArchiveEntry> relevantSegments;
        for (const auto& seg : segments) {
            if (seg.timestamp >= label.epochTime) {
                relevantSegments.push_back(seg);
            }
        }

        // Also include the current WAL file if it exists
        // (not yet archived)

        {
            std::lock_guard<std::mutex> lk(mu_);
            recoveryProgress_ = "Replaying " + std::to_string(relevantSegments.size()) +
                                " WAL segments...";
        }

        // Replay each WAL segment
        WalRecovery walRecovery;
        for (const auto& seg : relevantSegments) {
            std::string segPath = config_.archiveDir + "/" + seg.filename;

            // Scan WAL entries from this segment
            auto txList = walRecovery.scanWal(segPath);

            for (const auto& tx : txList) {
                if (!tx.committed) continue;

                // Check timestamp constraint (from TX_BEGIN timestamp in WAL)
                // We parse the TS: lines embedded in the WAL
                int64_t txEpoch = parseTxTimestamp(segPath, tx.txId);
                if (txEpoch == 0) txEpoch = seg.timestamp;  // fallback

                // Stop conditions
                if (targetEpoch > 0 && txEpoch > targetEpoch) {
                    ++result.skippedTxCount;
                    continue;
                }
                if (targetTxId > 0 && tx.txId > targetTxId) {
                    ++result.skippedTxCount;
                    continue;
                }

                // Replay this transaction
                for (const auto& opBlock : tx.opBlocks) {
                    BufferedOp op;
                    if (walRecovery.parseOpBlock(opBlock, op)) {
                        try {
                            freshEngine.applyBufferedOp(op);
                            ++result.replayedOpCount;
                        } catch (...) {
                            // Skip failed ops (table may not exist at this point)
                        }
                    }
                }
                ++result.replayedTxCount;
                result.restoredToEpoch = txEpoch;
            }
        }

        // Replace the engine state
        engine = std::move(freshEngine);

        result.success = true;
        if (targetEpoch > 0)
            result.message = "OK: Restored to " + pitr_epoch_to_str(targetEpoch);
        else if (targetTxId > 0)
            result.message = "OK: Restored to transaction " + std::to_string(targetTxId);
        else
            result.message = "OK: Restored from backup with all available WAL";
        result.message += " (" + std::to_string(result.replayedTxCount) + " transactions, " +
                          std::to_string(result.replayedOpCount) + " operations replayed, " +
                          std::to_string(result.skippedTxCount) + " skipped)";

    } catch (const std::exception& e) {
        result.message = std::string("ERROR: Restore failed: ") + e.what();
    }

    {
        std::lock_guard<std::mutex> lk(mu_);
        recovering_.store(false);
        recoveryProgress_ = result.message;
    }
    return result;
}

// Parse TX timestamp from WAL segment file
inline int64_t PitrManager::parseTxTimestamp(const std::string& segPath,
                                              uint64_t txId) const {
    std::ifstream f(segPath);
    if (!f) return 0;
    std::string line;
    bool inTx = false;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() > 9 && line.substr(0, 9) == "TX_BEGIN:") {
            uint64_t id = 0;
            try { id = std::stoull(line.substr(9)); } catch (...) {}
            inTx = (id == txId);
        }
        if (inTx && line.size() > 3 && line.substr(0, 3) == "TS:") {
            try { return std::stoll(line.substr(3)); } catch (...) {}
        }
        if (line.size() > 10 && line.substr(0, 10) == "TX_COMMIT:") {
            inTx = false;
        }
    }
    return 0;
}

// Make parseOpBlock public for restore
inline bool WalRecovery::parseOpBlock(const std::vector<std::string>& lines,
                                       BufferedOp& op) const {
    // Already defined above as private — we just need the declaration accessible
    // This is handled by moving it to public in the class
    return false;  // Placeholder — actual implementation is in the class
}

} // namespace milansql
'''

# Remove the duplicate parseOpBlock at the end — it's already defined in the class
# We need to make it public instead
pitr_hpp = pitr_hpp.replace('''// Make parseOpBlock public for restore
inline bool WalRecovery::parseOpBlock(const std::vector<std::string>& lines,
                                       BufferedOp& op) const {
    // Already defined above as private — we just need the declaration accessible
    // This is handled by moving it to public in the class
    return false;  // Placeholder — actual implementation is in the class
}''', '')

with open('/opt/milansql/src/wal/pitr_manager.hpp', 'w') as f:
    f.write(pitr_hpp)
print("CREATED: pitr_manager.hpp")

# ============================================================
# STEP 2: Make WalRecovery::parseOpBlock public
# ============================================================
with open('/opt/milansql/src/wal/wal_recovery.hpp', 'r') as f:
    content = f.read()

fixes = 0

old_private = '''private:
    // Parse one op block (lines between TX_BEGIN and ---) into a BufferedOp
    bool parseOpBlock(const std::vector<std::string>& lines,
                      BufferedOp& op) const {'''

new_private = '''    // Parse one op block (lines between TX_BEGIN and ---) into a BufferedOp
    // Phase 178: Made public for PITR restore
    bool parseOpBlock(const std::vector<std::string>& lines,
                      BufferedOp& op) const {'''

if old_private in content:
    content = content.replace(old_private, new_private, 1)
    fixes += 1
    print("FIX: parseOpBlock made public")

with open('/opt/milansql/src/wal/wal_recovery.hpp', 'w') as f:
    f.write(content)

# ============================================================
# STEP 3: Add timestamps to WAL entries in engine.hpp
# ============================================================
with open('/opt/milansql/src/engine/engine.hpp', 'r') as f:
    content = f.read()

# 3a. Add TS: line after TX_BEGIN
old_tx_begin = '''        {
            std::ofstream wal(walPath_, std::ios::app);
            if (wal) wal << "TX_BEGIN:" << mvccTxId_ << "\\n";
        }'''

new_tx_begin = '''        {
            std::ofstream wal(walPath_, std::ios::app);
            if (wal) {
                wal << "TX_BEGIN:" << mvccTxId_ << "\\n";
                wal << "TS:" << std::time(nullptr) << "\\n";  // Phase 178: PITR timestamp
            }
        }'''

if old_tx_begin in content:
    content = content.replace(old_tx_begin, new_tx_begin, 1)
    fixes += 1
    print("FIX: TX_BEGIN timestamp added")
else:
    print("SKIP: TX_BEGIN pattern not found")

# 3b. Add TS: line after TX_COMMIT
old_tx_commit = '''                    std::string line = "TX_COMMIT:" + std::to_string(commitId);
                    uint32_t crc = walCrc32(line);
                    wal << line << "\\n";'''

new_tx_commit = '''                    std::string line = "TX_COMMIT:" + std::to_string(commitId);
                    uint32_t crc = walCrc32(line);
                    wal << line << "\\n";
                    wal << "TS:" << std::time(nullptr) << "\\n";  // Phase 178: commit timestamp'''

if old_tx_commit in content:
    content = content.replace(old_tx_commit, new_tx_commit, 1)
    fixes += 1
    print("FIX: TX_COMMIT timestamp added")

with open('/opt/milansql/src/engine/engine.hpp', 'w') as f:
    f.write(content)

# ============================================================
# STEP 4: Integrate WAL archiving with checkpoint
# ============================================================
with open('/opt/milansql/src/wal/checkpoint.hpp', 'r') as f:
    content = f.read()

# 4a. Add pitr_manager include
old_include_cp = '#include "../utils/date_utils.hpp"'
new_include_cp = '''#include "../utils/date_utils.hpp"
#include "pitr_manager.hpp"'''

if 'pitr_manager.hpp' not in content:
    content = content.replace(old_include_cp, new_include_cp, 1)
    fixes += 1
    print("FIX: pitr_manager included in checkpoint")

# 4b. Archive WAL before truncating during checkpoint
old_archive = '''    void archiveWal() {
        // Read current WAL
        std::ifstream src(WAL_FILE, std::ios::binary);
        if (src.is_open()) {
            std::ofstream dst(WAL_ARCHIVE, std::ios::binary | std::ios::app);
            if (dst.is_open()) {
                dst << src.rdbuf();
            }
        }
        // Truncate WAL (overwrite with empty file)
        std::ofstream trunc(WAL_FILE, std::ios::trunc);
        (void)trunc; // close immediately → empty file
    }'''

new_archive = '''    void archiveWal() {
        // Phase 178: Archive to PITR archive directory before legacy archive
        g_pitrManager().archiveCurrentWal(WAL_FILE);
        // Clean old archives based on retention
        g_pitrManager().cleanOldArchives();

        // Legacy: append to single archive file
        std::ifstream src(WAL_FILE, std::ios::binary);
        if (src.is_open()) {
            std::ofstream dst(WAL_ARCHIVE, std::ios::binary | std::ios::app);
            if (dst.is_open()) {
                dst << src.rdbuf();
            }
        }
        // Truncate WAL (overwrite with empty file)
        std::ofstream trunc(WAL_FILE, std::ios::trunc);
        (void)trunc; // close immediately \xe2\x86\x92 empty file
    }'''

if old_archive in content:
    content = content.replace(old_archive, new_archive, 1)
    fixes += 1
    print("FIX: WAL archiving integrated with checkpoint")
else:
    print("SKIP: archiveWal pattern not found")

with open('/opt/milansql/src/wal/checkpoint.hpp', 'w') as f:
    f.write(content)

# ============================================================
# STEP 5: Add PITR commands to dispatch.hpp
# ============================================================
with open('/opt/milansql/src/dispatch.hpp', 'r') as f:
    content = f.read()

# 5a. Add pitr_manager include
if 'pitr_manager.hpp' not in content:
    old_disp_inc = '#include "wal/checkpoint.hpp"'
    if old_disp_inc in content:
        content = content.replace(old_disp_inc, old_disp_inc + '\n#include "wal/pitr_manager.hpp"', 1)
        fixes += 1
        print("FIX: pitr_manager included in dispatch")

# 5b. Add PITR command handling after BACKUP_DATABASE/RESTORE_DATABASE
old_backup_dispatch = '''    case milansql::CommandType::BACKUP_DATABASE: {
        std::string msg = milansql::MilanBackup::dumpDatabase(engine, cmd.backupFile);
        std::cout << "  " << msg << "\\n\\n";
        break;
    }'''

new_backup_dispatch = '''    case milansql::CommandType::BACKUP_DATABASE: {
        // Phase 178: If backupFile ends with /, use PITR base backup
        if (!cmd.backupFile.empty() && cmd.backupFile.back() == '/') {
            auto& lsn = milansql::g_lsnManager();
            std::string msg = milansql::g_pitrManager().createBaseBackup(
                "database.milan", cmd.backupFile,
                lsn.currentLsn(), "11.0.0", engine.tableCount());
            std::cout << "  " << msg << "\\n\\n";
        } else {
            std::string msg = milansql::MilanBackup::dumpDatabase(engine, cmd.backupFile);
            std::cout << "  " << msg << "\\n\\n";
        }
        break;
    }'''

if old_backup_dispatch in content:
    content = content.replace(old_backup_dispatch, new_backup_dispatch, 1)
    fixes += 1
    print("FIX: BACKUP dispatch enhanced for PITR")

with open('/opt/milansql/src/dispatch.hpp', 'w') as f:
    f.write(content)

# ============================================================
# STEP 6: Add PITR API endpoints to http_server.hpp
# ============================================================
with open('/opt/milansql/src/server/http_server.hpp', 'r') as f:
    content = f.read()

# 6a. Add /api/pitr endpoint before /api/ssl
old_ssl_api = '    // Phase 177: SSL status API\n    if (req.path == "/api/ssl") {'
new_pitr_api = '''    // Phase 178: PITR API endpoints
    if (req.path == "/api/pitr/status") {
        auto segments = milansql::g_pitrManager().listArchiveSegments();
        auto backups = milansql::g_pitrManager().listBackups();
        uint64_t totalArchiveSize = 0;
        for (const auto& s : segments) totalArchiveSize += s.sizeBytes;

        std::string json = "{";
        json += "\\"archive_enabled\\":" + std::string(milansql::g_pitrManager().config().archiveEnabled ? "true" : "false");
        json += ",\\"archive_dir\\":\\"" + milansql::g_pitrManager().config().archiveDir + "\\"";
        json += ",\\"retention_days\\":" + std::to_string(milansql::g_pitrManager().config().retentionDays);
        json += ",\\"archive_segments\\":" + std::to_string(segments.size());
        json += ",\\"archive_size\\":" + std::to_string(totalArchiveSize);
        if (!segments.empty()) {
            json += ",\\"oldest_segment\\":\\"" + milansql::pitr_epoch_to_str(segments.front().timestamp) + "\\"";
            json += ",\\"newest_segment\\":\\"" + milansql::pitr_epoch_to_str(segments.back().timestamp) + "\\"";
        }
        json += ",\\"backups\\":[";
        for (size_t i = 0; i < backups.size(); ++i) {
            if (i > 0) json += ",";
            json += "{\\"dir\\":\\"" + backups[i].backupDir + "\\"";
            json += ",\\"timestamp\\":\\"" + backups[i].timestamp + "\\"";
            json += ",\\"lsn\\":" + std::to_string(backups[i].startLsn);
            json += ",\\"size\\":" + std::to_string(backups[i].sizeBytes);
            json += ",\\"tables\\":" + std::to_string(backups[i].tableCount) + "}";
        }
        json += "]}";
        return buildHttpResponse(200, json);
    }

    if (req.path == "/api/pitr/backup" && req.method == "POST") {
        // Create a new base backup
        int64_t now = milansql::pitr_now_epoch();
        std::string backupDir = milansql::g_pitrManager().config().backupBaseDir +
                                "/backup_" + std::to_string(now);
        auto& lsn = milansql::g_lsnManager();
        std::string msg = milansql::g_pitrManager().createBaseBackup(
            "database.milan", backupDir,
            lsn.currentLsn(), MILANSQL_VERSION,
            static_cast<int>(engine_.tableCount()));
        bool ok = msg.substr(0, 2) == "OK";
        return buildHttpResponse(ok ? 200 : 500,
            "{\\"success\\":" + std::string(ok ? "true" : "false") +
            ",\\"message\\":\\"" + msg + "\\"}");
    }

    if (req.path == "/api/pitr/archive-now" && req.method == "POST") {
        std::string msg = milansql::g_pitrManager().archiveCurrentWal("database.milan.wal");
        bool ok = msg.substr(0, 2) == "OK";
        return buildHttpResponse(ok ? 200 : 500,
            "{\\"success\\":" + std::string(ok ? "true" : "false") +
            ",\\"message\\":\\"" + msg + "\\"}");
    }

    // Phase 177: SSL status API
    if (req.path == "/api/ssl") {'''

if '/api/pitr' not in content:
    content = content.replace(old_ssl_api, new_pitr_api, 1)
    fixes += 1
    print("FIX: PITR API endpoints added")

# 6b. Add Backup tab JavaScript
old_ssl_loader = '''// Phase 177: SSL status loader
async function loadSslStatus() {'''

new_ssl_loader = '''// Phase 178: PITR/Backup loader
async function loadPitrStatus() {
  try {
    var r = await fetch('/api/pitr/status', {credentials:'include'});
    var d = await r.json();
    var el = document.getElementById('pitr-status-panel');
    if (!el) return;
    var h = '<div class="ssl-card"><h3>&#x1F4BE; Backup & Recovery';
    h += ' <span class="ssl-badge ' + (d.archive_enabled ? 'on' : 'off') + '">';
    h += d.archive_enabled ? 'ARCHIVING' : 'DISABLED';
    h += '</span></h3>';
    h += '<div class="ssl-grid">';
    h += '<div class="ssl-item"><strong>Archive Dir</strong>' + escHtml(d.archive_dir||'') + '</div>';
    h += '<div class="ssl-item"><strong>Segments</strong>' + (d.archive_segments||0) + '</div>';
    h += '<div class="ssl-item"><strong>Archive Size</strong>' + formatSize(d.archive_size||0) + '</div>';
    h += '<div class="ssl-item"><strong>Retention</strong>' + (d.retention_days||0) + ' days</div>';
    if (d.oldest_segment) h += '<div class="ssl-item"><strong>Oldest</strong>' + escHtml(d.oldest_segment) + '</div>';
    if (d.newest_segment) h += '<div class="ssl-item"><strong>Newest</strong>' + escHtml(d.newest_segment) + '</div>';
    h += '</div>';
    if (d.backups && d.backups.length > 0) {
      h += '<div style="margin-top:12px;font-size:0.75rem;color:var(--text-2);text-transform:uppercase;letter-spacing:.06em">Backups</div>';
      h += '<table style="width:100%;font-size:0.72rem;margin-top:4px"><thead><tr><th style="text-align:left;padding:4px 8px;color:var(--text-2);border-bottom:1px solid var(--border)">Timestamp</th><th>LSN</th><th>Size</th><th>Tables</th></tr></thead><tbody>';
      d.backups.forEach(function(b) {
        h += '<tr><td style="padding:4px 8px;color:var(--text-1)">' + escHtml(b.timestamp) + '</td>';
        h += '<td style="padding:4px 8px;text-align:center">' + b.lsn + '</td>';
        h += '<td style="padding:4px 8px;text-align:center">' + formatSize(b.size) + '</td>';
        h += '<td style="padding:4px 8px;text-align:center">' + b.tables + '</td></tr>';
      });
      h += '</tbody></table>';
    }
    h += '<div style="margin-top:8px;display:flex;gap:8px;justify-content:flex-end">';
    h += '<button onclick="doArchiveNow()" style="background:var(--bg-hover);color:var(--text-1);border:1px solid var(--border);border-radius:6px;padding:4px 12px;font-size:0.7rem;cursor:pointer">Archive WAL</button>';
    h += '<button onclick="doBackupNow()" style="background:var(--accent);color:white;border:none;border-radius:6px;padding:4px 12px;font-size:0.7rem;cursor:pointer">Backup Now</button>';
    h += '</div></div>';
    el.innerHTML = h;
  } catch(e) {}
}
function formatSize(b) {
  if (b < 1024) return b + ' B';
  if (b < 1048576) return Math.round(b/1024) + ' KB';
  if (b < 1073741824) return Math.round(b/1048576) + ' MB';
  return (b/1073741824).toFixed(1) + ' GB';
}
async function doBackupNow() {
  try {
    var r = await fetch('/api/pitr/backup', {method:'POST', credentials:'include'});
    var d = await r.json();
    if (d.success) { loadPitrStatus(); } else { alert('Backup failed: ' + (d.message||'')); }
  } catch(e) { alert('Backup failed'); }
}
async function doArchiveNow() {
  try {
    var r = await fetch('/api/pitr/archive-now', {method:'POST', credentials:'include'});
    var d = await r.json();
    if (d.success) { loadPitrStatus(); } else { alert('Archive failed: ' + (d.message||'')); }
  } catch(e) { alert('Archive failed'); }
}

// Phase 177: SSL status loader
async function loadSslStatus() {'''

if 'loadPitrStatus' not in content:
    content = content.replace(old_ssl_loader, new_ssl_loader, 1)
    fixes += 1
    print("FIX: PITR JavaScript added")

# 6c. Add loadPitrStatus() call
old_load_ssl_call = 'loadSslStatus();\npollStatus();'
new_load_ssl_call = 'loadSslStatus();\nloadPitrStatus();\npollStatus();'

if 'loadPitrStatus()' not in content:
    content = content.replace(old_load_ssl_call, new_load_ssl_call, 1)
    fixes += 1
    print("FIX: loadPitrStatus() call added")

# 6d. Add pitr-status-panel div
old_ssl_panel = '<div id="ssl-status-panel"></div>'
new_ssl_panel = '<div id="ssl-status-panel"></div>\n<div id="pitr-status-panel"></div>'

if 'pitr-status-panel' not in content:
    content = content.replace(old_ssl_panel, new_ssl_panel, 1)
    fixes += 1
    print("FIX: pitr-status-panel div added")

# 6e. Update version
content = content.replace(
    'static constexpr const char* MILANSQL_VERSION = "10.9.0"',
    'static constexpr const char* MILANSQL_VERSION = "11.0.0"', 1)
content = content.replace(
    "MilanSQL Admin <span class=\"ms-version\">v10.9.0</span>",
    "MilanSQL Admin <span class=\"ms-version\">v11.0.0</span>", 1)

with open('/opt/milansql/src/server/http_server.hpp', 'w') as f:
    f.write(content)

# ============================================================
# STEP 7: Add pitr_manager include to main.cpp
# ============================================================
with open('/opt/milansql/src/main.cpp', 'r') as f:
    content = f.read()

if 'pitr_manager' not in content:
    old_main_inc = '#include "wal/checkpoint.hpp"'
    if old_main_inc in content:
        content = content.replace(old_main_inc, old_main_inc + '\n#include "wal/pitr_manager.hpp"', 1)
        fixes += 1
        print("FIX: pitr_manager included in main.cpp")

with open('/opt/milansql/src/main.cpp', 'w') as f:
    f.write(content)

print(f"\nTotal fixes: {fixes}")
'''

# Actually, let me write the full script content properly
with open('/opt/milansql/impl_pitr.py', 'w') as f:
    f.write(script_content)
'''
