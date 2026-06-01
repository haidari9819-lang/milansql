#pragma once
// ============================================================
// statement_cache.hpp — MilanSQL Prepared Statement Cache
// Phase 93: LRU eviction, normalized SQL keys
// ============================================================

#include <string>
#include <unordered_map>
#include <list>
#include <optional>
#include <cctype>
#include <sstream>
#include <iomanip>

// Forward declaration — ParsedCommand is defined in parser.hpp,
// which is included by dispatch.hpp before this header is used.

namespace milansql {

// Forward-declare ParsedCommand to avoid circular includes.
// The actual type must be complete when get/put are called.
struct ParsedCommand;

class StatementCache {
public:
    explicit StatementCache(int maxSize = 500)
        : maxSize_(maxSize), enabled_(true) {}

    static std::string normalize(const std::string& sql) {
        std::string r;
        bool inSpace = false;
        for (char c : sql) {
            if (std::isspace(static_cast<unsigned char>(c))) {
                if (!inSpace && !r.empty()) { r += ' '; inSpace = true; }
            } else {
                r += static_cast<char>(
                    std::tolower(static_cast<unsigned char>(c)));
                inSpace = false;
            }
        }
        while (!r.empty() && r.back() == ' ') r.pop_back();
        return r;
    }

    std::optional<ParsedCommand> get(const std::string& sql) {
        if (!enabled_) return std::nullopt;
        std::string key = normalize(sql);
        auto it = cache_.find(key);
        if (it == cache_.end()) { ++misses_; return std::nullopt; }
        // Move to front (most recently used)
        lruList_.erase(it->second.lruIt);
        lruList_.push_front(key);
        it->second.lruIt = lruList_.begin();
        ++hits_;
        return it->second.cmd;
    }

    void put(const std::string& sql, const ParsedCommand& cmd) {
        if (!enabled_) return;
        std::string key = normalize(sql);
        auto it = cache_.find(key);
        if (it != cache_.end()) {
            lruList_.erase(it->second.lruIt);
            lruList_.push_front(key);
            it->second.lruIt = lruList_.begin();
            it->second.cmd   = cmd;
            return;
        }
        if (static_cast<int>(cache_.size()) >= maxSize_) {
            std::string evict = lruList_.back();
            lruList_.pop_back();
            cache_.erase(evict);
        }
        lruList_.push_front(key);
        cache_[key] = {cmd, lruList_.begin()};
    }

    void clear() {
        cache_.clear();
        lruList_.clear();
        hits_   = 0;
        misses_ = 0;
    }

    void setEnabled(bool e) { enabled_ = e; }
    bool isEnabled()  const { return enabled_; }
    void setMaxSize(int n)  { maxSize_ = n; }
    int  size()        const { return static_cast<int>(cache_.size()); }

    std::string showStats() const {
        int total = hits_ + misses_;
        double hitRate = (total > 0) ? (100.0 * hits_ / total) : 0.0;
        std::ostringstream oss;
        oss << "Statement Cache Statistics:\n";
        oss << "  Enabled:  " << (enabled_ ? "ON" : "OFF") << "\n";
        oss << "  Max Size: " << maxSize_ << "\n";
        oss << "  Entries:  " << cache_.size() << "\n";
        oss << "  Hits:     " << hits_ << "\n";
        oss << "  Misses:   " << misses_ << "\n";
        oss << "  Hit Rate: " << std::fixed << std::setprecision(1)
            << hitRate << "%\n";
        return oss.str();
    }

private:
    struct Entry {
        ParsedCommand                     cmd;
        std::list<std::string>::iterator  lruIt;
    };

    std::unordered_map<std::string, Entry> cache_;
    std::list<std::string>                 lruList_;
    int                                    maxSize_;
    bool                                   enabled_;
    mutable int                            hits_   = 0;
    mutable int                            misses_ = 0;
};

} // namespace milansql
