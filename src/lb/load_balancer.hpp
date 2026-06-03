#pragma once
#include <string>
#include <vector>
#include <atomic>
#include <algorithm>
#include <sstream>

enum class RoutingMode { AUTO, MASTER, SLAVE };

struct BackendInfo {
    std::string host;
    int port = 4406;
    bool isAlive = true;
    std::atomic<int> currentConnections{0};
    int totalQueries = 0;

    BackendInfo() = default;
    BackendInfo(const BackendInfo& o)
        : host(o.host), port(o.port), isAlive(o.isAlive),
          currentConnections(o.currentConnections.load()),
          totalQueries(o.totalQueries) {}
    BackendInfo& operator=(const BackendInfo& o) {
        host = o.host; port = o.port; isAlive = o.isAlive;
        currentConnections.store(o.currentConnections.load());
        totalQueries = o.totalQueries;
        return *this;
    }
};

class LoadBalancer {
public:
    RoutingMode routingMode = RoutingMode::AUTO;

    void addBackend(const std::string& host, int port) {
        BackendInfo b;
        b.host = host;
        b.port = port;
        b.isAlive = true;
        backends_.push_back(b);
    }

    // Returns index of next backend (round-robin among alive backends)
    int roundRobin() {
        if (backends_.empty()) return -1;
        int start = rrIndex_.fetch_add(1) % (int)backends_.size();
        for (int i = 0; i < (int)backends_.size(); i++) {
            int idx = (start + i) % (int)backends_.size();
            if (backends_[idx].isAlive) return idx;
        }
        return -1; // all down
    }

    // Determine if SQL is a write query
    static bool isWriteQuery(const std::string& sql) {
        std::string upper = sql;
        std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);
        // strip leading whitespace
        size_t p = upper.find_first_not_of(" \t\n\r");
        if (p == std::string::npos) return false;
        upper = upper.substr(p);
        const char* writes[] = {"INSERT","UPDATE","DELETE","CREATE","DROP",
                                "ALTER","TRUNCATE","BEGIN","COMMIT","ROLLBACK","GRANT","REVOKE"};
        for (auto& w : writes)
            if (upper.rfind(w, 0) == 0) return true;
        return false;
    }

    // Route: returns backend index
    int route(const std::string& sql) {
        if (backends_.empty()) return -1;
        if (routingMode == RoutingMode::MASTER) return 0;
        if (routingMode == RoutingMode::SLAVE) {
            // round-robin among non-master (index > 0), fallback to master
            if (backends_.size() > 1) {
                int idx = 1 + (rrIndex_.fetch_add(1) % (int)(backends_.size() - 1));
                return backends_[idx].isAlive ? idx : 0;
            }
            return 0;
        }
        // AUTO: writes -> master (index 0), reads -> round-robin
        if (isWriteQuery(sql)) return 0;
        return roundRobin();
    }

    void markAlive(int idx, bool alive) {
        if (idx >= 0 && idx < (int)backends_.size())
            backends_[idx].isAlive = alive;
    }

    const std::vector<BackendInfo>& backends() const { return backends_; }
    size_t size() const { return backends_.size(); }

    std::string routingModeStr() const {
        if (routingMode == RoutingMode::AUTO)   return "AUTO";
        if (routingMode == RoutingMode::MASTER) return "MASTER";
        return "SLAVE";
    }

private:
    std::vector<BackendInfo> backends_;
    std::atomic<int> rrIndex_{0};
};
