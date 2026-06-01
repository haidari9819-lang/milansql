#pragma once
// ============================================================
// crypto_ext.hpp — milansql_crypto extension (Phase 90)
// Pure C++ stdlib: MD5, SHA1, SHA256, Base64 encode/decode
// No external dependencies.
// ============================================================
#include <string>
#include <vector>
#include <cstdint>
#include <sstream>
#include <iomanip>
#include <stdexcept>
#include <algorithm>
#include <cstring>

namespace milansql {
namespace crypto_ext {

// ── MD5 (RFC 1321) ────────────────────────────────────────────
static inline std::string md5(const std::string& input) {
    // Per-round shift amounts
    static const uint32_t s[64] = {
        7,12,17,22, 7,12,17,22, 7,12,17,22, 7,12,17,22,
        5, 9,14,20, 5, 9,14,20, 5, 9,14,20, 5, 9,14,20,
        4,11,16,23, 4,11,16,23, 4,11,16,23, 4,11,16,23,
        6,10,15,21, 6,10,15,21, 6,10,15,21, 6,10,15,21
    };
    // Precomputed table of sin-derived constants
    static const uint32_t K[64] = {
        0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,
        0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
        0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,
        0x6b901122,0xfd987193,0xa679438e,0x49b40821,
        0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,
        0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
        0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,
        0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
        0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,
        0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
        0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,
        0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
        0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,
        0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
        0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,
        0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391
    };

    // Pre-processing: pad message
    uint64_t origLen = static_cast<uint64_t>(input.size()) * 8;
    std::vector<uint8_t> msg(input.begin(), input.end());
    msg.push_back(0x80);
    while (msg.size() % 64 != 56) msg.push_back(0x00);
    // Append original length as 64-bit little-endian
    for (int i = 0; i < 8; ++i)
        msg.push_back(static_cast<uint8_t>((origLen >> (8 * i)) & 0xFF));

    // Initial hash values
    uint32_t a0 = 0x67452301u;
    uint32_t b0 = 0xefcdab89u;
    uint32_t c0 = 0x98badcfeu;
    uint32_t d0 = 0x10325476u;

    auto leftRotate = [](uint32_t x, uint32_t n) -> uint32_t {
        return (x << n) | (x >> (32 - n));
    };

    // Process each 512-bit chunk
    for (size_t off = 0; off < msg.size(); off += 64) {
        uint32_t M[16];
        for (int j = 0; j < 16; ++j) {
            M[j] = (static_cast<uint32_t>(msg[off + static_cast<size_t>(j)*4])       ) |
                   (static_cast<uint32_t>(msg[off + static_cast<size_t>(j)*4 + 1]) <<  8) |
                   (static_cast<uint32_t>(msg[off + static_cast<size_t>(j)*4 + 2]) << 16) |
                   (static_cast<uint32_t>(msg[off + static_cast<size_t>(j)*4 + 3]) << 24);
        }
        uint32_t A = a0, B = b0, C = c0, D = d0;
        for (uint32_t i = 0; i < 64; ++i) {
            uint32_t F, g;
            if (i < 16)      { F = (B & C) | (~B & D); g = i; }
            else if (i < 32) { F = (D & B) | (~D & C); g = (5*i + 1) % 16; }
            else if (i < 48) { F = B ^ C ^ D;           g = (3*i + 5) % 16; }
            else             { F = C ^ (B | ~D);         g = (7*i)     % 16; }
            F = F + A + K[i] + M[g];
            A = D; D = C; C = B;
            B = B + leftRotate(F, s[i]);
        }
        a0 += A; b0 += B; c0 += C; d0 += D;
    }

    // Produce digest as hex string (little-endian)
    std::ostringstream oss;
    auto hexLE = [&](uint32_t v) {
        for (int i = 0; i < 4; ++i)
            oss << std::hex << std::setw(2) << std::setfill('0')
                << static_cast<unsigned>((v >> (8*i)) & 0xFF);
    };
    hexLE(a0); hexLE(b0); hexLE(c0); hexLE(d0);
    return oss.str();
}

// ── SHA1 (FIPS 180-4) ─────────────────────────────────────────
static inline std::string sha1(const std::string& input) {
    uint64_t origLen = static_cast<uint64_t>(input.size()) * 8;
    std::vector<uint8_t> msg(input.begin(), input.end());
    msg.push_back(0x80);
    while (msg.size() % 64 != 56) msg.push_back(0x00);
    // Big-endian length
    for (int i = 7; i >= 0; --i)
        msg.push_back(static_cast<uint8_t>((origLen >> (8*i)) & 0xFF));

    uint32_t h0 = 0x67452301u;
    uint32_t h1 = 0xEFCDAB89u;
    uint32_t h2 = 0x98BADCFEu;
    uint32_t h3 = 0x10325476u;
    uint32_t h4 = 0xC3D2E1F0u;

    auto leftRotate = [](uint32_t v, uint32_t n) -> uint32_t {
        return (v << n) | (v >> (32 - n));
    };

    for (size_t off = 0; off < msg.size(); off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<uint32_t>(msg[off + static_cast<size_t>(i)*4])     << 24) |
                   (static_cast<uint32_t>(msg[off + static_cast<size_t>(i)*4 + 1]) << 16) |
                   (static_cast<uint32_t>(msg[off + static_cast<size_t>(i)*4 + 2]) <<  8) |
                   (static_cast<uint32_t>(msg[off + static_cast<size_t>(i)*4 + 3])       );
        }
        for (int i = 16; i < 80; ++i)
            w[i] = leftRotate(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);

        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20)      { f = (b & c) | (~b & d);       k = 0x5A827999u; }
            else if (i < 40) { f = b ^ c ^ d;                 k = 0x6ED9EBA1u; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
            else             { f = b ^ c ^ d;                 k = 0xCA62C1D6u; }
            uint32_t temp = leftRotate(a, 5) + f + e + k + w[i];
            e = d; d = c; c = leftRotate(b, 30); b = a; a = temp;
        }
        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
    }

    std::ostringstream oss;
    auto hexBE = [&](uint32_t v) {
        oss << std::hex << std::setw(8) << std::setfill('0') << v;
    };
    hexBE(h0); hexBE(h1); hexBE(h2); hexBE(h3); hexBE(h4);
    return oss.str();
}

// ── SHA256 (FIPS 180-4) ───────────────────────────────────────
static inline std::string sha256(const std::string& input) {
    static const uint32_t K256[64] = {
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

    uint64_t origLen = static_cast<uint64_t>(input.size()) * 8;
    std::vector<uint8_t> msg(input.begin(), input.end());
    msg.push_back(0x80);
    while (msg.size() % 64 != 56) msg.push_back(0x00);
    for (int i = 7; i >= 0; --i)
        msg.push_back(static_cast<uint8_t>((origLen >> (8*i)) & 0xFF));

    uint32_t h[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
    };

    auto rotr = [](uint32_t x, uint32_t n) -> uint32_t {
        return (x >> n) | (x << (32 - n));
    };

    for (size_t off = 0; off < msg.size(); off += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<uint32_t>(msg[off + static_cast<size_t>(i)*4])     << 24) |
                   (static_cast<uint32_t>(msg[off + static_cast<size_t>(i)*4 + 1]) << 16) |
                   (static_cast<uint32_t>(msg[off + static_cast<size_t>(i)*4 + 2]) <<  8) |
                   (static_cast<uint32_t>(msg[off + static_cast<size_t>(i)*4 + 3])       );
        }
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = rotr(w[i-15], 7) ^ rotr(w[i-15], 18) ^ (w[i-15] >> 3);
            uint32_t s1 = rotr(w[i-2],  17) ^ rotr(w[i-2],  19) ^ (w[i-2]  >> 10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t S1    = rotr(e,6) ^ rotr(e,11) ^ rotr(e,25);
            uint32_t ch    = (e & f) ^ (~e & g);
            uint32_t temp1 = hh + S1 + ch + K256[i] + w[i];
            uint32_t S0    = rotr(a,2) ^ rotr(a,13) ^ rotr(a,22);
            uint32_t maj   = (a & b) ^ (a & c) ^ (b & c);
            uint32_t temp2 = S0 + maj;
            hh=g; g=f; f=e; e=d+temp1; d=c; c=b; b=a; a=temp1+temp2;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d;
        h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
    }

    std::ostringstream oss;
    for (int i = 0; i < 8; ++i)
        oss << std::hex << std::setw(8) << std::setfill('0') << h[i];
    return oss.str();
}

// ── Base64 ────────────────────────────────────────────────────
static inline std::string base64Encode(const std::string& input) {
    static const char* tbl =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((input.size() + 2) / 3) * 4);
    size_t i = 0;
    while (i + 2 < input.size()) {
        uint32_t v = (static_cast<uint8_t>(input[i]) << 16) |
                     (static_cast<uint8_t>(input[i+1]) << 8) |
                      static_cast<uint8_t>(input[i+2]);
        out += tbl[(v >> 18) & 0x3F];
        out += tbl[(v >> 12) & 0x3F];
        out += tbl[(v >>  6) & 0x3F];
        out += tbl[ v        & 0x3F];
        i += 3;
    }
    if (i < input.size()) {
        uint32_t v = static_cast<uint8_t>(input[i]) << 16;
        if (i + 1 < input.size()) v |= static_cast<uint8_t>(input[i+1]) << 8;
        out += tbl[(v >> 18) & 0x3F];
        out += tbl[(v >> 12) & 0x3F];
        out += (i + 1 < input.size()) ? tbl[(v >> 6) & 0x3F] : '=';
        out += '=';
    }
    return out;
}

static inline std::string base64Decode(const std::string& input) {
    static const int8_t dtbl[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1, 0,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
    };
    std::string out;
    out.reserve(input.size() * 3 / 4);
    uint32_t buf = 0;
    int bits = 0;
    for (unsigned char c : input) {
        if (c == '=') break;
        int8_t v = dtbl[c];
        if (v < 0) continue;
        buf = (buf << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((buf >> bits) & 0xFF);
        }
    }
    return out;
}

// ── Dispatcher ────────────────────────────────────────────────
static inline std::string evalCrypto(const std::string& fn,
                                      const std::vector<std::string>& args) {
    if (fn == "MD5") {
        if (args.empty()) throw std::runtime_error("md5() requires 1 argument");
        return md5(args[0]);
    }
    if (fn == "SHA1") {
        if (args.empty()) throw std::runtime_error("sha1() requires 1 argument");
        return sha1(args[0]);
    }
    if (fn == "SHA256") {
        if (args.empty()) throw std::runtime_error("sha256() requires 1 argument");
        return sha256(args[0]);
    }
    if (fn == "ENCODE") {
        if (args.size() < 2) throw std::runtime_error("encode() requires 2 arguments");
        std::string method = args[1];
        for (char& c : method) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (method == "BASE64") return base64Encode(args[0]);
        throw std::runtime_error("encode(): unknown method: " + args[1]);
    }
    if (fn == "DECODE") {
        if (args.size() < 2) throw std::runtime_error("decode() requires 2 arguments");
        std::string method = args[1];
        for (char& c : method) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (method == "BASE64") return base64Decode(args[0]);
        throw std::runtime_error("decode(): unknown method: " + args[1]);
    }
    return "";
}

} // namespace crypto_ext
} // namespace milansql
