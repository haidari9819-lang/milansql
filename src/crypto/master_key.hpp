#pragma once
// ============================================================
// master_key.hpp — MilanSQL Master Key Singleton (Phase 162)
// Cryptographic root key for per-user table name isolation.
//
// Load order:
//   1. Env var  MILANSQL_MASTER_KEY  (64 hex chars = 32 bytes)
//   2. File     milansql.key         (same hex format)
//   3. Random   generated at startup, saved to milansql.key
// ============================================================

#include <string>
#include <vector>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <random>
#include <stdexcept>

namespace milansql {

class MasterKey {
public:
    // Thread-safe Meyer's singleton
    static MasterKey& instance() {
        static MasterKey inst;
        return inst;
    }

    // Returns the 32-byte master key
    const std::vector<uint8_t>& getKey() const { return key_; }

    // Returns master key as lowercase hex string (64 chars)
    std::string getKeyHex() const {
        std::ostringstream oss;
        for (uint8_t b : key_)
            oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
        return oss.str();
    }

    // Source for diagnostics
    const std::string& getSource() const { return source_; }

    // Non-copyable
    MasterKey(const MasterKey&) = delete;
    MasterKey& operator=(const MasterKey&) = delete;

private:
    MasterKey() {
        // 1. Try environment variable MILANSQL_MASTER_KEY
        const char* envVal = std::getenv("MILANSQL_MASTER_KEY");
        if (envVal && loadFromHex(std::string(envVal))) {
            source_ = "env:MILANSQL_MASTER_KEY";
            return;
        }

        // 2. Try file milansql.key
        if (loadFromFile("milansql.key")) {
            source_ = "file:milansql.key";
            return;
        }

        // 3. Generate random 32-byte key
        generateRandom();
        source_ = "generated";
        // Save for future restarts (fail silently)
        saveToFile("milansql.key");
    }

    bool loadFromHex(const std::string& hex) {
        if (hex.size() < 64) return false;
        std::vector<uint8_t> tmp;
        tmp.reserve(32);
        for (size_t i = 0; i + 1 < hex.size() && tmp.size() < 32; i += 2) {
            try {
                tmp.push_back(static_cast<uint8_t>(
                    std::stoul(hex.substr(i, 2), nullptr, 16)));
            } catch (...) {
                return false;
            }
        }
        if (tmp.size() != 32) return false;
        key_ = std::move(tmp);
        return true;
    }

    bool loadFromFile(const std::string& path) {
        std::ifstream f(path);
        if (!f.is_open()) return false;
        std::string hex;
        f >> hex;
        return !hex.empty() && loadFromHex(hex);
    }

    void generateRandom() {
        key_.resize(32);
        std::random_device rd;
        std::mt19937 rng(rd());
        std::uniform_int_distribution<int> dist(0, 255);
        for (auto& b : key_)
            b = static_cast<uint8_t>(dist(rng));
    }

    void saveToFile(const std::string& path) {
        try {
            std::ofstream f(path);
            if (!f.is_open()) return;
            for (uint8_t b : key_)
                f << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
            f << "\n";
        } catch (...) {}
    }

    std::vector<uint8_t> key_;
    std::string          source_;
};

} // namespace milansql
