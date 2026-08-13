#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <algorithm>

namespace milansql {

// ── CloudRegion ───────────────────────────────────────────────────────────────
enum class RegionStatus {
    ACTIVE,
    PLANNED,
    MAINTENANCE,
    DISABLED
};

inline std::string regionStatusToString(RegionStatus s) {
    switch (s) {
        case RegionStatus::ACTIVE:      return "active";
        case RegionStatus::PLANNED:     return "planned";
        case RegionStatus::MAINTENANCE: return "maintenance";
        case RegionStatus::DISABLED:    return "disabled";
    }
    return "unknown";
}

struct CloudRegion {
    std::string  id;        // e.g. "eu-central-1"
    std::string  name;      // e.g. "EU Central"
    std::string  location;  // e.g. "Falkenstein, Germany"
    RegionStatus status;
    int          latencyMs; // estimated latency from Western Europe
};

// ── Replica record ────────────────────────────────────────────────────────────
struct Replica {
    std::string replicaId;
    std::string instanceId;
    std::string regionId;
    std::string status;    // "active" | "syncing" | "error"
    std::string createdAt;
};

// ── RegionManager ─────────────────────────────────────────────────────────────
class RegionManager {
public:
    static RegionManager& instance() {
        static RegionManager m;
        return m;
    }
public:
    explicit RegionManager(const std::string& dataDir = "/opt/milansql/data")
        : dataDir_(dataDir) {
        initStaticRegions();
        filePath_ = dataDir_ + "/cloud_replicas.data";
        loadFromDisk();
    }

    // List all regions as JSON array
    std::string listJson() const {
        std::lock_guard<std::mutex> lk(mu_);
        std::ostringstream j;
        j << "[";
        bool first = true;
        for (auto& [k, r] : regions_) {
            if (!first) j << ",";
            first = false;
            j << regionToJson(r);
        }
        j << "]";
        return j.str();
    }

    // Check if a region accepts new instances
    bool isAvailable(const std::string& regionId) const {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = regions_.find(regionId);
        if (it == regions_.end()) return false;
        return it->second.status == RegionStatus::ACTIVE;
    }

    // Get a single region (throws if not found)
    CloudRegion getRegion(const std::string& regionId) const {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = regions_.find(regionId);
        if (it == regions_.end())
            throw std::runtime_error("Unknown region: " + regionId);
        return it->second;
    }

    // Create a read replica of instanceId in the specified region
    Replica createReplica(const std::string& instanceId,
                          const std::string& regionId) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = regions_.find(regionId);
        if (it == regions_.end())
            throw std::runtime_error("Unknown region: " + regionId);
        // Allow PLANNED regions — replica will be "pending" until region goes live
        bool isActive = (it->second.status == RegionStatus::ACTIVE);

        Replica rep;
        rep.replicaId  = "rep_" + hexRandom(4);
        rep.instanceId = instanceId;
        rep.regionId   = regionId;
        rep.status     = isActive ? "syncing" : "pending";
        rep.createdAt  = currentIsoTimestamp();

        replicas_[rep.replicaId] = rep;
        saveToDisk();
        return rep;
    }

    // List all replicas for an instance
    std::vector<Replica> listReplicas(const std::string& instanceId) const {
        std::lock_guard<std::mutex> lk(mu_);
        std::vector<Replica> result;
        for (auto& [k, v] : replicas_)
            if (v.instanceId == instanceId)
                result.push_back(v);
        return result;
    }

    // List replicas as JSON array
    std::string listReplicasJson(const std::string& instanceId) const {
        auto reps = listReplicas(instanceId);
        std::ostringstream j;
        j << "[";
        for (size_t i = 0; i < reps.size(); ++i) {
            if (i) j << ",";
            j << replicaToJson(reps[i]);
        }
        j << "]";
        return j.str();
    }

    // Delete a replica
    void deleteReplica(const std::string& replicaId) {
        std::lock_guard<std::mutex> lk(mu_);
        replicas_.erase(replicaId);
        saveToDisk();
    }

private:
    std::string dataDir_;
    std::string filePath_;
    mutable std::mutex mu_;

    std::unordered_map<std::string, CloudRegion> regions_;
    std::unordered_map<std::string, Replica>     replicas_;

    // Static region catalogue
    void initStaticRegions() {
        regions_ = {
            {"eu-central-1", {"eu-central-1", "EU Central",      "Falkenstein, Germany",  RegionStatus::ACTIVE,      5}},
            {"eu-west-1",    {"eu-west-1",    "EU West",         "Helsinki, Finland",     RegionStatus::PLANNED,    15}},
            {"us-east-1",    {"us-east-1",    "US East",         "Ashburn, VA, USA",      RegionStatus::PLANNED,    90}},
            {"ap-southeast-1",{"ap-southeast-1","Asia Pacific",  "Singapore",             RegionStatus::PLANNED,   180}},
        };
    }

    static std::string regionToJson(const CloudRegion& r) {
        std::ostringstream j;
        j << "{"
          << "\"id\":\""       << r.id       << "\","
          << "\"name\":\""     << r.name     << "\","
          << "\"location\":\"" << r.location << "\","
          << "\"status\":\""   << regionStatusToString(r.status) << "\","
          << "\"latencyMs\":"  << r.latencyMs
          << "}";
        return j.str();
    }

    static std::string replicaToJson(const Replica& r) {
        std::ostringstream j;
        j << "{"
          << "\"replicaId\":\""  << r.replicaId  << "\","
          << "\"instanceId\":\"" << r.instanceId << "\","
          << "\"regionId\":\""   << r.regionId   << "\","
          << "\"status\":\""     << r.status     << "\","
          << "\"createdAt\":\""  << r.createdAt  << "\""
          << "}";
        return j.str();
    }

    static std::string hexRandom(size_t bytes) {
        // Simple LCG-based hex for replica IDs (not security-critical)
        static unsigned long long state = 0x123456789ABCDEFULL;
        auto now = std::chrono::system_clock::now().time_since_epoch().count();
        state ^= static_cast<unsigned long long>(now);
        std::ostringstream oss;
        oss << std::hex;
        for (size_t i = 0; i < bytes; ++i) {
            state = state * 6364136223846793005ULL + 1442695040888963407ULL;
            unsigned v = (state >> 33) & 0xFF;
            if (v < 16) oss << "0";
            oss << v;
        }
        return oss.str();
    }

    static std::string currentIsoTimestamp() {
        auto now = std::chrono::system_clock::now();
        std::time_t t = std::chrono::system_clock::to_time_t(now);
        char buf[32];
        struct tm tm_buf;
#ifdef _WIN32
        gmtime_s(&tm_buf, &t);
#else
        gmtime_r(&t, &tm_buf);
#endif
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
        return std::string(buf);
    }

    // Persistence: one line per replica
    // format: replicaId|instanceId|regionId|status|createdAt
    static std::string replicaToLine(const Replica& r) {
        return r.replicaId + "|" + r.instanceId + "|" +
               r.regionId  + "|" + r.status     + "|" + r.createdAt;
    }

    static Replica lineToReplica(const std::string& line) {
        Replica r;
        std::vector<std::string> parts;
        std::istringstream ss(line);
        std::string tok;
        while (std::getline(ss, tok, '|')) parts.push_back(tok);
        if (parts.size() < 5) return r;
        r.replicaId  = parts[0];
        r.instanceId = parts[1];
        r.regionId   = parts[2];
        r.status     = parts[3];
        r.createdAt  = parts[4];
        return r;
    }

    void loadFromDisk() {
        std::ifstream f(filePath_);
        if (!f.is_open()) return;
        std::string line;
        while (std::getline(f, line)) {
            if (line.empty() || line[0] == '#') continue;
            Replica r = lineToReplica(line);
            if (!r.replicaId.empty())
                replicas_[r.replicaId] = r;
        }
    }

    void saveToDisk() {
        std::ofstream f(filePath_, std::ios::trunc);
        if (!f.is_open()) return;
        f << "# MilanSQL Cloud Replicas\n";
        for (auto& [k, v] : replicas_)
            f << replicaToLine(v) << "\n";
    }
};

} // namespace milansql
