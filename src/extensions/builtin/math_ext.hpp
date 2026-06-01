#pragma once
// ============================================================
// math_ext.hpp — milansql_math extension (Phase 90)
// Provides: pi, exp, log, log10, sin, cos, tan,
//           radians, degrees, factorial
// ============================================================
#include <string>
#include <vector>
#include <cmath>
#include <stdexcept>
#include <sstream>
#include <iomanip>

namespace milansql {
namespace math_ext {

// Format a double: strip trailing zeros after decimal point
static inline std::string fmtDouble(double v) {
    // Use enough precision
    std::ostringstream oss;
    oss << std::setprecision(15) << v;
    std::string s = oss.str();
    // If it has a decimal point, strip trailing zeros
    if (s.find('.') != std::string::npos) {
        size_t last = s.find_last_not_of('0');
        if (last != std::string::npos && s[last] == '.')
            s.erase(last + 1);  // keep the dot only if needed? remove it
        else if (last != std::string::npos)
            s.erase(last + 1);
    }
    return s;
}

static inline double toDouble(const std::string& s) {
    try { return std::stod(s); }
    catch (...) { throw std::runtime_error("math_ext: not a number: " + s); }
}

static inline std::string evalMath(const std::string& fn,
                                    const std::vector<std::string>& args) {
    if (fn == "PI") {
        return "3.14159265358979";
    }
    if (fn == "EXP") {
        if (args.empty()) throw std::runtime_error("exp() requires 1 argument");
        return fmtDouble(std::exp(toDouble(args[0])));
    }
    if (fn == "LOG") {
        if (args.empty()) throw std::runtime_error("log() requires 1 argument");
        double v = toDouble(args[0]);
        if (v <= 0) throw std::runtime_error("log() domain error");
        return fmtDouble(std::log(v));
    }
    if (fn == "LOG10") {
        if (args.empty()) throw std::runtime_error("log10() requires 1 argument");
        double v = toDouble(args[0]);
        if (v <= 0) throw std::runtime_error("log10() domain error");
        return fmtDouble(std::log10(v));
    }
    if (fn == "SIN") {
        if (args.empty()) throw std::runtime_error("sin() requires 1 argument");
        double r = std::sin(toDouble(args[0]));
        // For sin(0) return "0" cleanly
        if (r == 0.0) return "0";
        return fmtDouble(r);
    }
    if (fn == "COS") {
        if (args.empty()) throw std::runtime_error("cos() requires 1 argument");
        double r = std::cos(toDouble(args[0]));
        if (r == 1.0) return "1";
        if (r == 0.0) return "0";
        return fmtDouble(r);
    }
    if (fn == "TAN") {
        if (args.empty()) throw std::runtime_error("tan() requires 1 argument");
        return fmtDouble(std::tan(toDouble(args[0])));
    }
    if (fn == "RADIANS") {
        if (args.empty()) throw std::runtime_error("radians() requires 1 argument");
        double v = toDouble(args[0]);
        return fmtDouble(v * 3.14159265358979323846 / 180.0);
    }
    if (fn == "DEGREES") {
        if (args.empty()) throw std::runtime_error("degrees() requires 1 argument");
        double v = toDouble(args[0]);
        return fmtDouble(v * 180.0 / 3.14159265358979323846);
    }
    if (fn == "FACTORIAL") {
        if (args.empty()) throw std::runtime_error("factorial() requires 1 argument");
        long long n = 0;
        try { n = static_cast<long long>(std::stod(args[0])); }
        catch (...) { throw std::runtime_error("factorial(): not an integer"); }
        if (n < 0) throw std::runtime_error("factorial(): negative input");
        if (n > 20) throw std::runtime_error("factorial(): n > 20 overflows");
        long long result = 1;
        for (long long i = 2; i <= n; ++i) result *= i;
        return std::to_string(result);
    }
    // Not handled by this extension
    return "";
}

} // namespace math_ext
} // namespace milansql
