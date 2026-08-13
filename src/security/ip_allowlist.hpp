#pragma once
// ip_allowlist.hpp — Phase 5.3: Per-user IP allowlisting
#include <string>
#include <map>
#include <vector>
#include <mutex>
#include <fstream>
#include <sstream>

namespace milansql {

class IpAllowlist {
public:
    static IpAllowlist& instance() {
        static IpAllowlist ia;
        return ia;
    }

    void setAllowed(const std::string& username, const std::string& ipList) {
        std::lock_guard<std::mutex> lk(mutex_);
        allowed_[username] = parseList(ipList);
        save();
    }

    void removeAllowed(const std::string& username) {
        std::lock_guard<std::mutex> lk(mutex_);
        allowed_.erase(username);
        save();
    }

    bool isAllowed(const std::string& username, const std::string& ip) const {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = allowed_.find(username);
        if (it == allowed_.end()) return true;
        for (const auto& entry : it->second)
            if (matches(ip, entry)) return true;
        return false;
    }

    std::string getList(const std::string& username) const {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = allowed_.find(username);
        if (it == allowed_.end()) return "(no restriction)";
        std::string r;
        for (const auto& e : it->second) { if (!r.empty()) r += ", "; r += e; }
        return r;
    }

    std::string statusJson() const {
        std::lock_guard<std::mutex> lk(mutex_);
        std::string j = "{\"allowlists\":[";
        bool first = true;
        for (const auto& p : allowed_) {
            if (!first) j += ",";
            j += "{\"user\":\"" + p.first + "\",\"ips\":[";
            for (size_t i = 0; i < p.second.size(); ++i) {
                if (i) j += ",";
                j += "\"" + p.second[i] + "\"";
            }
            j += "]}";
            first = false;
        }
        return j + "]}";
    }

private:
    mutable std::mutex mutex_;
    std::map<std::string, std::vector<std::string>> allowed_;
    const char* FILE_ = "database.milan.allowlist";

    static std::vector<std::string> parseList(const std::string& s) {
        std::vector<std::string> v;
        std::istringstream ss(s);
        std::string tok;
        while (std::getline(ss, tok, ',')) {
            while (!tok.empty() && (tok.front()==' '||tok.front()=='\''||tok.front()=='"')) tok.erase(tok.begin());
            while (!tok.empty() && (tok.back() ==' '||tok.back() =='\''||tok.back() =='"')) tok.pop_back();
            if (!tok.empty()) v.push_back(tok);
        }
        return v;
    }

    static bool matches(const std::string& ip, const std::string& pattern) {
        if (pattern == ip || pattern == "*") return true;
        auto slash = pattern.find('/');
        if (slash != std::string::npos) {
            std::string net = pattern.substr(0, slash);
            int prefix = std::stoi(pattern.substr(slash+1));
            if (prefix == 24) {
                auto lastDot = net.rfind('.');
                if (lastDot != std::string::npos) {
                    std::string netPrefix = net.substr(0, lastDot+1);
                    if (ip.substr(0, netPrefix.size()) == netPrefix) return true;
                }
            }
        }
        return false;
    }

    void save() const {
        std::ofstream f(FILE_);
        for (const auto& p : allowed_) {
            f << p.first << "\t";
            for (size_t i = 0; i < p.second.size(); ++i) { if (i) f << ","; f << p.second[i]; }
            f << "\n";
        }
    }
};

} // namespace milansql
