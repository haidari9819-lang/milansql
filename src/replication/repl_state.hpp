#pragma once
// ============================================================
// repl_state.hpp — Global replication state for MilanSQL
// Phase 59: Master/Slave Replication
// ============================================================

#include <atomic>
#include <string>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <vector>
#include <chrono>
#include <sstream>

namespace milansql {

// ── Binlog entry struct (for SHOW BINLOG display) ────────────
struct ReplBinlogEntry {
    long long   pos;
    std::string timestamp;
    std::string sql;
};

// ── Global replication state ──────────────────────────────────
struct ReplState {
    // ── Master ──────────────────────────────────────────────
    std::atomic<bool>      isMaster{false};
    std::atomic<int>       connectedSlaves{0};
    std::string            binlogFile{"database.binlog"};

    // ── Slave ────────────────────────────────────────────────
    std::atomic<bool>      isSlave{false};
    std::atomic<bool>      slaveRunning{false};
    std::atomic<bool>      slavePaused{false};
    std::atomic<long long> slavePos{0};
    std::string            masterHost{"localhost"};
    int                    masterPort{4407};
    mutable std::mutex     statusMu;
    std::string            slaveStatus{"Stopped"};
    std::atomic<long long> slaveLagMs{0};

    // ── Phase 172: Streaming Replication ────────────────────
    int                    replicationPort{5433};   // master listen port
    std::atomic<bool>      syncMode{false};         // sync vs async replication

    // Failover detection (slave side): master unreachable > 30s
    static constexpr long long FAILOVER_TIMEOUT_MS = 30000;
    std::atomic<long long> lastSyncUnixMs{0};       // last successful sync (unix ms)
    std::atomic<bool>      masterDown{false};

    // Slave ack tracking (master side, for sync replication)
    std::mutex              ackMu;
    std::condition_variable ackCv;
    std::atomic<long long>  maxSlaveAckPos{0};
    std::atomic<long long>  syncWaitTimeouts{0};    // sync commits that timed out

    // New-binlog-data notification (master side, for streaming long-poll)
    std::mutex              dataMu;
    std::condition_variable dataCv;
};

inline ReplState g_replState;

// ── Hooks wired by main.cpp ────────────────────────────────────

// Called by dispatch.hpp after each write op on master
inline std::function<void(const std::string&)>            g_binlogHook      = nullptr;

// Called by STOP SLAVE / START SLAVE commands
inline std::function<void()>                              g_stopSlaveHook   = nullptr;
inline std::function<void()>                              g_startSlaveHook  = nullptr;

// Called by SHOW BINLOG — returns last N entries
inline std::function<std::vector<ReplBinlogEntry>(int)>   g_binlogReadLastFn = nullptr;

// Called by SHOW MASTER STATUS — returns current binlog position
inline std::function<long long()>                         g_binlogGetPosFn  = nullptr;

// ── Thread-local replay bypass flag ──────────────────────────
// Set to true by the slave exec-function so dispatchCommand
// skips the read-only check while replaying binlog entries.
inline thread_local bool tl_binlogReplay = false;

// ── Phase 172: helpers ────────────────────────────────────────

inline long long replNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// Master: record an ack position reported by a slave
inline void replRecordAck(long long pos) {
    long long prev = g_replState.maxSlaveAckPos.load();
    while (pos > prev && !g_replState.maxSlaveAckPos.compare_exchange_weak(prev, pos)) {}
    g_replState.ackCv.notify_all();
}

// Master (sync mode): block until a slave acked >= pos or timeout.
// Returns true if acked in time.
inline bool replWaitForAck(long long pos, int timeoutMs) {
    std::unique_lock<std::mutex> lk(g_replState.ackMu);
    bool ok = g_replState.ackCv.wait_for(lk, std::chrono::milliseconds(timeoutMs),
        [pos]() { return g_replState.maxSlaveAckPos.load() >= pos; });
    if (!ok) ++g_replState.syncWaitTimeouts;
    return ok;
}

// Master: wake all streaming long-poll waiters (call after binlog write)
inline void replNotifyNewData() {
    // Briefly take the mutex so waiters between pred-check and wait
    // cannot miss the notification.
    { std::lock_guard<std::mutex> lk(g_replState.dataMu); }
    g_replState.dataCv.notify_all();
}

// Master: block until pred() is true or timeout (streaming long-poll)
template <typename Pred>
inline bool replWaitForNewData(int timeoutMs, Pred pred) {
    std::unique_lock<std::mutex> lk(g_replState.dataMu);
    return g_replState.dataCv.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                                       std::move(pred));
}

// Slave: check + update failover state. Returns true if master is down.
inline bool replCheckFailover() {
    long long last = g_replState.lastSyncUnixMs.load();
    if (last == 0) return g_replState.masterDown.load();
    bool down = (replNowMs() - last) > ReplState::FAILOVER_TIMEOUT_MS;
    g_replState.masterDown.store(down);
    return down;
}

// JSON for HTTP endpoint /replication/status
inline std::string replicationStatusJson() {
    const auto& st = g_replState;
    std::string role = st.isMaster.load() ? "master"
                     : st.isSlave.load()  ? "replica" : "standalone";
    std::string slaveStatus;
    {
        std::lock_guard<std::mutex> lk(st.statusMu);
        slaveStatus = st.slaveStatus;
    }
    long long last = st.lastSyncUnixMs.load();
    long long msSinceSync = (last == 0) ? -1 : (replNowMs() - last);
    bool down = st.isSlave.load() ? replCheckFailover() : false;

    long long masterPos = -1;
    if (st.isMaster.load() && g_binlogGetPosFn) {
        try { masterPos = g_binlogGetPosFn(); } catch (...) {}
    }

    std::ostringstream oss;
    oss << "{"
        << "\"role\":\"" << role << "\","
        << "\"sync_mode\":\"" << (st.syncMode.load() ? "sync" : "async") << "\","
        << "\"replication_port\":" << st.replicationPort << ","
        << "\"connected_slaves\":" << st.connectedSlaves.load() << ","
        << "\"binlog_pos\":" << masterPos << ","
        << "\"max_slave_ack_pos\":" << st.maxSlaveAckPos.load() << ","
        << "\"sync_wait_timeouts\":" << st.syncWaitTimeouts.load() << ","
        << "\"replica\":{"
        <<   "\"running\":" << (st.slaveRunning.load() ? "true" : "false") << ","
        <<   "\"status\":\"" << slaveStatus << "\","
        <<   "\"master_host\":\"" << st.masterHost << "\","
        <<   "\"master_port\":" << st.masterPort << ","
        <<   "\"position\":" << st.slavePos.load() << ","
        <<   "\"lag_ms\":" << st.slaveLagMs.load() << ","
        <<   "\"ms_since_last_sync\":" << msSinceSync << ","
        <<   "\"master_down\":" << (down ? "true" : "false")
        << "}}";
    return oss.str();
}

} // namespace milansql
