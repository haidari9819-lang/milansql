#pragma once
// ============================================================
// vector_ext.hpp — Phase 111: pgvector Extension Functions
//
// Functions registered when CREATE EXTENSION vector is run.
// Delegates math to vector_type.hpp.
// ============================================================

#include <string>
#include <vector>
#include <sstream>
#include <iomanip>
#include <stdexcept>
#include "../../types/vector_type.hpp"

namespace milansql {
namespace vector_ext {

// Evaluate a named vector function with string arguments.
// Returns the result as a string.
inline std::string evalVector(const std::string& fn,
                               const std::vector<std::string>& args) {
    using namespace vector_type;

    // ── l2_distance(a, b) → float ─────────────────────────────
    if (fn == "L2_DISTANCE") {
        if (args.size() < 2) return "NULL";
        auto a = parse(args[0]);
        auto b = parse(args[1]);
        if (a.empty() || b.empty() || a.size() != b.size()) return "NULL";
        return fmtFloat(l2Distance(a, b));
    }

    // ── cosine_similarity(a, b) → float ──────────────────────
    if (fn == "COSINE_SIMILARITY") {
        if (args.size() < 2) return "NULL";
        auto a = parse(args[0]);
        auto b = parse(args[1]);
        if (a.empty() || b.empty() || a.size() != b.size()) return "NULL";
        return fmtFloat(cosineSimilarity(a, b));
    }

    // ── inner_product(a, b) → float ───────────────────────────
    if (fn == "INNER_PRODUCT") {
        if (args.size() < 2) return "NULL";
        auto a = parse(args[0]);
        auto b = parse(args[1]);
        if (a.empty() || b.empty() || a.size() != b.size()) return "NULL";
        return fmtFloat(innerProduct(a, b));
    }

    // ── vector_dims(v) → int ──────────────────────────────────
    if (fn == "VECTOR_DIMS") {
        if (args.empty()) return "0";
        auto v = parse(args[0]);
        return std::to_string(static_cast<int>(v.size()));
    }

    // ── vector_norm(v) → float ────────────────────────────────
    if (fn == "VECTOR_NORM") {
        if (args.empty()) return "NULL";
        auto v = parse(args[0]);
        return fmtFloat(norm(v));
    }

    // ── vector_add(a, b) → vector ─────────────────────────────
    if (fn == "VECTOR_ADD") {
        if (args.size() < 2) return "NULL";
        auto a = parse(args[0]);
        auto b = parse(args[1]);
        if (a.size() != b.size()) return "NULL";
        return serialize(add(a, b));
    }

    // ── vector_sub(a, b) → vector ─────────────────────────────
    if (fn == "VECTOR_SUB") {
        if (args.size() < 2) return "NULL";
        auto a = parse(args[0]);
        auto b = parse(args[1]);
        if (a.size() != b.size()) return "NULL";
        return serialize(sub(a, b));
    }

    // ── vector_mul(a, scalar) → vector ────────────────────────
    if (fn == "VECTOR_MUL") {
        if (args.size() < 2) return "NULL";
        auto a = parse(args[0]);
        float s = 1.0f;
        try { s = std::stof(args[1]); } catch (...) {}
        return serialize(mul(a, s));
    }

    return "NULL";
}

} // namespace vector_ext
} // namespace milansql
