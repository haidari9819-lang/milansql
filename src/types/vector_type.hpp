#pragma once
// ============================================================
// vector_type.hpp — Phase 111: pgvector-compatible Vector Type
//
// Stores dense float vectors as "[v0,v1,...,vN]" strings.
// Provides all math needed for similarity search.
// Zero external dependencies.
// ============================================================

#include <string>
#include <vector>
#include <cmath>
#include <sstream>
#include <iomanip>
#include <stdexcept>
#include <algorithm>
#include <numeric>

namespace milansql {
namespace vector_type {

// ── Parse "[0.1, 0.2, 0.3]" or "0.1,0.2,0.3" → vector<float> ─
inline std::vector<float> parse(const std::string& s) {
    std::vector<float> result;
    if (s.empty()) return result;

    // Strip outer brackets and single quotes
    size_t start = 0, end = s.size();
    while (start < end && (s[start] == '\'' || s[start] == '"')) ++start;
    while (end > start && (s[end-1] == '\'' || s[end-1] == '"')) --end;
    while (start < end && (s[start] == '[' || s[start] == ' ')) ++start;
    while (end > start && (s[end-1] == ']' || s[end-1] == ' ')) --end;

    if (start >= end) return result;

    std::string inner = s.substr(start, end - start);
    std::istringstream ss(inner);
    std::string token;
    while (std::getline(ss, token, ',')) {
        // Trim spaces
        size_t a = token.find_first_not_of(" \t\r\n");
        size_t b = token.find_last_not_of(" \t\r\n");
        if (a == std::string::npos) continue;
        token = token.substr(a, b - a + 1);
        if (!token.empty()) {
            try { result.push_back(std::stof(token)); }
            catch (...) {}
        }
    }
    return result;
}

// ── Serialize vector<float> → "[0.1,0.2,0.3]" ─────────────────
inline std::string serialize(const std::vector<float>& v) {
    std::ostringstream oss;
    oss << "[";
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) oss << ",";
        // Format: up to 6 significant digits
        float f = v[i];
        // Check if it's an integer value
        if (f == static_cast<long long>(f) && std::abs(f) < 1e12f) {
            oss << static_cast<long long>(f);
        } else {
            oss << std::fixed << std::setprecision(6) << f;
            // Trim trailing zeros
            std::string s = oss.str();
            // Get last number
            size_t dot = s.rfind('.');
            if (dot != std::string::npos) {
                size_t last = s.size() - 1;
                while (last > dot + 1 && s[last] == '0') --last;
                oss.str("");
                oss << s.substr(0, last + 1);
            }
        }
    }
    oss << "]";
    return oss.str();
}

// ── Dimension check ────────────────────────────────────────────
inline int dims(const std::vector<float>& v) {
    return static_cast<int>(v.size());
}

// Extract dimension from type string "VECTOR(1536)" → 1536
inline int dimsFromType(const std::string& typeStr) {
    auto lp = typeStr.find('(');
    auto rp = typeStr.find(')');
    if (lp == std::string::npos || rp == std::string::npos) return 0;
    try { return std::stoi(typeStr.substr(lp + 1, rp - lp - 1)); }
    catch (...) { return 0; }
}

inline bool isVectorType(const std::string& typeStr) {
    std::string u = typeStr;
    for (auto& c : u) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return u.size() >= 6 && u.substr(0, 6) == "VECTOR";
}

// ── L2 Distance (Euclidean) ───────────────────────────────────
inline float l2Distance(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size()) return -1.0f;
    float sum = 0.0f;
    for (size_t i = 0; i < a.size(); ++i) {
        float d = a[i] - b[i];
        sum += d * d;
    }
    return std::sqrt(sum);
}

// ── L2 Distance squared (for HNSW internal use) ───────────────
inline float l2DistanceSq(const std::vector<float>& a, const std::vector<float>& b) {
    float sum = 0.0f;
    for (size_t i = 0; i < a.size(); ++i) {
        float d = a[i] - b[i];
        sum += d * d;
    }
    return sum;
}

// ── L2 Norm ───────────────────────────────────────────────────
inline float norm(const std::vector<float>& v) {
    float sum = 0.0f;
    for (float f : v) sum += f * f;
    return std::sqrt(sum);
}

// ── Inner Product ─────────────────────────────────────────────
inline float innerProduct(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size()) return 0.0f;
    float sum = 0.0f;
    for (size_t i = 0; i < a.size(); ++i) sum += a[i] * b[i];
    return sum;
}

// ── Cosine Similarity ─────────────────────────────────────────
inline float cosineSimilarity(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size()) return 0.0f;
    float dot  = innerProduct(a, b);
    float na   = norm(a);
    float nb   = norm(b);
    if (na < 1e-9f || nb < 1e-9f) return 0.0f;
    return dot / (na * nb);
}

// ── Cosine Distance = 1 - cosine similarity ───────────────────
inline float cosineDistance(const std::vector<float>& a, const std::vector<float>& b) {
    return 1.0f - cosineSimilarity(a, b);
}

// ── Elementwise operations ────────────────────────────────────
inline std::vector<float> add(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size()) return a;
    std::vector<float> r(a.size());
    for (size_t i = 0; i < a.size(); ++i) r[i] = a[i] + b[i];
    return r;
}

inline std::vector<float> sub(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size()) return a;
    std::vector<float> r(a.size());
    for (size_t i = 0; i < a.size(); ++i) r[i] = a[i] - b[i];
    return r;
}

inline std::vector<float> mul(const std::vector<float>& a, float scalar) {
    std::vector<float> r(a.size());
    for (size_t i = 0; i < a.size(); ++i) r[i] = a[i] * scalar;
    return r;
}

// ── Format float for output ───────────────────────────────────
inline std::string fmtFloat(float f) {
    std::ostringstream oss;
    // 6 significant digits, trimmed trailing zeros
    oss << std::fixed << std::setprecision(6) << f;
    std::string s = oss.str();
    if (s.find('.') != std::string::npos) {
        size_t last = s.size() - 1;
        while (last > 0 && s[last] == '0') --last;
        if (s[last] == '.') ++last;
        s = s.substr(0, last + 1);
    }
    return s;
}

} // namespace vector_type
} // namespace milansql
