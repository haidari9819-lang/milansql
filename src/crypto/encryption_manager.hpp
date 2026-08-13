#pragma once
// encryption_manager.hpp — Phase 5.1: Encryption at Rest (AES-256-GCM via OpenSSL)
#include <string>
#include <vector>
#include <fstream>
#include <cstdint>
#include <mutex>
#include <cstring>

// OpenSSL EVP (AES-256-GCM)
#ifdef __linux__
#  include <openssl/evp.h>
#  include <openssl/rand.h>
#  define MILANSQL_HAS_OPENSSL 1
#else
#  define MILANSQL_HAS_OPENSSL 0
#endif

namespace milansql {

class EncryptionManager {
public:
    static EncryptionManager& instance() {
        static EncryptionManager em;
        return em;
    }

    EncryptionManager() { tryLoadKeyfile(); }  // Auto-load key on startup

    bool enabled() const { return enabled_; }
    std::string status() const {
        if (!enabled_) return "disabled";
        return "enabled (AES-256-GCM, key: " + keyPreview() + ")";
    }

    std::string enable(const std::string& hexKey) {
        std::lock_guard<std::mutex> lk(mutex_);
        if (hexKey == "auto") {
            // Generate a random 32-byte key
            key_.resize(32);
#if MILANSQL_HAS_OPENSSL
            RAND_bytes(key_.data(), 32);
#else
            for (int i = 0; i < 32; ++i) key_[i] = (uint8_t)(rand() & 0xff);
#endif
        } else {
            if (hexKey.size() < 32) return "ERROR: Key too short (need >=32 hex chars)";
            key_ = hexToBin(hexKey);
            if (key_.empty()) return "ERROR: Invalid hex key";
            if (key_.size() < 32) key_.resize(32, 0);
        }
        enabled_ = true;
        saveKeyfile();
        return "Encryption enabled (AES-256-GCM). Key fingerprint: " + keyPreview();
    }

    std::string disable() {
        std::lock_guard<std::mutex> lk(mutex_);
        enabled_ = false;
        key_.clear();
        removeKeyfile();
        return "Encryption disabled.";
    }

    std::string rotateKey(const std::string& newHexKey) {
        std::lock_guard<std::mutex> lk(mutex_);
        if (newHexKey.size() < 32) return "ERROR: New key too short";
        auto newKey = hexToBin(newHexKey);
        if (newKey.empty()) return "ERROR: Invalid hex key";
        if (newKey.size() < 32) newKey.resize(32, 0);
        key_ = newKey;
        saveKeyfile();
        return "Key rotated. New fingerprint: " + keyPreview();
    }

    // AES-256-GCM encrypt. Returns: [4-byte magic][12-byte IV][16-byte tag][ciphertext]
    // Falls back to XOR if OpenSSL not available.
    std::string encryptStr(const std::string& plain) const {
        if (!enabled_ || key_.empty()) return plain;
#if MILANSQL_HAS_OPENSSL
        // Generate random 12-byte IV
        unsigned char iv[12];
        RAND_bytes(iv, 12);

        std::string cipher(plain.size(), '\0');
        unsigned char tag[16];

        EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
        if (!ctx) return plain; // fallback
        int len = 0, cipherLen = 0;
        EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr);
        EVP_EncryptInit_ex(ctx, nullptr, nullptr, key_.data(), iv);
        EVP_EncryptUpdate(ctx, (unsigned char*)cipher.data(), &len,
                          (const unsigned char*)plain.data(), (int)plain.size());
        cipherLen = len;
        EVP_EncryptFinal_ex(ctx, (unsigned char*)cipher.data() + len, &len);
        cipherLen += len;
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag);
        EVP_CIPHER_CTX_free(ctx);
        cipher.resize(cipherLen);

        // Format: magic(4) + iv(12) + tag(16) + ciphertext
        std::string out;
        out.reserve(4 + 12 + 16 + cipherLen);
        out.append("MAES"); // magic
        out.append((char*)iv, 12);
        out.append((char*)tag, 16);
        out.append(cipher);
        return out;
#else
        // XOR fallback (no OpenSSL)
        std::string out(plain.size(), 0);
        for (size_t i = 0; i < plain.size(); ++i)
            out[i] = plain[i] ^ key_[i % key_.size()];
        return out;
#endif
    }

    std::string decryptStr(const std::string& enc) const {
        if (!enabled_ || key_.empty()) return enc;
#if MILANSQL_HAS_OPENSSL
        // Check magic
        if (enc.size() < 4 + 12 + 16 + 1 || enc.substr(0,4) != "MAES") {
            // Try XOR fallback for old data
            std::string out(enc.size(), 0);
            for (size_t i = 0; i < enc.size(); ++i)
                out[i] = enc[i] ^ key_[i % key_.size()];
            return out;
        }
        const unsigned char* iv  = (const unsigned char*)enc.data() + 4;
        const unsigned char* tag = (const unsigned char*)enc.data() + 4 + 12;
        const char* cipherData   = enc.data() + 4 + 12 + 16;
        int cipherLen = (int)enc.size() - 4 - 12 - 16;
        if (cipherLen < 0) return enc;

        std::string plain(cipherLen, '\0');
        EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
        if (!ctx) return enc;
        int len = 0, plainLen = 0;
        EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr);
        EVP_DecryptInit_ex(ctx, nullptr, nullptr, key_.data(), iv);
        EVP_DecryptUpdate(ctx, (unsigned char*)plain.data(), &len,
                          (const unsigned char*)cipherData, cipherLen);
        plainLen = len;
        // Set expected tag
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, (void*)tag);
        int ret = EVP_DecryptFinal_ex(ctx, (unsigned char*)plain.data() + len, &len);
        EVP_CIPHER_CTX_free(ctx);
        if (ret <= 0) return enc; // tag mismatch — return raw (data corruption)
        plainLen += len;
        plain.resize(plainLen);
        return plain;
#else
        // XOR fallback
        std::string out(enc.size(), 0);
        for (size_t i = 0; i < enc.size(); ++i)
            out[i] = enc[i] ^ key_[i % key_.size()];
        return out;
#endif
    }

    // Load key from keyfile on startup
    void tryLoadKeyfile() {
        std::ifstream f(keyfilePath_);
        if (!f.is_open()) return;
        std::string hex; f >> hex;
        if (hex.size() >= 32) {
            key_ = hexToBin(hex);
            if (key_.size() >= 32) { enabled_ = true; }
        }
    }

private:
    bool enabled_ = false;
    std::vector<uint8_t> key_;
    mutable std::mutex mutex_;
    std::string keyfilePath_ = "database.milan.keyfile";  // Same dir as database

    std::string keyPreview() const {
        if (key_.empty()) return "(none)";
        char buf[9]; std::snprintf(buf, sizeof(buf), "%02x%02x%02x%02x",
            key_[0], key_[1], key_[2], key_[3]);
        return std::string(buf) + "...";
    }

    void saveKeyfile() const {
        std::ofstream f(keyfilePath_);
        if (!f.is_open()) return;
        for (auto b : key_) { char buf[3]; std::snprintf(buf,3,"%02x",b); f << buf; }
        f << "\n";
    }

    void removeKeyfile() const { std::remove(keyfilePath_.c_str()); }

    static std::vector<uint8_t> hexToBin(const std::string& hex) {
        std::vector<uint8_t> out;
        if (hex.size() % 2 != 0) return out;
        for (size_t i = 0; i < hex.size(); i += 2) {
            auto c1 = hexNibble(hex[i]), c2 = hexNibble(hex[i+1]);
            if (c1 < 0 || c2 < 0) return {};
            out.push_back((uint8_t)((c1 << 4) | c2));
        }
        return out;
    }
    static int hexNibble(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }
};

} // namespace milansql
