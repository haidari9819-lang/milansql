#pragma once
// ============================================================
// auth_manager.hpp — MilanSQL Multi-User Auth + Permissions
// Phase 154: JWT, SHA-256, Rate Limiting, User Isolation
// Phase 155: GRANT/REVOKE, Column-Level Security, RLS V2
// Phase 156: API Key V2, Tenant Quotas, Admin Dashboard
// ============================================================

#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <random>
#include <algorithm>
#include <atomic>
#include <cstdlib>
#ifndef _WIN32
#include <sys/stat.h>
#endif
#include "../utils/date_utils.hpp"

// ── SHA-256 (pure C++, NIST FIPS 180-4) ─────────────────────

class SHA256Impl {
public:
    static std::vector<uint8_t> hash(const std::string& input) {
        uint32_t h[8] = {
            0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
            0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
        };
        static const uint32_t K[64] = {
            0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,
            0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
            0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,
            0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
            0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,
            0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
            0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,
            0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
            0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,
            0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
            0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,
            0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
            0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,
            0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
            0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,
            0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
        };

        // Pre-process message
        std::vector<uint8_t> msg(input.begin(), input.end());
        uint64_t bitLen = (uint64_t)msg.size() * 8;
        msg.push_back(0x80);
        while (msg.size() % 64 != 56) msg.push_back(0);
        for (int i = 7; i >= 0; --i) msg.push_back((uint8_t)(bitLen >> (i*8)));

        // Process each 512-bit block
        for (size_t i = 0; i < msg.size(); i += 64) {
            uint32_t W[64];
            for (int t = 0; t < 16; ++t)
                W[t] = ((uint32_t)msg[i+t*4]<<24)|((uint32_t)msg[i+t*4+1]<<16)|
                       ((uint32_t)msg[i+t*4+2]<<8)|(uint32_t)msg[i+t*4+3];
            for (int t = 16; t < 64; ++t)
                W[t] = sig1(W[t-2]) + W[t-7] + sig0(W[t-15]) + W[t-16];

            uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
            for (int t = 0; t < 64; ++t) {
                uint32_t T1 = hh + Sig1(e) + Ch(e,f,g) + K[t] + W[t];
                uint32_t T2 = Sig0(a) + Maj(a,b,c);
                hh=g; g=f; f=e; e=d+T1; d=c; c=b; b=a; a=T1+T2;
            }
            h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d;
            h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
        }

        std::vector<uint8_t> digest(32);
        for (int i = 0; i < 8; ++i) {
            digest[i*4]   = (uint8_t)(h[i]>>24);
            digest[i*4+1] = (uint8_t)(h[i]>>16);
            digest[i*4+2] = (uint8_t)(h[i]>>8);
            digest[i*4+3] = (uint8_t)(h[i]);
        }
        return digest;
    }

    static std::string hexStr(const std::vector<uint8_t>& v) {
        static const char* hex = "0123456789abcdef";
        std::string s; s.reserve(v.size()*2);
        for (uint8_t b : v) { s += hex[b>>4]; s += hex[b&0xf]; }
        return s;
    }

    static std::string hashHex(const std::string& input) {
        return hexStr(hash(input));
    }

private:
    static uint32_t rotr(uint32_t x,int n){return(x>>n)|(x<<(32-n));}
    static uint32_t Ch(uint32_t x,uint32_t y,uint32_t z){return(x&y)^(~x&z);}
    static uint32_t Maj(uint32_t x,uint32_t y,uint32_t z){return(x&y)^(x&z)^(y&z);}
    static uint32_t Sig0(uint32_t x){return rotr(x,2)^rotr(x,13)^rotr(x,22);}
    static uint32_t Sig1(uint32_t x){return rotr(x,6)^rotr(x,11)^rotr(x,25);}
    static uint32_t sig0(uint32_t x){return rotr(x,7)^rotr(x,18)^(x>>3);}
    static uint32_t sig1(uint32_t x){return rotr(x,17)^rotr(x,19)^(x>>10);}
};

// ── HMAC-SHA256 ───────────────────────────────────────────────

static std::vector<uint8_t> hmacSha256Bytes(const std::string& key, const std::string& msg) {
    const size_t BLOCK = 64;
    std::vector<uint8_t> k(key.begin(), key.end());
    if (k.size() > BLOCK) k = SHA256Impl::hash(key);
    k.resize(BLOCK, 0);

    std::vector<uint8_t> ipad(BLOCK), opad(BLOCK);
    for (size_t i = 0; i < BLOCK; ++i) { ipad[i] = k[i]^0x36; opad[i] = k[i]^0x5c; }

    std::string inner(ipad.begin(), ipad.end()); inner += msg;
    std::vector<uint8_t> innerHash = SHA256Impl::hash(inner);
    std::string outer(opad.begin(), opad.end());
    outer += std::string(innerHash.begin(), innerHash.end());
    return SHA256Impl::hash(outer);
}

static std::string hmacSha256Hex(const std::string& key, const std::string& msg) {
    return SHA256Impl::hexStr(hmacSha256Bytes(key, msg));
}

// ── PBKDF2-HMAC-SHA256 (RFC 8018) ────────────────────────────
// Uses existing hmacSha256Bytes. Output: 32 bytes (one SHA-256 block).
static constexpr int PBKDF2_ITERATIONS = 250000;  // tuned for ~300-500ms on production server

static std::vector<uint8_t> pbkdf2HmacSha256(const std::string& password,
                                               const std::string& salt,
                                               int iterations) {
    // For 32-byte output we only need block_index = 1
    // U1 = HMAC(password, salt || INT32_BE(1))
    std::string saltBlock = salt;
    saltBlock += '\0'; saltBlock += '\0'; saltBlock += '\0'; saltBlock += '\1';

    auto U = hmacSha256Bytes(password, saltBlock);
    auto result = U;  // T = U1

    for (int i = 1; i < iterations; ++i) {
        std::string prev(U.begin(), U.end());
        U = hmacSha256Bytes(password, prev);
        for (size_t j = 0; j < 32; ++j) result[j] ^= U[j];
    }
    return result;
}

// Hash a password with PBKDF2. Format: "pbkdf2$<iterations>$<salt_hex>$<hash_hex>"
static std::string hashPasswordPbkdf2(const std::string& password, const std::string& saltHex) {
    // Convert hex salt to raw bytes for PBKDF2 input
    auto dk = pbkdf2HmacSha256(password, saltHex, PBKDF2_ITERATIONS);
    return "pbkdf2$" + std::to_string(PBKDF2_ITERATIONS) + "$" + saltHex + "$" + SHA256Impl::hexStr(dk);
}

// ── Base64URL ─────────────────────────────────────────────────

static std::string base64urlEncode(const std::string& input) {
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int val=0, valb=-6;
    for (uint8_t c : input) {
        val = (val<<8)+c; valb += 8;
        while (valb >= 0) { out += T[(val>>valb)&0x3f]; valb -= 6; }
    }
    if (valb > -6) out += T[((val<<8)>>(valb+8))&0x3f];
    for (auto& c : out) { if(c=='+') c='-'; else if(c=='/') c='_'; }
    // no padding
    return out;
}

static std::string base64urlDecode(const std::string& input) {
    std::string s = input;
    for (auto& c : s) { if(c=='-') c='+'; else if(c=='_') c='/'; }
    while (s.size()%4) s += '=';
    std::string out;
    std::vector<int> T(256,-1);
    static const char* enc = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (int i=0;i<64;i++) T[(uint8_t)enc[i]]=i;
    int val=0, valb=-8;
    for (uint8_t c : s) {
        if (T[c]==-1) break;
        val=(val<<6)+T[c]; valb+=6;
        if (valb>=0) { out+=(char)((val>>valb)&0xff); valb-=8; }
    }
    return out;
}

static std::string base64urlEncodeBytes(const std::vector<uint8_t>& v) {
    return base64urlEncode(std::string(v.begin(), v.end()));
}

// ── Structs ───────────────────────────────────────────────────

struct AuthUser {
    int id = 0;
    std::string username;
    std::string email;
    std::string passwordHash; // "salt:sha256hash"
    std::string role;         // "root" or "user"
    std::string createdAt;
    std::string lastLogin;
    bool isActive = true;
    std::string apiKey;       // primary API key (legacy)
};

struct UserSession {
    std::string token;
    std::string refreshToken;
    int userId = 0;
    std::string username;
    int64_t createdAt = 0;
    int64_t expiresAt = 0;
    int64_t refreshExpiresAt = 0;
    bool revoked = false;
};

// Phase 155: Granular permission
struct Permission {
    int userId = 0;
    std::string tableName;                // e.g. "orders" or "alice.orders"
    std::string privilege;                // "SELECT","INSERT","UPDATE","DELETE","ALL"
    std::vector<std::string> columns;     // empty = all columns
    int grantedBy = 0;
};

// Phase 156: Named API key
struct ApiKeyInfo {
    std::string key;          // "ms_xxx..."
    std::string name;
    int userId = 0;
    int64_t createdAt = 0;
    int64_t expiresAt = 0;    // 0 = no expiry
    std::vector<std::string> permissions; // empty = all
    std::vector<std::string> tables;      // empty = all
    int64_t requestsToday = 0;
    int64_t lastUsed = 0;
    bool revoked = false;
};

// Phase 156: Tenant quota
struct TenantQuota {
    int userId = 0;
    int maxTables = 100;
    int64_t maxRows = 1000000;
    int64_t maxStorageMB = 1024;
};

// ── AuthManager ───────────────────────────────────────────────

class AuthManager {
public:
    struct AuthResult {
        bool ok = false;
        std::string token;
        std::string refresh;
        int userId = 0;
        std::string error;
    };
    struct ValidateResult {
        int userId = 0;
        std::string username;
        std::string role;
        bool valid = false;
    };

    AuthManager() = default;

    void init(const std::string& jwtSecret = "") {
        std::lock_guard<std::mutex> lk(mutex_);
        if (!jwtSecret.empty()) jwtSecret_ = jwtSecret;  // tests / explicit
        else resolveJwtSecret();  // production: env → file → legacy → generate

        // Create root user if none exist
        if (users_.empty()) {
            AuthUser root;
            root.id = nextId_++;
            root.username = "root";
            root.role = "root";
            root.isActive = true;
            root.createdAt = nowStr();
            std::string salt = generateRandom(32);
            root.passwordHash = hashPasswordPbkdf2("root", salt);
            users_[root.id] = root;
            nameIndex_["root"] = root.id;
        }
    }

    // ── Registration & Login ──────────────────────────────────

    AuthResult registerUser(const std::string& username, const std::string& password,
                            const std::string& email = "") {
        std::lock_guard<std::mutex> lk(mutex_);
        if (username.empty() || password.empty())
            return {false,"","",0,"username and password required"};
        if (nameIndex_.count(username))
            return {false,"","",0,"Username already taken"};

        AuthUser u;
        u.id = nextId_++;
        u.username = username;
        u.email = email;
        u.role = "user";
        u.isActive = true;
        u.createdAt = nowStr();
        std::string salt = generateRandom(32);
        u.passwordHash = hashPasswordPbkdf2(password, salt);
        users_[u.id] = u;
        nameIndex_[username] = u.id;

        auto [tok, ref] = makeTokens(u.id, u.username, u.role);
        return {true, tok, ref, u.id, ""};
    }

    AuthResult login(const std::string& username, const std::string& password) {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = nameIndex_.find(username);
        if (it == nameIndex_.end()) return {false,"","",0,"Invalid username or password"};
        auto& u = users_[it->second];
        if (!u.isActive) return {false,"","",0,"Account disabled"};
        auto [ok, needsMigration] = checkPasswordEx(password, u.passwordHash);
        if (!ok) return {false,"","",0,"Invalid username or password"};
        // Transparent migration: rehash legacy SHA-256 to PBKDF2
        if (needsMigration) {
            std::string salt = generateRandom(32);
            u.passwordHash = hashPasswordPbkdf2(password, salt);
        }
        u.lastLogin = nowStr();
        auto [tok, ref] = makeTokens(u.id, u.username, u.role);
        return {true, tok, ref, u.id, ""};
    }

    // ── Change Password ────────────────────────────────────────
    struct ChangePasswordResult { bool ok = false; std::string error; };

    ChangePasswordResult changePassword(int userId, const std::string& oldPassword,
                                        const std::string& newPassword) {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = users_.find(userId);
        if (it == users_.end()) return {false, "User not found"};
        auto& u = it->second;
        auto [match, _] = checkPasswordEx(oldPassword, u.passwordHash);
        if (!match) return {false, "Current password incorrect"};
        if (newPassword.size() < 8) return {false, "New password must be at least 8 characters"};
        std::string salt = generateRandom(32);
        u.passwordHash = hashPasswordPbkdf2(newPassword, salt);
        return {true, ""};
    }

    // Admin: force-set password without knowing old one
    ChangePasswordResult adminSetPassword(int requesterId, int targetUserId,
                                          const std::string& newPassword) {
        std::lock_guard<std::mutex> lk(mutex_);
        auto reqIt = users_.find(requesterId);
        if (reqIt == users_.end() || reqIt->second.role != "root")
            return {false, "Admin privileges required"};
        auto it = users_.find(targetUserId);
        if (it == users_.end()) return {false, "Target user not found"};
        if (newPassword.size() < 8) return {false, "New password must be at least 8 characters"};
        std::string salt = generateRandom(32);
        it->second.passwordHash = hashPasswordPbkdf2(newPassword, salt);
        return {true, ""};
    }

    int getUserIdByName(const std::string& username) {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = nameIndex_.find(username);
        return (it != nameIndex_.end()) ? it->second : -1;
    }

    // Set a user's role (root, user, service, etc.)
    bool setUserRole(int userId, const std::string& role) {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = users_.find(userId);
        if (it == users_.end()) return false;
        it->second.role = role;
        return true;
    }

    bool logout(const std::string& token) {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = sessions_.find(token);
        if (it != sessions_.end()) { it->second.revoked = true; return true; }
        return false;
    }

    ValidateResult validateToken(const std::string& token) {
        if (token.empty()) return {};
        // Session MUST exist and not be revoked (deny-by-default)
        {
            std::lock_guard<std::mutex> lk(mutex_);
            auto it = sessions_.find(token);
            if (it == sessions_.end()) return {};        // unknown token → reject
            if (it->second.revoked) return {};           // revoked → reject
            if (it->second.expiresAt < nowSeconds()) return {};  // expired → reject
        }
        return decodeJWT(token);
    }

    ValidateResult validateApiKey(const std::string& key) {
        std::lock_guard<std::mutex> lk(mutex_);
        // Check named API keys (Phase 156)
        auto it = namedKeys_.find(key);
        if (it != namedKeys_.end()) {
            auto& ki = it->second;
            if (ki.revoked) return {};
            if (ki.expiresAt > 0 && ki.expiresAt < nowSeconds()) return {};
            ki.lastUsed = nowSeconds();
            ki.requestsToday++;
            auto uit = users_.find(ki.userId);
            if (uit == users_.end() || !uit->second.isActive) return {};
            return {ki.userId, uit->second.username, uit->second.role, true};
        }
        // Legacy single API key
        auto kit = apiKeyIndex_.find(key);
        if (kit == apiKeyIndex_.end()) return {};
        auto uit = users_.find(kit->second);
        if (uit == users_.end() || !uit->second.isActive) return {};
        return {uit->second.id, uit->second.username, uit->second.role, true};
    }

    AuthResult refreshToken(const std::string& refreshTok) {
        std::lock_guard<std::mutex> lk(mutex_);
        // Find session with this refresh token
        for (auto& [tok, sess] : sessions_) {
            if (sess.refreshToken == refreshTok && !sess.revoked) {
                if (sess.refreshExpiresAt < nowSeconds()) return {false,"","",0,"Refresh token expired"};
                sess.revoked = true; // invalidate old session
                auto uit = users_.find(sess.userId);
                if (uit == users_.end()) return {false,"","",0,"User not found"};
                auto [newTok, newRef] = makeTokens(sess.userId, sess.username, uit->second.role);
                return {true, newTok, newRef, sess.userId, ""};
            }
        }
        return {false,"","",0,"Invalid refresh token"};
    }

    // ── API Key Management (Phase 156) ────────────────────────

    std::string generateApiKey(int userId) {
        std::lock_guard<std::mutex> lk(mutex_);
        std::string key = "ms_" + generateRandom(32);
        auto& u = users_[userId];
        u.apiKey = key;
        apiKeyIndex_[key] = userId;
        return key;
    }

    // Named API key with optional expiry and permissions
    std::string createNamedApiKey(int userId, const std::string& name,
                                   int expiresInDays = 0,
                                   const std::vector<std::string>& permissions = {},
                                   const std::vector<std::string>& tables = {}) {
        std::lock_guard<std::mutex> lk(mutex_);
        ApiKeyInfo ki;
        ki.key = "ms_" + generateRandom(32);
        ki.name = name;
        ki.userId = userId;
        ki.createdAt = nowSeconds();
        ki.expiresAt = (expiresInDays > 0) ? ki.createdAt + (int64_t)expiresInDays*86400 : 0;
        ki.permissions = permissions;
        ki.tables = tables;
        namedKeys_[ki.key] = ki;
        return ki.key;
    }

    std::vector<ApiKeyInfo> listApiKeys(int userId) const {
        std::lock_guard<std::mutex> lk(mutex_);
        std::vector<ApiKeyInfo> result;
        for (const auto& [k, ki] : namedKeys_)
            if (ki.userId == userId && !ki.revoked) result.push_back(ki);
        return result;
    }

    bool revokeApiKey(const std::string& key) {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = namedKeys_.find(key);
        if (it != namedKeys_.end()) { it->second.revoked = true; return true; }
        auto it2 = apiKeyIndex_.find(key);
        if (it2 != apiKeyIndex_.end()) { apiKeyIndex_.erase(it2); return true; }
        return false;
    }

    ApiKeyInfo* getApiKeyInfo(const std::string& key) {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = namedKeys_.find(key);
        if (it != namedKeys_.end()) return &it->second;
        return nullptr;
    }

    // ── Phase 155: GRANT / REVOKE ─────────────────────────────

    void grantPermission(const Permission& perm) {
        std::lock_guard<std::mutex> lk(mutex_);
        // Remove existing grant for same user+table+privilege first
        permissions_.erase(std::remove_if(permissions_.begin(), permissions_.end(),
            [&](const Permission& p){
                return p.userId == perm.userId && p.tableName == perm.tableName &&
                       (p.privilege == perm.privilege || perm.privilege == "ALL" || p.privilege == "ALL");
            }), permissions_.end());
        permissions_.push_back(perm);
    }

    void revokePermission(int userId, const std::string& tableName, const std::string& privilege) {
        std::lock_guard<std::mutex> lk(mutex_);
        permissions_.erase(std::remove_if(permissions_.begin(), permissions_.end(),
            [&](const Permission& p){
                return p.userId == userId && p.tableName == tableName &&
                       (p.privilege == privilege || privilege == "ALL" || p.privilege == "ALL");
            }), permissions_.end());
    }

    // Check if userId has privilege on tableName
    bool hasPermission(int userId, const std::string& tableName, const std::string& privilege) const {
        std::lock_guard<std::mutex> lk(mutex_);
        // Root has everything
        auto uit = users_.find(userId);
        if (uit != users_.end() && uit->second.role == "root") return true;
        // Anonymous users (userId <= 0) have no implicit permissions
        if (userId <= 0) return false;

        for (const auto& p : permissions_) {
            if (p.userId != userId) continue;
            // Match table name (strip user prefix for comparison)
            std::string pTable = p.tableName;
            if (pTable == tableName || pTable == "*") {
                if (p.privilege == "ALL" || p.privilege == privilege) return true;
            }
        }
        return false;
    }

    // Get allowed columns for userId on tableName (empty = all)
    std::vector<std::string> getAllowedColumns(int userId, const std::string& tableName) const {
        std::lock_guard<std::mutex> lk(mutex_);
        auto uit = users_.find(userId);
        if (uit != users_.end() && uit->second.role == "root") return {};
        if (userId <= 0) return {};

        for (const auto& p : permissions_) {
            if (p.userId == userId && (p.tableName == tableName || p.tableName == "*")) {
                if (!p.columns.empty()) return p.columns;
                return {}; // all columns
            }
        }
        return {};
    }

    std::string showGrantsFor(const std::string& username) const {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = nameIndex_.find(username);
        std::ostringstream oss;
        oss << "privilege | table | columns\n";
        oss << "----------+-----------+--------\n";
        if (it == nameIndex_.end()) { oss << "(user not found)\n"; return oss.str(); }
        int uid = it->second;
        for (const auto& p : permissions_) {
            if (p.userId != uid) continue;
            oss << p.privilege << " | " << p.tableName << " | ";
            if (p.columns.empty()) oss << "(all)";
            else { for (size_t i=0;i<p.columns.size();i++){if(i)oss<<",";oss<<p.columns[i];} }
            oss << "\n";
        }
        return oss.str();
    }

    // ── Phase 156: Tenant Quota ───────────────────────────────

    TenantQuota getQuota(int userId) const {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = quotas_.find(userId);
        if (it != quotas_.end()) return it->second;
        TenantQuota q; q.userId = userId; return q;
    }

    void setQuota(const TenantQuota& q) {
        std::lock_guard<std::mutex> lk(mutex_);
        quotas_[q.userId] = q;
    }

    // ── SHOW output helpers ───────────────────────────────────

    std::string showUsers() const {
        std::lock_guard<std::mutex> lk(mutex_);
        std::ostringstream oss;
        oss << "id | username | email | role | created_at | is_active\n";
        oss << "---+----------+-------+------+------------+----------\n";
        for (const auto& [id, u] : users_) {
            oss << u.id << " | " << u.username << " | " << u.email
                << " | " << u.role << " | " << u.createdAt
                << " | " << (u.isActive ? "1" : "0") << "\n";
        }
        return oss.str();
    }

    std::string showSessions() const {
        std::lock_guard<std::mutex> lk(mutex_);
        std::ostringstream oss;
        oss << "user_id | username | created_at | expires_at | revoked\n";
        oss << "--------+----------+------------+------------+--------\n";
        int64_t now = nowSeconds();
        for (const auto& [tok, s] : sessions_) {
            if (s.revoked || s.expiresAt < now) continue;
            oss << s.userId << " | " << s.username << " | "
                << s.createdAt << " | " << s.expiresAt << " | 0\n";
        }
        return oss.str();
    }

    bool revokeSession(const std::string& token) {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = sessions_.find(token);
        if (it != sessions_.end()) { it->second.revoked = true; return true; }
        return false;
    }

    // ── Admin stats (Phase 156) ───────────────────────────────

    std::string showAllUsers() const {
        std::lock_guard<std::mutex> lk(mutex_);
        std::ostringstream oss;
        oss << "id | username | role | tables | last_login | is_active\n";
        oss << "---+----------+------+--------+------------+----------\n";
        for (const auto& [id, u] : users_) {
            oss << u.id << " | " << u.username << " | " << u.role
                << " | 0 | " << u.lastLogin << " | " << (u.isActive?"1":"0") << "\n";
        }
        return oss.str();
    }

    // ── Persistence ───────────────────────────────────────────

    void save(const std::string& path) const {
        // Security: ensure auth file is owner-only readable
        std::lock_guard<std::mutex> lk(mutex_);
        std::ofstream f(path);
        if (!f) return;
        f << "# MilanSQL Auth v3\n";
        // JWT secret no longer stored here — see /etc/milansql/jwt.secret
        f << "[users]\n";
        for (const auto& [id, u] : users_) {
            f << u.id << "\t" << u.username << "\t" << u.email << "\t"
              << u.passwordHash << "\t" << u.role << "\t" << u.createdAt
              << "\t" << (u.isActive?1:0) << "\t" << u.apiKey << "\t" << u.lastLogin << "\n";
        }
        f << "[permissions]\n";
        for (const auto& p : permissions_) {
            f << p.userId << "\t" << p.tableName << "\t" << p.privilege << "\t";
            for (size_t i=0;i<p.columns.size();i++){if(i)f<<",";f<<p.columns[i];}
            f << "\t" << p.grantedBy << "\n";
        }
        f << "[apikeys]\n";
        for (const auto& [k, ki] : namedKeys_) {
            f << ki.key << "\t" << ki.name << "\t" << ki.userId << "\t"
              << ki.createdAt << "\t" << ki.expiresAt << "\t"
              << (ki.revoked?1:0) << "\t";
            for (size_t i=0;i<ki.permissions.size();i++){if(i)f<<",";f<<ki.permissions[i];}
            f << "\t";
            for (size_t i=0;i<ki.tables.size();i++){if(i)f<<",";f<<ki.tables[i];}
            f << "\n";
        }
        // Persist active sessions (skip expired/revoked to keep file small)
        f << "[sessions]\n";
        int64_t now = nowSeconds();
        for (const auto& [tok, s] : sessions_) {
            if (s.revoked || s.expiresAt < now) continue;
            f << tok << "\t" << s.refreshToken << "\t" << s.userId << "\t"
              << s.username << "\t" << s.createdAt << "\t" << s.expiresAt << "\t"
              << s.refreshExpiresAt << "\t" << (s.revoked?1:0) << "\n";
        }
#ifndef _WIN32
        chmod(path.c_str(), 0600);  // Security: owner-only readable
#endif
    }

    void load(const std::string& path) {
        std::lock_guard<std::mutex> lk(mutex_);
        std::ifstream f(path);
        if (!f) return;
        std::string line, section;
        while (std::getline(f, line)) {
            if (line.empty() || line[0]=='#') continue;
            if (line[0]=='[') { section=line; continue; }
            if (section=="[secret]") { legacySecret_=line; continue; }  // migration path
            if (section=="[users]") {
                auto parts = splitTab(line);
                if (parts.size() < 7) continue;
                AuthUser u;
                u.id = std::stoi(parts[0]);
                u.username = parts[1]; u.email = parts[2];
                u.passwordHash = parts[3]; u.role = parts[4];
                u.createdAt = parts[5]; u.isActive = (parts[6]=="1");
                if (parts.size()>7) u.apiKey = parts[7];
                if (parts.size()>8) u.lastLogin = parts[8];
                users_[u.id] = u;
                nameIndex_[u.username] = u.id;
                if (!u.apiKey.empty()) apiKeyIndex_[u.apiKey] = u.id;
                if (u.id >= nextId_) nextId_ = u.id + 1;
            }
            if (section=="[permissions]") {
                auto parts = splitTab(line);
                if (parts.size() < 4) continue;
                Permission p;
                p.userId = std::stoi(parts[0]);
                p.tableName = parts[1]; p.privilege = parts[2];
                if (!parts[3].empty()) {
                    std::istringstream cs(parts[3]);
                    std::string col;
                    while (std::getline(cs, col, ',')) if (!col.empty()) p.columns.push_back(col);
                }
                if (parts.size()>4) p.grantedBy = std::stoi(parts[4]);
                permissions_.push_back(p);
            }
            if (section=="[apikeys]") {
                auto parts = splitTab(line);
                if (parts.size() < 6) continue;
                ApiKeyInfo ki;
                ki.key = parts[0]; ki.name = parts[1];
                ki.userId = std::stoi(parts[2]);
                ki.createdAt = std::stoll(parts[3]);
                ki.expiresAt = std::stoll(parts[4]);
                ki.revoked = (parts[5]=="1");
                if (parts.size()>6 && !parts[6].empty()) {
                    std::istringstream ps(parts[6]);
                    std::string perm;
                    while (std::getline(ps, perm, ',')) if (!perm.empty()) ki.permissions.push_back(perm);
                }
                if (parts.size()>7 && !parts[7].empty()) {
                    std::istringstream ts(parts[7]);
                    std::string tbl;
                    while (std::getline(ts, tbl, ',')) if (!tbl.empty()) ki.tables.push_back(tbl);
                }
                namedKeys_[ki.key] = ki;
            }
            if (section=="[sessions]") {
                auto parts = splitTab(line);
                if (parts.size() < 8) continue;
                UserSession s;
                s.token = parts[0];
                s.refreshToken = parts[1];
                s.userId = std::stoi(parts[2]);
                s.username = parts[3];
                s.createdAt = std::stoll(parts[4]);
                s.expiresAt = std::stoll(parts[5]);
                s.refreshExpiresAt = std::stoll(parts[6]);
                s.revoked = (parts[7]=="1");
                // Skip expired sessions on load
                if (!s.revoked && s.expiresAt > nowSeconds())
                    sessions_[s.token] = s;
            }
        }
    }

    // ── Getters ───────────────────────────────────────────────

    int getUserCount() const {
        std::lock_guard<std::mutex> lk(mutex_);
        return (int)users_.size();
    }

    const AuthUser* getUser(const std::string& username) const {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = nameIndex_.find(username);
        if (it == nameIndex_.end()) return nullptr;
        auto uit = users_.find(it->second);
        return (uit != users_.end()) ? &uit->second : nullptr;
    }

    const AuthUser* getUserById(int id) const {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = users_.find(id);
        return (it != users_.end()) ? &it->second : nullptr;
    }

    // Phase 175: CLS - access all permissions
    const std::vector<Permission>& getPermissions() const {
        std::lock_guard<std::mutex> lk(mutex_);
        return permissions_;
    }

    // Public wrappers for testing
    static std::string sha256Hex_pub(const std::string& s) { return SHA256Impl::hashHex(s); }
    static std::pair<bool,bool> checkPasswordExPublic(const std::string& pw, const std::string& h) {
        return checkPasswordEx(pw, h);
    }
    static std::string base64urlEncode_pub(const std::string& s) { return base64urlEncode(s); }
    static std::string base64urlDecode_pub(const std::string& s) { return base64urlDecode(s); }

    // ── JWT ───────────────────────────────────────────────────

    // Escape string for safe JSON embedding (prevent injection)
    static std::string jsonEscape(const std::string& s) {
        std::string out;
        out.reserve(s.size());
        for (char c : s) {
            switch (c) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n";  break;
                case '\r': out += "\\r";  break;
                case '\t': out += "\\t";  break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) {
                        char buf[8]; std::snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
                        out += buf;
                    } else out += c;
            }
        }
        return out;
    }

    std::string encodeJWT(int userId, const std::string& username, const std::string& role) {
        std::string hdr = base64urlEncode("{\"alg\":\"HS256\",\"typ\":\"JWT\"}");
        std::string nonce = generateRandom(8);  // unique per token
        std::string pay = base64urlEncode(
            "{\"user_id\":" + std::to_string(userId) +
            ",\"username\":\"" + jsonEscape(username) + "\",\"role\":\"" + jsonEscape(role) +
            "\",\"exp\":" + std::to_string(nowSeconds() + 86400) +
            ",\"jti\":\"" + nonce + "\"}");
        std::string data = hdr + "." + pay;
        std::string sig = base64urlEncodeBytes(hmacSha256Bytes(jwtSecret_, data));
        std::string token = data + "." + sig;
        // Store session for revocation support
        UserSession sess;
        sess.token = token;
        sess.refreshToken = base64urlEncode(generateRandom(24) + std::to_string(userId));
        sess.userId = userId;
        sess.username = username;
        sess.createdAt = nowSeconds();
        sess.expiresAt = sess.createdAt + 86400;
        sess.refreshExpiresAt = sess.createdAt + 2592000;
        sessions_[token] = sess;
        return token;
    }

    ValidateResult decodeJWT(const std::string& token) {
        // Split into 3 parts
        size_t dot1 = token.find('.');
        if (dot1 == std::string::npos) return {};
        size_t dot2 = token.find('.', dot1+1);
        if (dot2 == std::string::npos) return {};

        std::string data = token.substr(0, dot2);
        std::string sig  = token.substr(dot2+1);

        // Verify signature (constant-time comparison to prevent timing attacks)
        std::string expected = base64urlEncodeBytes(hmacSha256Bytes(jwtSecret_, data));
        if (sig.size() != expected.size()) return {};
        volatile uint8_t diff = 0;
        for (size_t i = 0; i < sig.size(); ++i) diff |= sig[i] ^ expected[i];
        if (diff != 0) return {};

        // Decode payload
        std::string payload = base64urlDecode(token.substr(dot1+1, dot2-dot1-1));

        // Extract fields
        auto extractInt = [&](const std::string& key) -> int64_t {
            std::string k = "\"" + key + "\":";
            auto pos = payload.find(k);
            if (pos == std::string::npos) return 0;
            pos += k.size();
            auto end = payload.find_first_of(",}", pos);
            try { return std::stoll(payload.substr(pos, end-pos)); } catch(...) { return 0; }
        };
        auto extractStr = [&](const std::string& key) -> std::string {
            std::string k = "\"" + key + "\":\"";
            auto pos = payload.find(k);
            if (pos == std::string::npos) return "";
            pos += k.size();
            auto end = payload.find('"', pos);
            if (end == std::string::npos) return "";
            return payload.substr(pos, end-pos);
        };

        int64_t exp = extractInt("exp");
        if (exp < nowSeconds()) return {};  // expired

        int uid = (int)extractInt("user_id");
        std::string uname = extractStr("username");
        std::string role  = extractStr("role");

        return {uid, uname, role, true};
    }

private:
    std::map<int, AuthUser> users_;
    std::map<std::string, int> nameIndex_;
    std::map<std::string, UserSession> sessions_;
    std::map<std::string, int> apiKeyIndex_;
    std::map<std::string, ApiKeyInfo> namedKeys_;
    std::vector<Permission> permissions_;
    std::map<int, TenantQuota> quotas_;
    std::string jwtSecret_;
    std::string legacySecret_;   // read from [secret] in auth file during migration
    std::string jwtSecretPath_;  // resolved path to jwt.secret file
    int nextId_ = 1;
    mutable std::mutex mutex_;

    static int64_t nowSeconds() {
        return (int64_t)std::time(nullptr);
    }
    static std::string nowStr() {
        time_t t = (time_t)nowSeconds();
        char buf[32]; std::tm tm = milansql::safe_gmtime(&t);
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
        return buf;
    }
    static std::string generateRandom(size_t bytes) {
        static const char* hex = "0123456789abcdef";
        // Use CSPRNG — std::random_device directly for each byte
        std::random_device rd;
        std::string out; out.reserve(bytes*2);
        for (size_t i=0;i<bytes;++i) {
            uint8_t b = (uint8_t)(rd() & 0xff);
            out += hex[b>>4]; out += hex[b&0xf];
        }
        return out;
    }
    // ── JWT Secret Resolution ─────────────────────────────────
    // Priority: 1) env MILANSQL_JWT_SECRET  2) file jwt.secret  3) legacy from auth-file  4) generate
    void resolveJwtSecret() {
        // 1. Environment variable
        const char* envSecret = std::getenv("MILANSQL_JWT_SECRET");
        if (envSecret && std::string(envSecret).size() > 0) {
            jwtSecret_ = envSecret;
            return;
        }

        // Determine secret file path
#ifdef _WIN32
        jwtSecretPath_ = "jwt.secret";  // local fallback on Windows
#else
        jwtSecretPath_ = "/etc/milansql/jwt.secret";
#endif

        // 2. Read from secret file
        {
            std::ifstream f(jwtSecretPath_);
            if (f) {
                std::string s;
                if (std::getline(f, s) && !s.empty()) {
                    jwtSecret_ = s;
                    return;
                }
            }
        }

        // 3. Migrate legacy secret from auth-file
        if (!legacySecret_.empty()) {
            jwtSecret_ = legacySecret_;
            writeJwtSecretFile(jwtSecret_);
            return;
        }

        // 4. Generate new secret
        jwtSecret_ = generateRandom(32);
        writeJwtSecretFile(jwtSecret_);
    }

    void writeJwtSecretFile(const std::string& secret) {
        if (jwtSecretPath_.empty()) return;
#ifndef _WIN32
        // Ensure directory exists (safe — no std::system())
        ::mkdir("/etc/milansql", 0700);
#endif
        std::ofstream f(jwtSecretPath_);
        if (f) {
            f << secret << "\n";
            f.close();
#ifndef _WIN32
            // chmod 600 — owner read/write only (safe — no std::system())
            ::chmod(jwtSecretPath_.c_str(), 0600);
#endif
        }
    }

    // Returns {matches, needsMigration}
    static std::pair<bool,bool> checkPasswordEx(const std::string& password,
                                                 const std::string& storedHash) {
        if (storedHash.substr(0, 7) == "pbkdf2$") {
            // PBKDF2 format: "pbkdf2$<iterations>$<salt>$<hash>"
            size_t d1 = storedHash.find('$');         // after "pbkdf2"
            size_t d2 = storedHash.find('$', d1+1);   // after iterations
            size_t d3 = storedHash.find('$', d2+1);   // after salt
            if (d2 == std::string::npos || d3 == std::string::npos)
                return {false, false};
            int iters = std::stoi(storedHash.substr(d1+1, d2-d1-1));
            std::string salt = storedHash.substr(d2+1, d3-d2-1);
            std::string expected = storedHash.substr(d3+1);
            auto dk = pbkdf2HmacSha256(password, salt, iters);
            // Security: constant-time comparison to prevent timing attacks
            std::string computed = SHA256Impl::hexStr(dk);
            bool matches = (computed.size() == expected.size());
            volatile unsigned char diff = 0;
            for (size_t i = 0; i < computed.size() && i < expected.size(); ++i)
                diff |= static_cast<unsigned char>(computed[i]) ^ static_cast<unsigned char>(expected[i]);
            matches = matches && (diff == 0);
            // Migrate if stored iterations differ from current target
            return {matches, matches && iters != PBKDF2_ITERATIONS};
        }
        // Legacy format: "salt:sha256hash"
        size_t colon = storedHash.find(':');
        if (colon == std::string::npos) return {false, false};
        std::string salt = storedHash.substr(0, colon);
        std::string expected = storedHash.substr(colon+1);
        std::string computed = SHA256Impl::hashHex(salt + password);
        bool ok = (computed.size() == expected.size());
        volatile unsigned char diff2 = 0;
        for (size_t i = 0; i < computed.size() && i < expected.size(); ++i)
            diff2 |= static_cast<unsigned char>(computed[i]) ^ static_cast<unsigned char>(expected[i]);
        ok = ok && (diff2 == 0);
        return {ok, ok};  // if matched, needs migration to PBKDF2
    }
    // Legacy compat wrapper
    static bool checkPassword(const std::string& password, const std::string& storedHash) {
        return checkPasswordEx(password, storedHash).first;
    }
    std::pair<std::string,std::string> makeTokens(int uid, const std::string& uname, const std::string& role) {
        std::string tok = encodeJWT(uid, uname, role);
        // encodeJWT stores session, grab the refresh token
        auto it = sessions_.find(tok);
        std::string ref = (it != sessions_.end()) ? it->second.refreshToken : "";
        return {tok, ref};
    }
    static std::vector<std::string> splitTab(const std::string& s) {
        std::vector<std::string> parts;
        std::istringstream ss(s);
        std::string tok;
        while (std::getline(ss, tok, '\t')) parts.push_back(tok);
        return parts;
    }
};
