#pragma once
// ============================================================
// user_key_manager.hpp — Per-User HMAC Table Name Encryption
// Phase 162: Cryptographic User Isolation
//
// Design:
//   - Per-user key:  HMAC-SHA256(masterKey, "user:" + userId)
//   - Table enc:     "__u_" + first 16 hex chars of
//                    HMAC-SHA256(userKey, tableName)
//   - Keys cached in RAM; unloaded on logout
//
// Usage:
//   UserKeyManager mgr;
//   auto enc = mgr.encryptTableName(42, "orders");
//   // => "__u_a3f8e1c2b4d69012"  (deterministic)
// ============================================================

#include <string>
#include <map>
#include <vector>
#include <mutex>
#include <cstdint>
#include <sstream>
#include <iomanip>
#include "master_key.hpp"
#include "../auth/auth_manager.hpp"   // SHA256Impl, hmacSha256Bytes

namespace milansql {

class UserKeyManager {
public:
    // Derive and cache the key for userId.
    // Idempotent — safe to call multiple times.
    void loadUser(int userId) {
        std::lock_guard<std::mutex> lk(mutex_);
        if (keys_.count(userId)) return;
        keys_[userId] = deriveKey_(userId);
    }

    // Remove user key from RAM (call on logout / session end).
    void unloadUser(int userId) {
        std::lock_guard<std::mutex> lk(mutex_);
        keys_.erase(userId);
    }

    // Encrypt table name → "__u_" + 16 lowercase hex chars.
    // Deterministic: same (userId, tableName) always returns the same result.
    // Auto-derives user key if not yet loaded.
    std::string encryptTableName(int userId, const std::string& tableName) {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = keys_.find(userId);
        if (it == keys_.end()) {
            keys_[userId] = deriveKey_(userId);
            it = keys_.find(userId);
        }

        // HMAC-SHA256(userKey, tableName)
        const std::string keyStr(it->second.begin(), it->second.end());
        auto hmac = hmacSha256Bytes(keyStr, tableName);

        // Take first 8 bytes → 16 hex chars (64 bits of entropy — ample)
        std::ostringstream oss;
        oss << "__u_";
        for (size_t i = 0; i < 8 && i < hmac.size(); ++i)
            oss << std::hex << std::setw(2) << std::setfill('0')
                << static_cast<int>(hmac[i]);
        return oss.str();
    }

    bool isLoaded(int userId) const {
        std::lock_guard<std::mutex> lk(mutex_);
        return keys_.count(userId) > 0;
    }

private:
    // Derive HMAC-SHA256(masterKey, "user:" + userId) → 32 bytes
    static std::vector<uint8_t> deriveKey_(int userId) {
        const auto& masterKey = MasterKey::instance().getKey();
        const std::string masterStr(masterKey.begin(), masterKey.end());
        const std::string salt = "user:" + std::to_string(userId);
        return hmacSha256Bytes(masterStr, salt);
    }

    mutable std::mutex            mutex_;
    std::map<int, std::vector<uint8_t>> keys_;
};

} // namespace milansql
