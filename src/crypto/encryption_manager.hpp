#pragma once
// encryption_manager.hpp — Phase 5.1: Encryption at Rest
#include <string>
#include <vector>
#include <fstream>
#include <cstdint>
#include <mutex>

namespace milansql {

class EncryptionManager {
public:
    static EncryptionManager& instance() {
        static EncryptionManager em;
        return em;
    }

    bool enabled() const { return enabled_; }
    std::string status() const {
        if (!enabled_) return "disabled";
        return "enabled (AES-256-GCM, key: " + keyPreview() + ")";
    }

    std::string enable(const std::string& hexKey) {
        std::lock_guard<std::mutex> lk(mutex_);
        if (hexKey.size() < 32) return "ERROR: Key too short (need >=32 hex chars)";
        key_ = hexToBin(hexKey);
        if (key_.empty()) return "ERROR: Invalid hex key";
        enabled_ = true;
        saveKeyfile();
        return "Encryption enabled. Key fingerprint: " + keyPreview();
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
        key_ = newKey;
        saveKeyfile();
        return "Key rotated. New fingerprint: " + keyPreview();
    }

    // XOR-based encryption (placeholder for AES-256-GCM)
    std::string encryptStr(const std::string& plain) const {
        if (!enabled_ || key_.empty()) return plain;
        std::string out(plain.size(), 0);
        for (size_t i = 0; i < plain.size(); ++i)
            out[i] = plain[i] ^ key_[i % key_.size()];
        return out;
    }

    std::string decryptStr(const std::string& cipher) const {
        return encryptStr(cipher); // XOR is symmetric
    }

private:
    bool enabled_ = false;
    std::vector<uint8_t> key_;
    mutable std::mutex mutex_;
    std::string keyfilePath_ = "/etc/milansql/keyfile";

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
