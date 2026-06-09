#pragma once
// ──────────────────────────────────────────────────────────────────────────
// user_key_manager.hpp — Phase 162: Per-user HMAC-SHA256 key management
// Derives a per-user key from the MasterKey and produces deterministic,
// non-reversible table name ciphertext: "__u_" + 16 hex chars
// ──────────────────────────────────────────────────────────────────────────

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "master_key.hpp"
#include "../auth/auth_manager.hpp"  // hmacSha256Bytes, SHA256Impl

namespace milansql {

class UserKeyManager {
public:
    static UserKeyManager& instance() {
        static UserKeyManager inst;
        return inst;
    }

    // Load (derive + cache) a user's key
    void loadUser(int userId) {
        std::lock_guard<std::mutex> lk(mutex_);
        if (!keys_.count(userId))
            keys_[userId] = deriveKey_(userId);
    }

    // Remove a user's key from memory (called on logout)
    void unloadUser(int userId) {
        std::lock_guard<std::mutex> lk(mutex_);
        keys_.erase(userId);
    }

    bool isLoaded(int userId) const {
        std::lock_guard<std::mutex> lk(mutex_);
        return keys_.count(userId) > 0;
    }

    // Deterministic encryption: same (userId, tableName) → same result
    // Returns "__u_" + first 8 bytes of HMAC as 16 hex chars
    std::string encryptTableName(int userId, const std::string& tableName) {
        std::lock_guard<std::mutex> lk(mutex_);
        if (!keys_.count(userId))
            keys_[userId] = deriveKey_(userId);

        const std::string& keyStr = keys_[userId];
        auto hmac = hmacSha256Bytes(keyStr, tableName);

        static const char hex[] = "0123456789abcdef";
        std::string out = "__u_";
        out.reserve(20);
        for (size_t i = 0; i < 8; ++i) {
            out += hex[hmac[i] >> 4];
            out += hex[hmac[i] & 0xf];
        }
        return out;
    }

private:
    mutable std::mutex mutex_;
    std::map<int, std::string> keys_;

    UserKeyManager() = default;
    UserKeyManager(const UserKeyManager&) = delete;
    UserKeyManager& operator=(const UserKeyManager&) = delete;

    // Derive per-user key: HMAC(masterKey, "user:<id>")
    std::string deriveKey_(int userId) {
        const auto& masterBytes = MasterKey::instance().getKey();
        std::string masterStr(masterBytes.begin(), masterBytes.end());
        std::string msg = "user:" + std::to_string(userId);
        auto derived = hmacSha256Bytes(masterStr, msg);
        return std::string(derived.begin(), derived.end());
    }
};

} // namespace milansql
