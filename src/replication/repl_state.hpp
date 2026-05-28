#pragma once
// ============================================================
// repl_state.hpp — Global replication state for MilanSQL
// Phase 59: Master/Slave Replication
// ============================================================

#include <atomic>
#include <string>
#include <mutex>
#include <functional>
#include <vector>

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

} // namespace milansql
