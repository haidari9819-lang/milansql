#pragma once
#include <string>
#include <vector>
#include <atomic>
#include <algorithm>
#include <sstream>
#include <chrono>

// ============================================================
// sentinel.hpp — Automatic Failover + High Availability (Phase 128)
// ============================================================

namespace milansql {

struct MonitoredNode {
    std::string host;
    int port = 4406;
    bool isAlive = true;
    bool isMaster = false;
    int failedChecks = 0;
    int replicationLag = 0; // in ms, for slave selection
    long long lastCheckMs = 0;
};

class Sentinel {
public:
    int failoverThreshold = 3;   // checks before failover
    int checkIntervalMs = 1000;  // 1 second
    bool sentinelActive = false;
    std::string currentMasterHost;
    int currentMasterPort = 4406;

    void addMonitor(const std::string& host, int port, bool isMaster = false) {
        MonitoredNode node;
        node.host = host;
        node.port = port;
        node.isMaster = isMaster;
        node.isAlive = true;
        if (isMaster || monitored_.empty()) {
            node.isMaster = true;
            currentMasterHost = host;
            currentMasterPort = port;
        }
        monitored_.push_back(node);
        sentinelActive = true;
    }

    // Simulate a health check (in real impl, would ping via TCP)
    bool checkNode(int idx) {
        if (idx < 0 || idx >= (int)monitored_.size()) return false;
        // Simulation: always alive in test mode
        monitored_[idx].lastCheckMs = nowMs();
        return monitored_[idx].isAlive;
    }

    // Mark a node as down (for testing)
    void markDown(const std::string& host, int port) {
        for (auto& n : monitored_)
            if (n.host == host && n.port == port) { n.isAlive = false; n.failedChecks = failoverThreshold; }
    }

    // Elect new master from alive slaves
    std::string electNewMaster() {
        // Choose slave with lowest replication lag
        MonitoredNode* best = nullptr;
        for (auto& n : monitored_) {
            if (!n.isMaster && n.isAlive) {
                if (!best || n.replicationLag < best->replicationLag) best = &n;
            }
        }
        if (!best) return "";
        best->isMaster = true;
        currentMasterHost = best->host;
        currentMasterPort = best->port;
        // Demote old master
        for (auto& n : monitored_)
            if (&n != best && n.isMaster) n.isMaster = false;
        return best->host + ":" + std::to_string(best->port);
    }

    const std::vector<MonitoredNode>& nodes() const { return monitored_; }
    size_t nodeCount() const { return monitored_.size(); }

    std::string statusSummary() const {
        int alive = 0, masters = 0, slaves = 0;
        for (auto& n : monitored_) {
            if (n.isAlive) alive++;
            if (n.isMaster) masters++;
            else slaves++;
        }
        return "Nodes=" + std::to_string(monitored_.size()) +
               " Alive=" + std::to_string(alive) +
               " Masters=" + std::to_string(masters) +
               " Slaves=" + std::to_string(slaves);
    }

private:
    std::vector<MonitoredNode> monitored_;

    static long long nowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }
};

} // namespace milansql
