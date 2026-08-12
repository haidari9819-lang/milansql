#pragma once
// ============================================================
// serverless_manager.hpp — Serverless Mode (Phase 3.4)
// Idle management via SIGSTOP/SIGCONT (Linux process suspension).
// On Windows, SIGSTOP/SIGCONT are unavailable; we simulate them.
// ============================================================

#include <atomic>
#include <thread>
#include <chrono>
#include <cstdint>
#include <string>

#ifndef _WIN32
#include <signal.h>
#include <unistd.h>
#endif

namespace milansql {

class ServerlessManager {
public:
    static ServerlessManager& global() {
        static ServerlessManager inst;
        return inst;
    }

    ServerlessManager() {
        lastActivityMs_.store(nowMs());
    }

    ~ServerlessManager() {
        stopWatchdog();
    }

    void setIdleTimeout(int seconds) {
        idleTimeoutSec_.store(seconds);
    }

    int idleTimeout() const { return idleTimeoutSec_.load(); }

    void setEnabled(bool e) {
        enabled_.store(e);
        if (!e && suspended_.load()) {
            resume(); // auto-resume when disabled
        }
    }

    bool isEnabled() const { return enabled_.load(); }

    void recordActivity() {
        lastActivityMs_.store(nowMs());
        if (suspended_.load()) {
            resume();
        }
    }

    bool isSuspended() const { return suspended_.load(); }

    int64_t coldStartLatencyMs() const { return coldStartMs_.load(); }

    void resume() {
        if (!suspended_.load()) return;
        int64_t resumeStart = nowMs();
#ifndef _WIN32
        if (dbPid_ > 0) {
            kill(dbPid_, SIGCONT);
        }
#endif
        suspended_.store(false);
        coldStartMs_.store(nowMs() - resumeStart);
        lastActivityMs_.store(nowMs());
    }

    void startWatchdog(pid_t pid) {
        dbPid_ = pid;
        stopWatchdog_ = false;
        watchdog_ = std::thread([this]() {
            while (!stopWatchdog_) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                if (!enabled_.load()) continue;
                if (suspended_.load()) continue;
                int64_t idle = nowMs() - lastActivityMs_.load();
                int64_t timeout = (int64_t)idleTimeoutSec_.load() * 1000;
                if (timeout > 0 && idle >= timeout) {
#ifndef _WIN32
                    if (dbPid_ > 0) {
                        suspended_.store(true);
                        kill(dbPid_, SIGSTOP);
                    }
#else
                    // On Windows: just mark as suspended (no real SIGSTOP)
                    suspended_.store(true);
#endif
                }
            }
        });
    }

    void stopWatchdog() {
        stopWatchdog_ = true;
        if (watchdog_.joinable()) watchdog_.join();
    }

private:
    std::atomic<int>     idleTimeoutSec_{300};
    std::atomic<bool>    enabled_{false};
    std::atomic<int64_t> lastActivityMs_{0};
    std::atomic<bool>    suspended_{false};
    std::atomic<int64_t> coldStartMs_{0};
    std::thread          watchdog_;
    std::atomic<bool>    stopWatchdog_{false};
#ifndef _WIN32
    pid_t dbPid_{0};
#else
    int   dbPid_{0};
#endif

    static int64_t nowMs() {
        using namespace std::chrono;
        return duration_cast<milliseconds>(
            steady_clock::now().time_since_epoch()).count();
    }
};

} // namespace milansql
