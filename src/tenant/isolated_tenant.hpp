#pragma once
// isolated_tenant.hpp — Phase 5.5: Dedicated Tenant Isolation
#include <string>
#include <map>
#include <mutex>
#include <fstream>
#include <sstream>
#include <ctime>

namespace milansql {

struct IsolatedTenant {
    std::string name;
    std::string storagePath;
    int         memoryMB  = 512;
    int         cpuCores  = 1;
    std::string createdAt;
};

class IsolatedTenantManager {
public:
    static IsolatedTenantManager& instance() {
        static IsolatedTenantManager m;
        return m;
    }

    std::string create(const std::string& name, const std::string& config) {
        std::lock_guard<std::mutex> lk(mutex_);
        if (tenants_.count(name))
            return "ERROR: Isolated tenant '" + name + "' already exists.";
        IsolatedTenant t;
        t.name = name;
        auto parse = [&](const std::string& key) -> std::string {
            auto pos = config.find(key + "=");
            if (pos == std::string::npos) return "";
            auto end = config.find(';', pos);
            return config.substr(pos + key.size() + 1,
                end == std::string::npos ? std::string::npos : end - pos - key.size() - 1);
        };
        auto mem = parse("MEMORY");
        if (!mem.empty()) { try { t.memoryMB = std::stoi(mem); } catch (...) {} }
        auto cpu = parse("CPU");
        if (!cpu.empty()) { try { t.cpuCores = std::stoi(cpu); } catch (...) {} }
        auto storage = parse("STORAGE");
        t.storagePath = storage.empty() ? ("/data/tenants/" + name + "/") : storage;
        int rc = std::system(("mkdir -p " + t.storagePath + " 2>/dev/null").c_str());
        (void)rc;
        std::time_t now = std::time(nullptr);
        char buf[32]; struct tm ltm;
        localtime_r(&now, &ltm);
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &ltm);
        t.createdAt = buf;
        tenants_[name] = t;
        save();
        return "Isolated tenant '" + name + "' created. Storage: " + t.storagePath
             + " Memory: " + std::to_string(t.memoryMB) + "MB CPU: " + std::to_string(t.cpuCores);
    }

    std::string drop(const std::string& name) {
        std::lock_guard<std::mutex> lk(mutex_);
        if (!tenants_.count(name))
            return "ERROR: Isolated tenant '" + name + "' not found.";
        tenants_.erase(name);
        save();
        return "Isolated tenant '" + name + "' dropped.";
    }

    bool exists(const std::string& name) const {
        std::lock_guard<std::mutex> lk(mutex_);
        return tenants_.count(name) > 0;
    }

    std::string listJson() const {
        std::lock_guard<std::mutex> lk(mutex_);
        std::string j = "{\"isolated_tenants\":[";
        bool first = true;
        for (const auto& p : tenants_) {
            const auto& t = p.second;
            if (!first) j += ",";
            j += "{\"name\":\"" + t.name + "\""
               + ",\"storage\":\"" + t.storagePath + "\""
               + ",\"memory_mb\":" + std::to_string(t.memoryMB)
               + ",\"cpu_cores\":" + std::to_string(t.cpuCores)
               + ",\"created_at\":\"" + t.createdAt + "\"}";
            first = false;
        }
        j += "],\"count\":" + std::to_string(tenants_.size()) + "}";
        return j;
    }

private:
    mutable std::mutex mutex_;
    std::map<std::string, IsolatedTenant> tenants_;
    const char* FILE_ = "database.milan.isolated_tenants";

    void save() const {
        std::ofstream f(FILE_);
        for (const auto& p : tenants_) {
            const auto& t = p.second;
            f << t.name << "\t" << t.storagePath << "\t"
              << t.memoryMB << "\t" << t.cpuCores << "\t" << t.createdAt << "\n";
        }
    }
};

} // namespace milansql
