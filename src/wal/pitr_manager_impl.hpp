#pragma once
// pitr_manager_impl.hpp - Engine-dependent PITR implementation
// Must be included AFTER engine.hpp, wal_recovery.hpp, storage.hpp

#include "pitr_manager.hpp"
#include "../engine/engine.hpp"
#include "wal_recovery.hpp"
#include "../storage/storage.hpp"

namespace milansql {

inline PitrManager::RestoreResult PitrManager::restoreToPoint(
        Engine& engine,
        const std::string& backupDir,
        int64_t targetEpoch,
        uint64_t targetTxId) {
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
        // Load backup state directly into provided engine
        storage.loadWithCount(engine);

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
                            engine.applyBufferedOp(op);
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

} // namespace milansql
