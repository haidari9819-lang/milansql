#pragma once
#include <string>
#include <vector>
#include <algorithm>

namespace milansql {

class AccessControl {
    std::vector<std::string> allowHosts_;
    std::vector<std::string> denyHosts_;
    std::vector<std::string> blacklistQueries_;
    int  passwordMinLength_     = 6;
    bool passwordRequireSpecial_ = false;
    int  maxConnectionsPerIp_   = 100;
    int  connectionRateLimit_   = 1000;

public:
    void addAllowHost(const std::string& host) { allowHosts_.push_back(host); }
    void addDenyHost (const std::string& host) { denyHosts_.push_back(host);  }

    bool isHostAllowed(const std::string& host) const {
        for (const auto& d : denyHosts_)  if (d == host) return false;
        if (!allowHosts_.empty()) {
            for (const auto& a : allowHosts_) if (a == host) return true;
            return false;
        }
        return true;
    }

    const std::vector<std::string>& getAllowHosts() const { return allowHosts_; }
    const std::vector<std::string>& getDenyHosts()  const { return denyHosts_;  }

    void addBlacklistQuery(const std::string& pattern) {
        blacklistQueries_.push_back(pattern);
    }

    bool isQueryAllowed(const std::string& sql) const {
        for (const auto& p : blacklistQueries_) {
            if (sql.find(p) != std::string::npos) return false;
        }
        return true;
    }

    bool validatePassword(const std::string& pw) const {
        if ((int)pw.size() < passwordMinLength_) return false;
        if (passwordRequireSpecial_) {
            bool hasSpecial = false;
            for (char c : pw) if (!std::isalnum((unsigned char)c)) { hasSpecial = true; break; }
            if (!hasSpecial) return false;
        }
        return true;
    }

    void setPasswordMinLength     (int n)    { passwordMinLength_      = n;  }
    void setPasswordRequireSpecial(bool on)  { passwordRequireSpecial_ = on; }
    void setMaxConnectionsPerIp   (int n)    { maxConnectionsPerIp_    = n;  }
    void setConnectionRateLimit   (int n)    { connectionRateLimit_    = n;  }
};

} // namespace milansql
