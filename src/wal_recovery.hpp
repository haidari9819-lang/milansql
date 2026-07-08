#pragma once

#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <iostream>
#include <cstdio>
#include <cstdint>
#include <ctime>
#include <map>

// ============================================================
// wal_recovery.hpp — Phase 72: WAL Crash Recovery
// Scans database.milan.wal and replays committed transactions
// on startup after a crash.
//
// WAL Format (Phase 72 extended):
//   TX_BEGIN:<txId>
//   OP <opType> <tableName>
//   VAL <value>
//   SET <col> <val>
//   WHERE <col> <val>
//   ALTER <op> <colName> <colType> <colNew>
//   ---
//   TX_COMMIT:<txId>    ← written before persist
//   TX_ROLLBACK:<txId>  ← written on ROLLBACK
// ============================================================

namespace milansql {

// One transaction entry from the WAL
struct WalTxEntry {
    uint64_t txId = 0;
    bool committed = false;
    bool rolledBack = false;
    // Each element is one op block (list of lines before "---")
    std::vector<std::vector<std::string>> opBlocks;
};

class WalRecovery {
public:
    struct RecoveryResult {
        bool hadWal          = false;
        int  recoveredTxCount = 0;  // transactions with TX_COMMIT → replayed
        int  discardedTxCount = 0;  // transactions without TX_COMMIT → dropped
        int  replayedOpCount  = 0;  // total ops applied
    };

    // Bug #21: CRC32 for WAL integrity verification
    static uint32_t walCrc32(const std::string& s) {
        uint32_t crc = 0xFFFFFFFFu;
        for (unsigned char c : s) {
            for (int k = 0; k < 8; ++k) {
                bool xorBit = ((crc ^ c) & 1) != 0;
                crc >>= 1;
                if (xorBit) crc ^= 0xEDB88320u;
                c >>= 1;
            }
        }
        return crc ^ 0xFFFFFFFFu;
    }

    // ── Scan WAL file into WalTxEntry list ─────────────────────
    std::vector<WalTxEntry> scanWal(const std::string& path) {
        std::vector<WalTxEntry> txList;
        std::ifstream f(path);
        if (!f) return txList;

        WalTxEntry* current = nullptr;
        std::vector<std::string> block;
        // Bug #21: track pending commit lines for CRC verification
        std::map<uint64_t, std::string> pendingCommitLines;

        std::string line;
        while (std::getline(f, line)) {
            // Strip trailing \r (Windows line endings)
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;

            if (line.size() > 9 && line.substr(0, 9) == "TX_BEGIN:") {
                uint64_t id = 0;
                try { id = std::stoull(line.substr(9)); } catch (...) {}
                txList.push_back(WalTxEntry{});
                txList.back().txId = id;
                current = &txList.back();
                block.clear();
            } else if (line.size() > 10 && line.substr(0, 10) == "TX_COMMIT:") {
                uint64_t id = 0;
                try { id = std::stoull(line.substr(10)); } catch (...) {}
                // Store the commit line for CRC verification
                pendingCommitLines[id] = line;
                // Mark as committed (may be reverted if CRC fails)
                for (auto& tx : txList)
                    if (tx.txId == id) { tx.committed = true; break; }
                current = nullptr;
                block.clear();
            } else if (line.size() > 4 && line.substr(0, 4) == "CRC:") {
                // Bug #21: Verify CRC of the preceding TX_COMMIT line
                uint32_t expectedCrc = 0;
                try { expectedCrc = static_cast<uint32_t>(std::stoul(line.substr(4))); } catch (...) {}
                // Find the last pending commit and verify
                if (!pendingCommitLines.empty()) {
                    auto last = pendingCommitLines.rbegin();
                    uint32_t actualCrc = walCrc32(last->second);
                    if (actualCrc != expectedCrc) {
                        // CRC mismatch: revoke the commit
                        uint64_t badId = last->first;
                        for (auto& tx : txList)
                            if (tx.txId == badId) { tx.committed = false; break; }
                    }
                    pendingCommitLines.erase(last->first);
                }
            } else if (line.size() > 12 && line.substr(0, 12) == "TX_ROLLBACK:") {
                uint64_t id = 0;
                try { id = std::stoull(line.substr(12)); } catch (...) {}
                for (auto& tx : txList)
                    if (tx.txId == id) { tx.rolledBack = true; break; }
                current = nullptr;
                block.clear();
            } else if (current != nullptr) {
                if (line == "---") {
                    if (!block.empty()) {
                        current->opBlocks.push_back(block);
                        block.clear();
                    }
                } else {
                    block.push_back(line);
                }
            }
        }
        return txList;
    }

    // ── Delete WAL file ─────────────────────────────────────────
    void clearWal(const std::string& path) {
        std::remove(path.c_str());
    }

    // ── All-in-one recovery entry point ─────────────────────────
    // Call AFTER loading the binary database (database.milan).
    // Replays any committed transactions that weren't yet persisted.
    RecoveryResult recover(Engine& engine, const std::string& path);

private:
    // Parse one op block (lines between TX_BEGIN and ---) into a BufferedOp
    bool parseOpBlock(const std::vector<std::string>& lines,
                      BufferedOp& op) const {
        op = BufferedOp{};
        bool hasOp = false;
        for (const auto& line : lines) {
            if (line.size() > 3 && line.substr(0, 3) == "OP ") {
                // OP <typeInt> <tableName>
                std::string rest = line.substr(3);
                size_t sp = rest.find(' ');
                if (sp == std::string::npos) continue;
                int opTypeInt = 0;
                try { opTypeInt = std::stoi(rest.substr(0, sp)); } catch (...) { continue; }
                op.opType    = static_cast<BufferedOp::Type>(opTypeInt);
                op.tableName = rest.substr(sp + 1);
                hasOp = true;
            } else if (line.size() > 4 && line.substr(0, 4) == "VAL ") {
                op.values.push_back(line.substr(4));
            } else if (line.size() > 4 && line.substr(0, 4) == "SET ") {
                // SET <col> <rest_is_value>
                std::string rest = line.substr(4);
                size_t sp = rest.find(' ');
                if (sp == std::string::npos) {
                    op.setCol = rest;
                } else {
                    op.setCol = rest.substr(0, sp);
                    op.setVal = rest.substr(sp + 1);
                }
            } else if (line.size() > 6 && line.substr(0, 6) == "WHERE ") {
                // WHERE <col> <rest_is_value>
                std::string rest = line.substr(6);
                size_t sp = rest.find(' ');
                if (sp == std::string::npos) {
                    op.whereCol = rest;
                } else {
                    op.whereCol = rest.substr(0, sp);
                    op.whereVal = rest.substr(sp + 1);
                }
            } else if (line.size() > 6 && line.substr(0, 6) == "ALTER ") {
                std::istringstream ss(line.substr(6));
                ss >> op.alterOp >> op.alterColName >> op.alterColType >> op.alterColNew;
            }
        }
        return hasOp;
    }
};

} // namespace milansql

// ─── Engine-dependent inline implementations ─────────────────
// Include engine.hpp here so we can use Engine and BufferedOp
// (Engine does NOT include wal_recovery.hpp — no circular dep)
#include "../engine/engine.hpp"

namespace milansql {

inline WalRecovery::RecoveryResult WalRecovery::recover(
        Engine& engine, const std::string& path) {
    RecoveryResult result;

    // Check if WAL file exists
    {
        std::ifstream check(path);
        if (!check) return result;  // no WAL → nothing to recover
    }
    result.hadWal = true;

    auto txList = scanWal(path);

    // Count committed vs uncommitted transactions
    for (const auto& tx : txList) {
        if (tx.committed)
            ++result.recoveredTxCount;
        else if (!tx.rolledBack && !tx.opBlocks.empty())
            ++result.discardedTxCount;
    }

    // Replay committed transactions
    for (const auto& tx : txList) {
        if (!tx.committed) continue;
        for (const auto& opBlock : tx.opBlocks) {
            BufferedOp op;
            if (parseOpBlock(opBlock, op)) {
                try {
                    engine.applyBufferedOp(op);
                    ++result.replayedOpCount;
                } catch (...) {
                    // Skip ops that fail (e.g., table doesn't exist)
                }
            }
        }
    }

    clearWal(path);
    return result;
}

} // namespace milansql
