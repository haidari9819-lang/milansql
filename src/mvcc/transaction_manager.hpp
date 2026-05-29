#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <ctime>
#include <iostream>
#include <algorithm>

// ============================================================
// transaction_manager.hpp — Phase 71: MVCC Transaction Manager
// ============================================================

namespace milansql {

struct TxInfo {
    uint64_t    id               = 0;
    std::string isolationLevel   = "REPEATABLE READ";
    std::string startTime;       // "YYYY-MM-DD HH:MM:SS"
};

class TransactionManager {
public:
    TransactionManager() = default;

    // ── Start a new transaction, return its ID ─────────────────
    uint64_t beginTx(const std::string& isolationLevel = "REPEATABLE READ") {
        uint64_t id = ++globalTxId_;
        TxInfo info;
        info.id             = id;
        info.isolationLevel = isolationLevel;
        info.startTime      = currentTime();
        activeTxs_[id]      = std::move(info);
        return id;
    }

    // ── Commit a transaction ────────────────────────────────────
    void commitTx(uint64_t id) {
        activeTxs_.erase(id);
        committedTxs_.insert(id);
    }

    // ── Rollback a transaction ──────────────────────────────────
    void rollbackTx(uint64_t id) {
        activeTxs_.erase(id);
        // NOT added to committedTxs_ — rows with xmin==id stay invisible
    }

    // ── Check if a txId is committed ───────────────────────────
    bool isCommitted(uint64_t id) const {
        return id == 0 || committedTxs_.count(id) > 0;
    }

    // ── Current global counter (for snapshots) ─────────────────
    uint64_t currentGlobalId() const { return globalTxId_; }

    // ── Minimum active tx ID (VACUUM boundary) ─────────────────
    uint64_t minActiveTxId() const {
        if (activeTxs_.empty()) return globalTxId_ + 1;
        return activeTxs_.begin()->first;
    }

    // ── Active transactions (for SHOW TRANSACTIONS) ─────────────
    const std::map<uint64_t, TxInfo>& activeTxs() const { return activeTxs_; }

    // ── Print SHOW TRANSACTIONS table ──────────────────────────
    void showTransactions() const {
        if (activeTxs_.empty()) {
            std::cout << "  (Keine aktiven Transaktionen)\n\n";
            return;
        }

        // column widths
        size_t wId    = 2;
        size_t wIso   = 16;
        size_t wStart = 19;
        for (const auto& [id, info] : activeTxs_) {
            wId    = std::max(wId,    std::to_string(id).size());
            wIso   = std::max(wIso,   info.isolationLevel.size());
            wStart = std::max(wStart, info.startTime.size());
        }

        auto hline = [&](const char* l, const char* m, const char* r, const char* f) {
            std::cout << "  " << l;
            for (size_t j = 0; j < wId + 2; ++j)    std::cout << f;
            std::cout << m;
            for (size_t j = 0; j < 8;        ++j)    std::cout << f;  // Status
            std::cout << m;
            for (size_t j = 0; j < wStart + 2; ++j)  std::cout << f;
            std::cout << m;
            for (size_t j = 0; j < wIso + 2;   ++j)  std::cout << f;
            std::cout << r << "\n";
        };
        auto cell = [](const std::string& s, size_t w) {
            std::cout << " " << s;
            for (size_t j = s.size(); j < w; ++j) std::cout << " ";
            std::cout << " \u2502";
        };

        std::cout << "\n";
        hline("\u250c", "\u252c", "\u2510", "\u2500");
        std::cout << "  \u2502";
        cell("ID",              wId);
        cell("Status",          6);
        cell("Started",         wStart);
        cell("Isolation Level", wIso);
        std::cout << "\n";
        hline("\u251c", "\u253c", "\u2524", "\u2500");

        for (const auto& [id, info] : activeTxs_) {
            std::cout << "  \u2502";
            cell(std::to_string(id), wId);
            cell("ACTIVE",           6);
            cell(info.startTime,     wStart);
            cell(info.isolationLevel, wIso);
            std::cout << "\n";
        }

        hline("\u2514", "\u2534", "\u2518", "\u2500");
        std::cout << "  " << activeTxs_.size() << " aktive Transaktion(en)\n\n";
    }

private:
    uint64_t                      globalTxId_  = 0;
    std::map<uint64_t, TxInfo>    activeTxs_;     // id → info (active/uncommitted)
    std::set<uint64_t>            committedTxs_;  // committed tx IDs

    static std::string currentTime() {
        std::time_t t = std::time(nullptr);
        char buf[20] = {};
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
        return buf;
    }
};

} // namespace milansql
