#pragma once
// mtls_manager.hpp — Phase 5.3: Mutual TLS
#include <string>
#include <mutex>
#include <fstream>

namespace milansql {

class MtlsManager {
public:
    static MtlsManager& instance() {
        static MtlsManager m;
        return m;
    }

    bool enabled() const { return enabled_; }
    std::string caPath() const { return caPath_; }

    std::string enable(const std::string& caPath) {
        std::lock_guard<std::mutex> lk(mutex_);
        enabled_ = true;
        caPath_  = caPath;
        return "mTLS enabled. CA: " + (caPath.empty() ? "(default)" : caPath);
    }

    std::string disable() {
        std::lock_guard<std::mutex> lk(mutex_);
        enabled_ = false;
        caPath_.clear();
        return "mTLS disabled.";
    }

    std::string statusJson() const {
        return "{\"mtls_enabled\":" + std::string(enabled_ ? "true" : "false")
             + ",\"ca_path\":\"" + caPath_ + "\"}";
    }

private:
    bool enabled_ = false;
    std::string caPath_;
    mutable std::mutex mutex_;
};

} // namespace milansql
