#pragma once
// ============================================================
// uuid_ext.hpp — milansql_uuid extension (Phase 90)
// Provides: gen_random_uuid (v4), uuid_generate_v1 (v1),
//           is_valid_uuid
// Uses <random> and <chrono> from stdlib only.
// ============================================================
#include <string>
#include <vector>
#include <random>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <stdexcept>
#include <cstdint>

namespace milansql {
namespace uuid_ext {

static inline std::string fmtHex8(uint32_t v) {
    std::ostringstream oss;
    oss << std::hex << std::setw(8) << std::setfill('0') << v;
    return oss.str();
}
static inline std::string fmtHex4(uint16_t v) {
    std::ostringstream oss;
    oss << std::hex << std::setw(4) << std::setfill('0') << v;
    return oss.str();
}

// UUID v4: random
static inline std::string genUuidV4() {
    static thread_local std::mt19937_64 rng{
        static_cast<uint64_t>(
            std::chrono::high_resolution_clock::now().time_since_epoch().count()
        )
    };
    std::uniform_int_distribution<uint64_t> dist(0, UINT64_MAX);
    uint64_t hi = dist(rng);
    uint64_t lo = dist(rng);

    // Set version 4 bits (bits 12-15 of hi = 0100)
    hi = (hi & 0xFFFFFFFFFFFF0FFFull) | 0x0000000000004000ull;
    // Set variant bits (bits 62-63 of lo = 10)
    lo = (lo & 0x3FFFFFFFFFFFFFFFull) | 0x8000000000000000ull;

    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    // time_low (32 bits)
    oss << std::setw(8) << ((hi >> 32) & 0xFFFFFFFF) << '-';
    // time_mid (16 bits)
    oss << std::setw(4) << ((hi >> 16) & 0xFFFF) << '-';
    // time_hi_and_version (16 bits)
    oss << std::setw(4) << (hi & 0xFFFF) << '-';
    // clock_seq_hi_and_res + clock_seq_low (16 bits)
    oss << std::setw(4) << ((lo >> 48) & 0xFFFF) << '-';
    // node (48 bits)
    oss << std::setw(12) << (lo & 0xFFFFFFFFFFFFull);
    return oss.str();
}

// UUID v1: timestamp-based
static inline std::string genUuidV1() {
    // Get timestamp: nanoseconds since Oct 15, 1582
    auto now = std::chrono::high_resolution_clock::now().time_since_epoch();
    uint64_t ns100 = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count() / 100
    );
    // Offset from Unix epoch (Jan 1, 1970) to UUID epoch (Oct 15, 1582)
    // = 122192928000000000 * 100ns intervals
    ns100 += 0x01B21DD213814000ULL;

    uint32_t time_low          = static_cast<uint32_t>(ns100 & 0xFFFFFFFF);
    uint16_t time_mid          = static_cast<uint16_t>((ns100 >> 32) & 0xFFFF);
    uint16_t time_hi_and_ver   = static_cast<uint16_t>(((ns100 >> 48) & 0x0FFF) | 0x1000);

    static thread_local std::mt19937 rng32{
        static_cast<uint32_t>(
            std::chrono::steady_clock::now().time_since_epoch().count() & 0xFFFFFFFF
        )
    };
    std::uniform_int_distribution<uint16_t> dist16(0, 0xFFFF);
    uint16_t clock_seq = static_cast<uint16_t>((dist16(rng32) & 0x3FFF) | 0x8000);

    // Random node (48 bits)
    uint64_t node = (static_cast<uint64_t>(dist16(rng32)) << 32) |
                    (static_cast<uint64_t>(dist16(rng32)) << 16) |
                     static_cast<uint64_t>(dist16(rng32));
    // Set multicast bit in node
    node |= 0x010000000000ULL;

    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    oss << std::setw(8) << time_low          << '-';
    oss << std::setw(4) << time_mid          << '-';
    oss << std::setw(4) << time_hi_and_ver   << '-';
    oss << std::setw(4) << clock_seq         << '-';
    oss << std::setw(12) << (node & 0xFFFFFFFFFFFFull);
    return oss.str();
}

// Validate UUID format: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
static inline bool validateUuid(const std::string& s) {
    if (s.size() != 36) return false;
    // Check positions of hyphens
    if (s[8] != '-' || s[13] != '-' || s[18] != '-' || s[23] != '-') return false;
    for (size_t i = 0; i < 36; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) continue;
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
            return false;
    }
    return true;
}

// Dispatcher
static inline std::string evalUuid(const std::string& fn,
                                    const std::vector<std::string>& args) {
    if (fn == "GEN_RANDOM_UUID") {
        return genUuidV4();
    }
    if (fn == "UUID_GENERATE_V1") {
        return genUuidV1();
    }
    if (fn == "IS_VALID_UUID") {
        if (args.empty()) throw std::runtime_error("is_valid_uuid() requires 1 argument");
        return validateUuid(args[0]) ? "1" : "0";
    }
    return "";
}

} // namespace uuid_ext
} // namespace milansql
