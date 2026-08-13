#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <fstream>
#include <sstream>
#include <chrono>
#include <random>
#include <algorithm>
#include <stdexcept>
#include <ctime>

namespace milansql {

// ── Plan definitions ──────────────────────────────────────────────────────────
enum class CloudPlan {
    FREE,
    STARTER,
    PRO,
    ENTERPRISE
};

inline std::string planToString(CloudPlan p) {
    switch (p) {
        case CloudPlan::FREE:       return "free";
        case CloudPlan::STARTER:    return "starter";
        case CloudPlan::PRO:        return "pro";
        case CloudPlan::ENTERPRISE: return "enterprise";
    }
    return "free";
}

inline CloudPlan planFromString(const std::string& s) {
    if (s == "starter")    return CloudPlan::STARTER;
    if (s == "pro")        return CloudPlan::PRO;
    if (s == "enterprise") return CloudPlan::ENTERPRISE;
    return CloudPlan::FREE;
}

struct PlanLimits {
    std::string name;
    long long   storageBytes;       // -1 = unlimited
    int         queriesPerDay;      // -1 = unlimited
    double      priceMonthly;       // EUR
    bool        prioritySupport;
};

inline PlanLimits getPlanLimits(CloudPlan plan) {
    switch (plan) {
        case CloudPlan::FREE:
            return {"free", 500LL * 1024 * 1024, 100, 0.0, false};
        case CloudPlan::STARTER:
            return {"starter", 5LL * 1024 * 1024 * 1024, -1, 9.0, false};
        case CloudPlan::PRO:
            return {"pro", 50LL * 1024 * 1024 * 1024, -1, 49.0, true};
        case CloudPlan::ENTERPRISE:
            return {"enterprise", -1LL, -1, 0.0, true};
    }
    return {"free", 500LL * 1024 * 1024, 100, 0.0, false};
}

// ── CloudInstance struct ──────────────────────────────────────────────────────
enum class InstanceStatus {
    PROVISIONING,
    RUNNING,
    PAUSED,
    DELETED
};

inline std::string statusToString(InstanceStatus s) {
    switch (s) {
        case InstanceStatus::PROVISIONING: return "provisioning";
        case InstanceStatus::RUNNING:      return "running";
        case InstanceStatus::PAUSED:       return "paused";
        case InstanceStatus::DELETED:      return "deleted";
    }
    return "unknown";
}

inline InstanceStatus statusFromString(const std::string& s) {
    if (s == "provisioning") return InstanceStatus::PROVISIONING;
    if (s == "running")      return InstanceStatus::RUNNING;
    if (s == "paused")       return InstanceStatus::PAUSED;
    if (s == "deleted")      return InstanceStatus::DELETED;
    return InstanceStatus::PROVISIONING;
}

struct CloudInstance {
    std::string    id;
    std::string    userId;
    std::string    name;
    CloudPlan      plan;
    std::string    region;
    InstanceStatus status;
    std::string    createdAt;
    std::string    apiKeyHash;
    std::string    apiKeyPlain; // only populated at creation, empty otherwise
    long long      queryCount;
    long long      storageBytes;
};

// ── Helpers ───────────────────────────────────────────────────────────────────
namespace detail {

inline std::string hexRandom(size_t bytes) {
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint32_t> dist(0, 255);
    std::ostringstream oss;
    oss << std::hex;
    for (size_t i = 0; i < bytes; ++i) {
        unsigned v = dist(gen);
        if (v < 16) oss << "0";
        oss << v;
    }
    return oss.str();
}

inline std::string currentIsoTimestamp() {
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

// FNV-1a 64-bit hash used as API key fingerprint (replace with SHA-256 in production)
inline std::string hashApiKey(const std::string& key) {
    uint64_t hash = 14695981039346656037ULL;
    for (unsigned char c : key) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    std::ostringstream oss;
    oss << std::hex << hash;
    return oss.str();
}

inline std::string jsonEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if      (c == '"')  out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else                out += c;
    }
    return out;
}

inline std::string instanceToLine(const CloudInstance& inst) {
    return inst.id + "|" + inst.userId + "|" + inst.name + "|" +
           planToString(inst.plan) + "|" + inst.region + "|" +
           statusToString(inst.status) + "|" + inst.createdAt + "|" +
           inst.apiKeyHash + "|" + std::to_string(inst.queryCount) + "|" +
           std::to_string(inst.storageBytes);
}

inline CloudInstance lineToInstance(const std::string& line) {
    CloudInstance inst;
    std::vector<std::string> parts;
    std::istringstream ss(line);
    std::string tok;
    while (std::getline(ss, tok, '|')) parts.push_back(tok);
    if (parts.size() < 10) return inst;
    inst.id           = parts[0];
    inst.userId       = parts[1];
    inst.name         = parts[2];
    inst.plan         = planFromString(parts[3]);
    inst.region       = parts[4];
    inst.status       = statusFromString(parts[5]);
    inst.createdAt    = parts[6];
    inst.apiKeyHash   = parts[7];
    inst.queryCount   = std::stoll(parts[8]);
    inst.storageBytes = std::stoll(parts[9]);
    return inst;
}

inline std::string instanceToJson(const CloudInstance& inst) {
    PlanLimits limits = getPlanLimits(inst.plan);
    std::ostringstream j;
    j << "{"
      << "\"id\":\""     << jsonEscape(inst.id)     << "\","
      << "\"userId\":\"" << jsonEscape(inst.userId) << "\","
      << "\"name\":\""   << jsonEscape(inst.name)   << "\","
      << "\"plan\":\""   << planToString(inst.plan) << "\","
      << "\"planLimits\":{"
        << "\"storageBytes\":"  << limits.storageBytes   << ","
        << "\"queriesPerDay\":" << limits.queriesPerDay  << ","
        << "\"priceMonthly\":"  << limits.priceMonthly
      << "},"
      << "\"region\":\""    << jsonEscape(inst.region)                  << "\","
      << "\"status\":\""    << statusToString(inst.status)              << "\","
      << "\"createdAt\":\"" << jsonEscape(inst.createdAt)               << "\","
      << "\"queryCount\":"  << inst.queryCount                          << ","
      << "\"storageBytes\":" << inst.storageBytes;
    if (!inst.apiKeyPlain.empty())
        j << ",\"apiKey\":\"" << jsonEscape(inst.apiKeyPlain) << "\"";
    j << "}";
    return j.str();
}

} // namespace detail

// ── CloudInstanceManager ──────────────────────────────────────────────────────
class CloudInstanceManager {
public:
    static CloudInstanceManager& instance() {
        static CloudInstanceManager mgr;
        return mgr;
    }
public:
    explicit CloudInstanceManager(const std::string& dataDir = "/opt/milansql/data")
        : dataDir_(dataDir) {
        filePath_ = dataDir_ + "/cloud_instances.data";
        loadFromDisk();
    }

    // Create a new instance; returned CloudInstance contains apiKeyPlain (shown once only)
    CloudInstance create(const std::string& userId,
                         const std::string& name,
                         CloudPlan          plan,
                         const std::string& region) {
        std::lock_guard<std::mutex> lk(mu_);

        CloudInstance inst;
        inst.id           = "inst_" + detail::hexRandom(4);
        inst.userId       = userId;
        inst.name         = name;
        inst.plan         = plan;
        inst.region       = region;
        inst.status       = InstanceStatus::RUNNING;
        inst.createdAt    = detail::currentIsoTimestamp();
        inst.apiKeyPlain  = "msk_" + detail::hexRandom(16);
        inst.apiKeyHash   = detail::hashApiKey(inst.apiKeyPlain);
        inst.queryCount   = 0;
        inst.storageBytes = 0;

        // Persist without plain key
        CloudInstance stored = inst;
        stored.apiKeyPlain.clear();
        instances_[inst.id] = stored;
        saveToDisk();

        return inst; // caller receives the plain API key (shown once)
    }

    CloudInstance getById(const std::string& id) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = instances_.find(id);
        if (it == instances_.end())
            throw std::runtime_error("Instance not found: " + id);
        return it->second;
    }

    std::vector<CloudInstance> getByUser(const std::string& userId) {
        std::lock_guard<std::mutex> lk(mu_);
        std::vector<CloudInstance> result;
        for (auto& [k, v] : instances_)
            if (v.userId == userId && v.status != InstanceStatus::DELETED)
                result.push_back(v);
        return result;
    }

    void deleteInstance(const std::string& id) {
        std::lock_guard<std::mutex> lk(mu_);
        requireExists(id).status = InstanceStatus::DELETED;
        saveToDisk();
    }

    void pauseInstance(const std::string& id) {
        std::lock_guard<std::mutex> lk(mu_);
        auto& inst = requireExists(id);
        if (inst.status != InstanceStatus::RUNNING)
            throw std::runtime_error("Instance is not running: " + id);
        inst.status = InstanceStatus::PAUSED;
        saveToDisk();
    }

    void resumeInstance(const std::string& id) {
        std::lock_guard<std::mutex> lk(mu_);
        auto& inst = requireExists(id);
        if (inst.status != InstanceStatus::PAUSED)
            throw std::runtime_error("Instance is not paused: " + id);
        inst.status = InstanceStatus::RUNNING;
        saveToDisk();
    }

    void resizeInstance(const std::string& id, CloudPlan newPlan) {
        std::lock_guard<std::mutex> lk(mu_);
        requireExists(id).plan = newPlan;
        saveToDisk();
    }

    void incrementQueryCount(const std::string& id, long long n = 1) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = instances_.find(id);
        if (it == instances_.end()) return;
        it->second.queryCount += n;
        saveToDisk();
    }

    void updateStorage(const std::string& id, long long bytes) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = instances_.find(id);
        if (it == instances_.end()) return;
        it->second.storageBytes = bytes;
        saveToDisk();
    }

    std::string listJson(const std::string& userId) {
        auto list = getByUser(userId);
        std::ostringstream j;
        j << "[";
        for (size_t i = 0; i < list.size(); ++i) {
            if (i) j << ",";
            j << detail::instanceToJson(list[i]);
        }
        j << "]";
        return j.str();
    }

    std::string instanceJson(const std::string& id) {
        return detail::instanceToJson(getById(id));
    }

private:
    std::string filePath_;
    std::string dataDir_;
    std::mutex  mu_;
    std::unordered_map<std::string, CloudInstance> instances_;

    CloudInstance& requireExists(const std::string& id) {
        auto it = instances_.find(id);
        if (it == instances_.end())
            throw std::runtime_error("Instance not found: " + id);
        return it->second;
    }

    void loadFromDisk() {
        std::ifstream f(filePath_);
        if (!f.is_open()) return;
        std::string line;
        while (std::getline(f, line)) {
            if (line.empty() || line[0] == '#') continue;
            CloudInstance inst = detail::lineToInstance(line);
            if (!inst.id.empty())
                instances_[inst.id] = inst;
        }
    }

    void saveToDisk() {
        std::ofstream f(filePath_, std::ios::trunc);
        if (!f.is_open()) return;
        f << "# MilanSQL Cloud Instances\n";
        for (auto& [k, v] : instances_)
            f << detail::instanceToLine(v) << "\n";
    }
};

} // namespace milansql
