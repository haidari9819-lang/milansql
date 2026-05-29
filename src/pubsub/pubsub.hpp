#pragma once
// ============================================================
// pubsub.hpp — LISTEN / NOTIFY / UNLISTEN for MilanSQL
// Phase 76: In-process pub/sub channel system
//
// API:
//   PubSub::listen(channel, sessionId)
//   PubSub::unlisten(channel, sessionId)
//   PubSub::unlistenAll(sessionId)
//   PubSub::notify(channel, payload)
//   PubSub::pending(sessionId) -> vector of {channel, payload}
// ============================================================

#include <string>
#include <vector>
#include <map>
#include <set>
#include <mutex>
#include <deque>
#include <utility>

namespace milansql {

struct Notification {
    std::string channel;
    std::string payload;
};

class PubSub {
public:
    // Register sessionId as listener on channel.
    void listen(const std::string& channel, const std::string& sessionId) {
        std::lock_guard<std::mutex> lk(mu_);
        listeners_[channel].insert(sessionId);
    }

    // Remove sessionId from channel. If channel becomes empty, remove it.
    void unlisten(const std::string& channel, const std::string& sessionId) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = listeners_.find(channel);
        if (it == listeners_.end()) return;
        it->second.erase(sessionId);
        if (it->second.empty()) listeners_.erase(it);
    }

    // Remove sessionId from all channels.
    void unlistenAll(const std::string& sessionId) {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto it = listeners_.begin(); it != listeners_.end(); ) {
            it->second.erase(sessionId);
            if (it->second.empty())
                it = listeners_.erase(it);
            else
                ++it;
        }
        inbox_.erase(sessionId);
    }

    // Deliver payload to all listeners on channel.
    // Returns the number of sessions notified.
    size_t notify(const std::string& channel, const std::string& payload = "") {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = listeners_.find(channel);
        if (it == listeners_.end()) return 0;
        for (const auto& sid : it->second)
            inbox_[sid].push_back({channel, payload});
        return it->second.size();
    }

    // Drain and return all pending notifications for sessionId.
    std::vector<Notification> pending(const std::string& sessionId) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = inbox_.find(sessionId);
        if (it == inbox_.end()) return {};
        std::vector<Notification> result(it->second.begin(), it->second.end());
        it->second.clear();
        return result;
    }

    // Returns channels sessionId is currently listening on.
    std::vector<std::string> activeChannels(const std::string& sessionId) const {
        std::lock_guard<std::mutex> lk(mu_);
        std::vector<std::string> result;
        for (const auto& [ch, sids] : listeners_)
            if (sids.count(sessionId)) result.push_back(ch);
        return result;
    }

private:
    mutable std::mutex mu_;
    std::map<std::string, std::set<std::string>>    listeners_; // channel -> sessions
    std::map<std::string, std::deque<Notification>> inbox_;     // session -> messages
};

// Global singleton accessible from dispatch and engine.
inline PubSub& g_pubsub() {
    static PubSub instance;
    return instance;
}

} // namespace milansql
