#pragma once
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
                                  uint64_t targetTxId);

    // ── Recovery Status ────────────────────────────────────────
    bool isRecovering() const { return recovering_.load(); }
    std::string recoveryProgress() const {
        std::lock_guard<std::mutex> lk(mu_);
        return recoveryProgress_;
    }

private:
    Config config_;
    mutable std::mutex mu_;
    mutable std::atomic<bool> recovering_{false};
    mutable std::string recoveryProgress_;
    uint64_t archivedSegments_{0};
    uint64_t totalArchivedBytes_{0};
    std::string lastArchiveTime_;

    int64_t parseTxTimestamp(const std::string& segPath, uint64_t txId) const;

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
