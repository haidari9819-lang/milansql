#pragma once
// ──────────────────────────────────────────────────────────────────────────
// master_key.hpp — Phase 162: MasterKey singleton
// Loads a 32-byte master key from:
//   1. MILANSQL_MASTER_KEY env var (64-char hex)
//   2. milansql.key file (64-char hex)
//   3. random bytes (generated at startup)
// ──────────────────────────────────────────────────────────────────────────

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <random>
#include <string>
#include <vector>

namespace milansql {

class MasterKey {
public:
    static MasterKey& instance() {
        static MasterKey inst;
        return inst;
    }

    const std::vector<uint8_t>& getKey() const { return key_; }

    std::string getKeyHex() const {
        static const char hex[] = "0123456789abcdef";
        std::string out;
        out.reserve(64);
        for (uint8_t b : key_) {
            out += hex[b >> 4];
            out += hex[b & 0xf];
        }
        return out;
    }

    const std::string& getSource() const { return source_; }

private:
    std::vector<uint8_t> key_;
    std::string source_;

    MasterKey() {
        key_.resize(32, 0);
        // 1. Try environment variable
        const char* env = std::getenv("MILANSQL_MASTER_KEY");
        if (env && parseHex(env, key_)) {
            source_ = "env";
            return;
        }
        // 2. Try key file
        std::ifstream f("milansql.key");
        if (f.good()) {
            std::string line;
            std::getline(f, line);
            if (parseHex(line, key_)) {
                source_ = "file";
                return;
            }
        }
        // 3. Generate random key
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<int> dist(0, 255);
        for (auto& b : key_) b = static_cast<uint8_t>(dist(gen));
        source_ = "random";
    }

    static bool parseHex(const std::string& s, std::vector<uint8_t>& out) {
        if (s.size() != 64) return false;
        for (size_t i = 0; i < 32; ++i) {
            uint8_t hi = hexNibble(s[i * 2]);
            uint8_t lo = hexNibble(s[i * 2 + 1]);
            if (hi > 15 || lo > 15) return false;
            out[i] = static_cast<uint8_t>((hi << 4) | lo);
        }
        return true;
    }

    static uint8_t hexNibble(char c) {
        if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
        if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
        if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(c - 'A' + 10);
        return 255;
    }

    MasterKey(const MasterKey&) = delete;
    MasterKey& operator=(const MasterKey&) = delete;
};

} // namespace milansql
