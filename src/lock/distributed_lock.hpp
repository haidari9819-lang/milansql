#pragma once
#include <string>
#include <map>

namespace milansql {

class DistributedLockManager {
    struct Entry { bool held = true; };
    std::map<std::string, Entry> locks_;

public:
    bool getLock(const std::string& name, int /*timeout*/) {
        if (locks_.count(name) && locks_[name].held) return false;
        locks_[name] = {true};
        return true;
    }

    bool releaseLock(const std::string& name) {
        auto it = locks_.find(name);
        if (it == locks_.end() || !it->second.held) return false;
        locks_.erase(it);
        return true;
    }

    bool isFreeLock(const std::string& name) const {
        auto it = locks_.find(name);
        return it == locks_.end() || !it->second.held;
    }
};

} // namespace milansql
