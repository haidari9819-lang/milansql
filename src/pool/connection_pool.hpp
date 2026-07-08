#pragma once
// ============================================================
// connection_pool.hpp — Real Connection Pool
//
// Phase 94:  Stats-only simulation (SHOW POOL STATUS / SET POOL_MODE)
// Phase 170: Real pool implementation:
//   - Pooled logical engine sessions (PooledConnection) with reuse
//   - min/max pool size (CLI: --pool-min / --pool-max)
//   - Wait queue with timeout (default 30s) when pool is exhausted
//   - Background health checker replaces dead/expired connections
//   - Graceful shutdown: waits until all active connections drain
//   - JSON stats for HTTP endpoint /pool/stats
//
// The legacy API used by dispatch.hpp (SHOW POOL STATUS,
// SET POOL_MODE, SET POOL_MAX_CONNECTIONS, SET POOL_MAX_CLIENT_WAIT)
// is preserved.
// ============================================================

#include <string>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <memory>
#include <thread>
#include <atomic>
#include <chrono>
#include <functional>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <cstdint>

namespace milansql {

enum class PoolMode { SESSION, TRANSACTION, STATEMENT };

// One pooled logical connection (engine session slot).
struct PooledConnection {
    uint64_t id = 0;
    std::chrono::steady_clock::time_point createdAt{};
    std::chrono::steady_clock::time_point lastUsedAt{};
    uint64_t useCount = 0;
    std::atomic<bool> healthy{true};

    void markUnhealthy() { healthy.store(false); }
};

using ConnPtr = std::shared_ptr<PooledConnection>;

class ConnectionPool {
public:
    // Health check: return false → connection is dead and gets replaced.
    using HealthCheckFn = std::function<bool(const PooledConnection&)>;

    static constexpr int DEFAULT_MIN          = 10;
    static constexpr int DEFAULT_MAX          = 100;
    static constexpr int DEFAULT_WAIT_MS      = 30000;   // 30s queue timeout
    static constexpr int HEALTH_INTERVAL_SEC  = 15;
    static constexpr int MAX_LIFETIME_SEC     = 3600;    // recycle after 1h

    ConnectionPool() { prewarm(); }
    ConnectionPool(int minConns, int maxConns) {
        configure(minConns, maxConns);
    }

    ~ConnectionPool() {
        shutdown(0);
        stopHealthChecker();
    }

    // ── Configuration ─────────────────────────────────────────
    void configure(int minConns, int maxConns) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (minConns < 0)  minConns = 0;
            if (maxConns < 1)  maxConns = 1;
            if (minConns > maxConns) minConns = maxConns;
            minConns_ = minConns;
            maxConns_ = maxConns;
        }
        prewarm();
        cv_.notify_all();
    }

    void setMaxWait(int ms) {
        std::lock_guard<std::mutex> lk(mu_);
        if (ms >= 0) maxWaitMs_ = ms;
    }

    // Legacy API (SET POOL_MAX_CONNECTIONS = N)
    void setMaxConnections(int n) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (n < 1) return;
            maxConns_ = n;
            if (minConns_ > maxConns_) minConns_ = maxConns_;
        }
        cv_.notify_all();
    }

    // Legacy API (SET POOL_MODE = session|transaction|statement)
    void setMode(const std::string& mode) {
        std::lock_guard<std::mutex> lk(mu_);
        if (mode == "transaction")    mode_ = PoolMode::TRANSACTION;
        else if (mode == "statement") mode_ = PoolMode::STATEMENT;
        else                          mode_ = PoolMode::SESSION;
    }

    PoolMode getMode() const {
        std::lock_guard<std::mutex> lk(mu_);
        return mode_;
    }

    int getMinConnections() const { std::lock_guard<std::mutex> lk(mu_); return minConns_; }
    int getMaxConnections() const { std::lock_guard<std::mutex> lk(mu_); return maxConns_; }
    int getMaxWaitMs()      const { std::lock_guard<std::mutex> lk(mu_); return maxWaitMs_; }

    // ── Acquire / Release ─────────────────────────────────────
    // Returns nullptr on timeout (queue full for too long) or shutdown.
    // timeoutMs < 0 → use configured maxWaitMs_ (default 30s).
    ConnPtr acquire(int timeoutMs = -1) {
        std::unique_lock<std::mutex> lk(mu_);
        totalRequests_++;
        if (shuttingDown_) return nullptr;

        int waitMs = (timeoutMs >= 0) ? timeoutMs : maxWaitMs_;
        auto deadline = std::chrono::steady_clock::now()
                      + std::chrono::milliseconds(waitMs);
        auto t0 = std::chrono::steady_clock::now();

        for (;;) {
            // 1) Reuse an idle connection
            while (!idle_.empty()) {
                ConnPtr c = idle_.front();
                idle_.pop_front();
                if (!c->healthy.load()) { totalConns_--; deadReplaced_++; continue; }
                activate(c, t0);
                return c;
            }
            // 2) Grow the pool if below max
            if (totalConns_ < maxConns_) {
                ConnPtr c = createConnection();
                activate(c, t0);
                return c;
            }
            // 3) Pool exhausted → wait in queue (bounded by deadline)
            waiting_++;
            auto st = cv_.wait_until(lk, deadline);
            waiting_--;
            if (shuttingDown_) return nullptr;
            if (st == std::cv_status::timeout
                && std::chrono::steady_clock::now() >= deadline) {
                timeouts_++;
                return nullptr;
            }
        }
    }

    void release(const ConnPtr& c) {
        if (!c) return;
        {
            std::lock_guard<std::mutex> lk(mu_);
            active_--;
            c->lastUsedAt = std::chrono::steady_clock::now();
            if (c->healthy.load() && !shuttingDown_) {
                idle_.push_back(c);
            } else {
                totalConns_--;
                if (!c->healthy.load()) deadReplaced_++;
            }
        }
        cv_.notify_one();
        drainCv_.notify_all();
    }

    // ── Health checker ────────────────────────────────────────
    // Replaces dead / expired idle connections and tops up to min.
    void startHealthChecker(HealthCheckFn fn = nullptr,
                            int intervalSec = HEALTH_INTERVAL_SEC) {
        if (healthRunning_.exchange(true)) return;
        healthFn_ = fn ? std::move(fn) : defaultHealthCheck();
        healthThread_ = std::thread([this, intervalSec]() {
            while (healthRunning_) {
                for (int i = 0; i < intervalSec * 10 && healthRunning_; ++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                if (!healthRunning_) break;
                runHealthCheck();
            }
        });
    }

    void stopHealthChecker() {
        if (!healthRunning_.exchange(false)) return;
        if (healthThread_.joinable()) healthThread_.join();
    }

    // Run one health check pass synchronously (also used by tests).
    // Returns number of connections replaced.
    size_t runHealthCheck() {
        std::lock_guard<std::mutex> lk(mu_);
        if (shuttingDown_) return 0;
        size_t replaced = 0;
        auto now = std::chrono::steady_clock::now();
        std::deque<ConnPtr> keep;
        while (!idle_.empty()) {
            ConnPtr c = idle_.front();
            idle_.pop_front();
            bool ok = c->healthy.load();
            if (ok && healthFn_) ok = healthFn_(*c);
            if (ok) {
                auto ageSec = std::chrono::duration_cast<std::chrono::seconds>(
                                  now - c->createdAt).count();
                if (ageSec >= MAX_LIFETIME_SEC) ok = false;   // recycle old conns
            }
            if (ok) {
                keep.push_back(c);
            } else {
                totalConns_--;
                replaced++;
            }
        }
        idle_ = std::move(keep);
        deadReplaced_ += replaced;
        // Top up to minimum
        while (totalConns_ < minConns_ && totalConns_ < maxConns_)
            idle_.push_back(createConnection());
        lastHealthCheck_ = std::time(nullptr);
        return replaced;
    }

    // ── Graceful shutdown ─────────────────────────────────────
    // Stops handing out connections and waits (up to waitMs) until all
    // active queries have finished. Returns true if fully drained.
    bool shutdown(int waitMs = 30000) {
        std::unique_lock<std::mutex> lk(mu_);
        shuttingDown_ = true;
        cv_.notify_all();   // wake queued waiters → they get nullptr
        auto deadline = std::chrono::steady_clock::now()
                      + std::chrono::milliseconds(waitMs);
        while (active_ > 0) {
            if (drainCv_.wait_until(lk, deadline) == std::cv_status::timeout
                && std::chrono::steady_clock::now() >= deadline)
                break;
        }
        bool drained = (active_ == 0);
        idle_.clear();
        totalConns_ = active_;   // only still-active remain
        return drained;
    }

    bool isShuttingDown() const {
        std::lock_guard<std::mutex> lk(mu_);
        return shuttingDown_;
    }

    // Re-open after shutdown (used by tests).
    void reopen() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            shuttingDown_ = false;
        }
        prewarm();
    }

    // ── Stats ─────────────────────────────────────────────────
    int activeCount()  const { std::lock_guard<std::mutex> lk(mu_); return active_; }
    int idleCount()    const { std::lock_guard<std::mutex> lk(mu_); return (int)idle_.size(); }
    int waitingCount() const { std::lock_guard<std::mutex> lk(mu_); return waiting_; }
    int totalCount()   const { std::lock_guard<std::mutex> lk(mu_); return totalConns_; }
    long long timeoutCount() const { std::lock_guard<std::mutex> lk(mu_); return timeouts_; }
    long long deadReplacedCount() const { std::lock_guard<std::mutex> lk(mu_); return deadReplaced_; }

    // JSON for HTTP endpoint /pool/stats
    std::string statsJson() const {
        std::lock_guard<std::mutex> lk(mu_);
        double avgWait = totalRequests_ > 0
            ? totalWaitMs_ / (double)totalRequests_ : 0.0;
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2);
        oss << "{"
            << "\"active\":"          << active_            << ","
            << "\"idle\":"            << idle_.size()       << ","
            << "\"waiting\":"         << waiting_           << ","
            << "\"total\":"           << totalConns_        << ","
            << "\"min\":"             << minConns_          << ","
            << "\"max\":"             << maxConns_          << ","
            << "\"max_wait_ms\":"     << maxWaitMs_         << ","
            << "\"total_requests\":"  << totalRequests_     << ","
            << "\"timeouts\":"        << timeouts_          << ","
            << "\"dead_replaced\":"   << deadReplaced_      << ","
            << "\"avg_wait_ms\":"     << avgWait            << ","
            << "\"mode\":\""          << modeStr()          << "\","
            << "\"shutting_down\":"   << (shuttingDown_ ? "true" : "false")
            << "}";
        return oss.str();
    }

    // Legacy text output (SHOW POOL STATUS)
    std::string showStatus() const {
        std::lock_guard<std::mutex> lk(mu_);
        double avgWait = totalRequests_ > 0
            ? totalWaitMs_ / (double)totalRequests_ : 0.0;
        std::ostringstream oss;
        oss << "Pool Status:\n";
        oss << "  Pool Mode:           " << modeStr()        << "\n";
        oss << "  Min Connections:     " << minConns_        << "\n";
        oss << "  Max Connections:     " << maxConns_        << "\n";
        oss << "  Active Connections:  " << active_          << "\n";
        oss << "  Idle Connections:    " << idle_.size()     << "\n";
        oss << "  Waiting Clients:     " << waiting_         << "\n";
        oss << "  Total Requests:      " << totalRequests_   << "\n";
        oss << "  Timeouts:            " << timeouts_        << "\n";
        oss << "  Dead Replaced:       " << deadReplaced_    << "\n";
        oss << "  Max Wait (ms):       " << maxWaitMs_       << "\n";
        oss << std::fixed << std::setprecision(2);
        oss << "  Avg Wait Time:       " << avgWait << " ms\n";
        return oss.str();
    }

private:
    // must hold mu_
    ConnPtr createConnection() {
        auto c = std::make_shared<PooledConnection>();
        c->id = nextId_++;
        c->createdAt  = std::chrono::steady_clock::now();
        c->lastUsedAt = c->createdAt;
        totalConns_++;
        return c;
    }

    // must hold mu_
    void activate(const ConnPtr& c, std::chrono::steady_clock::time_point t0) {
        active_++;
        c->useCount++;
        c->lastUsedAt = std::chrono::steady_clock::now();
        totalWaitMs_ += std::chrono::duration<double, std::milli>(
                            c->lastUsedAt - t0).count();
    }

    void prewarm() {
        std::lock_guard<std::mutex> lk(mu_);
        if (shuttingDown_) return;
        while (totalConns_ < minConns_)
            idle_.push_back(createConnection());
    }

    static HealthCheckFn defaultHealthCheck() {
        return [](const PooledConnection& c) { return c.healthy.load(); };
    }

    std::string modeStr() const {
        return mode_ == PoolMode::TRANSACTION ? "transaction"
             : mode_ == PoolMode::STATEMENT   ? "statement" : "session";
    }

    mutable std::mutex mu_;
    std::condition_variable cv_;        // waiters for a free connection
    std::condition_variable drainCv_;   // graceful shutdown drain

    std::deque<ConnPtr> idle_;
    int  minConns_   = DEFAULT_MIN;
    int  maxConns_   = DEFAULT_MAX;
    int  maxWaitMs_  = DEFAULT_WAIT_MS;
    int  active_     = 0;
    int  waiting_    = 0;
    int  totalConns_ = 0;
    bool shuttingDown_ = false;

    long long totalRequests_ = 0;
    long long timeouts_      = 0;
    long long deadReplaced_  = 0;
    double    totalWaitMs_   = 0.0;
    uint64_t  nextId_        = 1;
    std::time_t lastHealthCheck_ = 0;

    PoolMode mode_ = PoolMode::SESSION;

    HealthCheckFn     healthFn_;
    std::atomic<bool> healthRunning_{false};
    std::thread       healthThread_;
};

// RAII lease: releases the connection back to the pool on scope exit.
class PoolLease {
public:
    PoolLease() = default;
    PoolLease(ConnectionPool& pool, int timeoutMs = -1)
        : pool_(&pool), conn_(pool.acquire(timeoutMs)) {}
    ~PoolLease() { if (pool_ && conn_) pool_->release(conn_); }

    PoolLease(const PoolLease&)            = delete;
    PoolLease& operator=(const PoolLease&) = delete;
    PoolLease(PoolLease&& o) noexcept : pool_(o.pool_), conn_(std::move(o.conn_)) {
        o.pool_ = nullptr; o.conn_.reset();
    }
    PoolLease& operator=(PoolLease&& o) noexcept {
        if (this != &o) {
            if (pool_ && conn_) pool_->release(conn_);
            pool_ = o.pool_; conn_ = std::move(o.conn_);
            o.pool_ = nullptr; o.conn_.reset();
        }
        return *this;
    }

    explicit operator bool() const { return (bool)conn_; }
    PooledConnection* operator->() const { return conn_.get(); }
    PooledConnection* get() const { return conn_.get(); }

private:
    ConnectionPool* pool_ = nullptr;
    ConnPtr         conn_;
};

// Global pool shared by dispatch.hpp (SQL commands) and the HTTP server.
// C++17 inline variable → single instance across all translation units.
inline ConnectionPool g_connectionPool;

} // namespace milansql
