#pragma once
#include <string>
#include <mutex>
#include <sstream>
#include <iomanip>

namespace milansql {

enum class PoolMode { SESSION, TRANSACTION, STATEMENT };

struct ConnPoolStats {
    int maxConnections   = 50;
    int activeConns      = 0;
    int idleConns        = 50;
    int waitingClients   = 0;
    long long totalReqs  = 0;
    double totalWaitMs   = 0.0;
    PoolMode mode        = PoolMode::SESSION;
    int maxWaitMs        = 5000;
};

class ConnectionPool {
public:
    ConnectionPool() {
        stats_.idleConns = stats_.maxConnections;
    }

    void setMode(const std::string& mode) {
        std::lock_guard<std::mutex> lk(mu_);
        if (mode == "transaction")      stats_.mode = PoolMode::TRANSACTION;
        else if (mode == "statement")   stats_.mode = PoolMode::STATEMENT;
        else                            stats_.mode = PoolMode::SESSION;
    }

    void setMaxConnections(int n) {
        std::lock_guard<std::mutex> lk(mu_);
        stats_.maxConnections = n;
        stats_.idleConns = n - stats_.activeConns;
    }

    void setMaxWait(int ms) {
        std::lock_guard<std::mutex> lk(mu_);
        stats_.maxWaitMs = ms;
    }

    // Simulate acquiring a connection (for tracking)
    bool acquire(int timeoutMs = -1) {
        (void)timeoutMs;
        std::lock_guard<std::mutex> lk(mu_);
        stats_.totalReqs++;
        if (stats_.activeConns < stats_.maxConnections) {
            stats_.activeConns++;
            stats_.idleConns = stats_.maxConnections - stats_.activeConns;
            return true;
        }
        stats_.waitingClients++;
        return false; // would block in real impl
    }

    void release() {
        std::lock_guard<std::mutex> lk(mu_);
        if (stats_.activeConns > 0) {
            stats_.activeConns--;
            stats_.idleConns = stats_.maxConnections - stats_.activeConns;
        }
        if (stats_.waitingClients > 0) stats_.waitingClients--;
    }

    PoolMode getMode() const { return stats_.mode; }

    std::string showStatus() const {
        std::lock_guard<std::mutex> lk(mu_);
        std::string modeStr =
            stats_.mode == PoolMode::TRANSACTION ? "transaction" :
            stats_.mode == PoolMode::STATEMENT   ? "statement"   : "session";
        double avgWait = stats_.totalReqs > 0
            ? stats_.totalWaitMs / stats_.totalReqs : 0.0;
        std::ostringstream oss;
        oss << "Pool Status:\n";
        oss << "  Pool Mode:           " << modeStr << "\n";
        oss << "  Max Connections:     " << stats_.maxConnections << "\n";
        oss << "  Active Connections:  " << stats_.activeConns << "\n";
        oss << "  Idle Connections:    " << stats_.idleConns << "\n";
        oss << "  Waiting Clients:     " << stats_.waitingClients << "\n";
        oss << "  Total Requests:      " << stats_.totalReqs << "\n";
        oss << "  Max Wait (ms):       " << stats_.maxWaitMs << "\n";
        oss << std::fixed << std::setprecision(2);
        oss << "  Avg Wait Time:       " << avgWait << " ms\n";
        return oss.str();
    }

private:
    mutable std::mutex mu_;
    ConnPoolStats stats_;
};

} // namespace milansql
