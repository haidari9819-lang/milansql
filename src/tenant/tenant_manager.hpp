#pragma once
#include <string>
#include <map>
#include <vector>
#include <sstream>
#include <algorithm>

// ============================================================
// tenant_manager.hpp — Multi-Tenant Support (Phase 127)
// ============================================================

namespace milansql {

struct TenantDef {
    std::string name;
    int maxConnections = 100;
    double maxStorageGB = 10.0;
    int maxTables = 200;
    std::string dataDir;   // "data/tenants/<name>/"
    int currentConnections = 0;
    int currentTables = 0;
    double currentStorageMB = 0.0;
    bool active = true;
};

class TenantManager {
public:
    std::string activeTenant;  // empty = default (no tenant)

    TenantManager() {
        // Default tenant always exists
        TenantDef def;
        def.name = "default";
        def.dataDir = "data/tenants/default/";
        tenants_["default"] = def;
        activeTenant = "default";
    }

    bool createTenant(const std::string& name,
                      int maxConn = 100,
                      double maxStorageGB = 10.0,
                      int maxTables = 200) {
        if (tenants_.count(name)) return false;
        TenantDef def;
        def.name = name;
        def.maxConnections = maxConn;
        def.maxStorageGB = maxStorageGB;
        def.maxTables = maxTables;
        def.dataDir = "data/tenants/" + name + "/";
        tenants_[name] = def;
        return true;
    }

    bool dropTenant(const std::string& name) {
        if (name == "default") return false;
        return tenants_.erase(name) > 0;
    }

    bool switchTenant(const std::string& name) {
        if (!tenants_.count(name)) return false;
        activeTenant = name;
        return true;
    }

    TenantDef* getTenant(const std::string& name) {
        auto it = tenants_.find(name);
        if (it == tenants_.end()) return nullptr;
        return &it->second;
    }

    TenantDef* getActive() {
        return getTenant(activeTenant);
    }

    std::vector<TenantDef> allTenants() const {
        std::vector<TenantDef> result;
        for (auto& [n, d] : tenants_) result.push_back(d);
        return result;
    }

    size_t count() const { return tenants_.size(); }

    std::string getTenantDataDir(const std::string& name) const {
        auto it = tenants_.find(name);
        if (it == tenants_.end()) return "";
        return it->second.dataDir;
    }

private:
    std::map<std::string, TenantDef> tenants_;
};

} // namespace milansql
