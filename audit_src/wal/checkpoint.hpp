#pragma once

#include <string>
#include <fstream>
#include <sstream>
#include <iostream>
#include <cstdint>
#include <ctime>
#include <cstdio>
#include "../utils/date_utils.hpp"

// ============================================================
// checkpoint.hpp — Phase 85: WAL Checkpointing
//
// Responsibilities:
//   - Track how many transactions have occurred since last checkpoint
//   - Write a checkpoint record to database.checkpoint
//   - Trigger WAL rotation (archive old WAL, start fresh)
//   - Load checkpoint state on startup
//   - Auto-checkpoint when txSinceLastCheckpoint_ >= interval
//
// Checkpoint file format (database.checkpoint):
//   lastTxId:<uint64>
//   timestamp:<YYYY-MM-DD HH:MM:SS>
//   walPosition:<uint64>
// ============================================================

namespace milansql {

class CheckpointManager {
public:
    static constexpr uint64_t DEFAULT_INTERVAL = 1000;
    static constexpr const char* CHECKPOINT_FILE = "database.checkpoint";
    static constexpr const char* WAL_FILE        = "database.milan.wal";
    static constexpr const char* WAL_ARCHIVE     = "database.wal.archive";

    CheckpointManager() {
        loadCheckpoint();
    }

    // ── Called after each committed transaction ───────────────
    void onCommit() {
        ++txSinceLastCheckpoint_;
        ++totalTx_;
    }

    // ── Should we trigger an auto-checkpoint? ─────────────────
    bool shouldAutoCheckpoint() const {
        return autoCheckpointEnabled_ &&
               txSinceLastCheckpoint_ >= autoCheckpointInterval_;
    }

    // ── Perform checkpoint: archive WAL, reset counter ────────
    // Returns number of WAL bytes archived.
    uint64_t doCheckpoint(uint64_t currentTxId) {
        // Measure WAL size before archiving
        uint64_t walBytes = getFileSize(WAL_FILE);

        // Archive the WAL (append to archive, then truncate WAL)
        archiveWal();

        // Update state
        lastCheckpointTxId_        = currentTxId;
        walPositionAtCheckpoint_   = 0;  // fresh WAL starts at 0
        txSinceLastCheckpoint_     = 0;
        lastCheckpointTime_        = currentTime();
        ++checkpointCount_;

        saveCheckpoint();

        std::cout << "  CHECKPOINT abgeschlossen: "
                  << walBytes << " Bytes WAL archiviert, "
                  << "Tx-ID=" << currentTxId << "\n\n";
        return walBytes;
    }

    // ── Checkpoint status display ─────────────────────────────
    void showStatus() const {
        uint64_t walBytes = getFileSize(WAL_FILE);

        std::cout << "\n  WAL Checkpoint Status\n";
        std::cout << "  ─────────────────────────────────────────\n";
        std::cout << "  Letzter Checkpoint Tx-ID : " << lastCheckpointTxId_   << "\n";
        std::cout << "  Letzter Checkpoint Zeit  : " << (lastCheckpointTime_.empty() ? "(nie)" : lastCheckpointTime_) << "\n";
        std::cout << "  Tx seit letztem Checkpoint: " << txSinceLastCheckpoint_ << "\n";
        std::cout << "  Checkpoints gesamt       : " << checkpointCount_      << "\n";
        std::cout << "  Aktuelle WAL-Groesse      : " << walBytes << " Bytes\n";
        std::cout << "  Auto-Checkpoint          : " << (autoCheckpointEnabled_ ? "ON" : "OFF") << "\n";
        std::cout << "  Auto-Checkpoint Intervall: " << autoCheckpointInterval_ << " Tx\n";
        std::cout << "\n";
    }

    // ── Settings ──────────────────────────────────────────────
    void setAutoCheckpointInterval(uint64_t n) {
        autoCheckpointInterval_ = n;
        autoCheckpointEnabled_  = true;
    }

    void setAutoCheckpointEnabled(bool on) {
        autoCheckpointEnabled_ = on;
    }

    uint64_t lastCheckpointTxId() const { return lastCheckpointTxId_; }
    uint64_t txSinceLastCheckpoint() const { return txSinceLastCheckpoint_; }

    // ── Persist state to disk ─────────────────────────────────
    void saveCheckpoint() const {
        std::ofstream f(CHECKPOINT_FILE);
        if (!f.is_open()) return;
        f << "lastTxId:"     << lastCheckpointTxId_      << "\n";
        f << "timestamp:"    << lastCheckpointTime_       << "\n";
        f << "walPosition:"  << walPositionAtCheckpoint_  << "\n";
        f << "checkpoints:"  << checkpointCount_          << "\n";
        f << "totalTx:"      << totalTx_                  << "\n";
    }

    // ── Load state from disk (called at startup) ──────────────
    void loadCheckpoint() {
        std::ifstream f(CHECKPOINT_FILE);
        if (!f.is_open()) return;
        std::string line;
        while (std::getline(f, line)) {
            auto sep = line.find(':');
            if (sep == std::string::npos) continue;
            std::string key = line.substr(0, sep);
            std::string val = line.substr(sep + 1);
            if (key == "lastTxId")    lastCheckpointTxId_     = std::stoull(val);
            if (key == "timestamp")   lastCheckpointTime_     = val;
            if (key == "walPosition") walPositionAtCheckpoint_ = std::stoull(val);
            if (key == "checkpoints") checkpointCount_         = std::stoull(val);
            if (key == "totalTx")     totalTx_                 = std::stoull(val);
        }
    }

private:
    uint64_t lastCheckpointTxId_      = 0;
    uint64_t walPositionAtCheckpoint_  = 0;
    uint64_t autoCheckpointInterval_   = DEFAULT_INTERVAL;
    uint64_t txSinceLastCheckpoint_    = 0;
    uint64_t checkpointCount_          = 0;
    uint64_t totalTx_                  = 0;
    bool     autoCheckpointEnabled_    = true;
    std::string lastCheckpointTime_;

    // ── Archive WAL: append current WAL to archive, then clear WAL ──
    void archiveWal() {
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
    }

    // ── File size helper ──────────────────────────────────────
    static uint64_t getFileSize(const char* path) {
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (!f.is_open()) return 0;
        auto sz = f.tellg();
        return sz > 0 ? static_cast<uint64_t>(sz) : 0;
    }

    static std::string currentTime() {
        std::time_t t = std::time(nullptr);
        char buf[20] = {};
        std::tm ltm = milansql::safe_localtime(&t);
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &ltm);
        return buf;
    }
};

} // namespace milansql
