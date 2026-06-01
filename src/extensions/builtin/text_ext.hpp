#pragma once
// ============================================================
// text_ext.hpp — milansql_text extension (Phase 90)
// Provides: soundex, levenshtein, similarity,
//           initcap, repeat, lpad, rpad
// ============================================================
#include <string>
#include <vector>
#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace milansql {
namespace text_ext {

// ── Soundex (US National Archives algorithm) ──────────────────
static inline std::string soundex(const std::string& str) {
    if (str.empty()) return "0000";

    // Soundex coding table
    static const char codeTable[26] = {
        '0','1','2','3','0','1','2','0','0','2','2','4','5',
        '5','0','1','2','6','2','3','0','1','0','2','0','2'
    };

    std::string s;
    for (char c : str) {
        if (std::isalpha(static_cast<unsigned char>(c)))
            s += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    if (s.empty()) return "0000";

    std::string result;
    result += s[0];

    char prevCode = codeTable[static_cast<size_t>(s[0] - 'A')];
    for (size_t i = 1; i < s.size() && result.size() < 4; ++i) {
        char code = codeTable[static_cast<size_t>(s[i] - 'A')];
        if (code != '0' && code != prevCode) {
            result += code;
        }
        prevCode = code;
    }

    // Pad with zeros to length 4
    while (result.size() < 4) result += '0';
    return result;
}

// ── Levenshtein distance ──────────────────────────────────────
static inline int levenshtein(const std::string& s1, const std::string& s2) {
    size_t m = s1.size(), n = s2.size();
    std::vector<std::vector<int>> dp(m + 1, std::vector<int>(n + 1, 0));
    for (size_t i = 0; i <= m; ++i) dp[i][0] = static_cast<int>(i);
    for (size_t j = 0; j <= n; ++j) dp[0][j] = static_cast<int>(j);
    for (size_t i = 1; i <= m; ++i) {
        for (size_t j = 1; j <= n; ++j) {
            if (s1[i-1] == s2[j-1])
                dp[i][j] = dp[i-1][j-1];
            else
                dp[i][j] = 1 + std::min({dp[i-1][j], dp[i][j-1], dp[i-1][j-1]});
        }
    }
    return dp[m][n];
}

// ── Trigram similarity ────────────────────────────────────────
static inline double similarity(const std::string& s1, const std::string& s2) {
    if (s1.empty() && s2.empty()) return 1.0;
    if (s1.empty() || s2.empty()) return 0.0;

    auto getTrigrams = [](const std::string& s) -> std::vector<std::string> {
        std::vector<std::string> tg;
        // Pad with spaces: " " + s + " "
        std::string padded = " " + s + " ";
        for (size_t i = 0; i + 2 < padded.size(); ++i)
            tg.push_back(padded.substr(i, 3));
        return tg;
    };

    auto tg1 = getTrigrams(s1);
    auto tg2 = getTrigrams(s2);

    std::sort(tg1.begin(), tg1.end());
    std::sort(tg2.begin(), tg2.end());

    std::vector<std::string> common;
    std::set_intersection(tg1.begin(), tg1.end(),
                          tg2.begin(), tg2.end(),
                          std::back_inserter(common));

    size_t total = tg1.size() + tg2.size();
    if (total == 0) return 0.0;
    return (2.0 * static_cast<double>(common.size())) / static_cast<double>(total);
}

// ── initcap: capitalize first letter of each word ─────────────
static inline std::string initcap(const std::string& s) {
    std::string result = s;
    bool newWord = true;
    for (size_t i = 0; i < result.size(); ++i) {
        if (std::isspace(static_cast<unsigned char>(result[i]))) {
            newWord = true;
        } else if (newWord) {
            result[i] = static_cast<char>(
                std::toupper(static_cast<unsigned char>(result[i])));
            newWord = false;
        } else {
            result[i] = static_cast<char>(
                std::tolower(static_cast<unsigned char>(result[i])));
        }
    }
    return result;
}

// ── repeat ────────────────────────────────────────────────────
static inline std::string repeat(const std::string& s, int n) {
    if (n <= 0) return "";
    std::string result;
    result.reserve(s.size() * static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) result += s;
    return result;
}

// ── lpad ──────────────────────────────────────────────────────
static inline std::string lpad(const std::string& s, int width, const std::string& fill) {
    if (width <= 0 || static_cast<size_t>(width) <= s.size()) return s;
    std::string pad;
    std::string f = fill.empty() ? " " : fill;
    size_t needed = static_cast<size_t>(width) - s.size();
    while (pad.size() < needed)
        pad += f;
    return pad.substr(0, needed) + s;
}

// ── rpad ──────────────────────────────────────────────────────
static inline std::string rpad(const std::string& s, int width, const std::string& fill) {
    if (width <= 0 || static_cast<size_t>(width) <= s.size()) return s;
    std::string result = s;
    std::string f = fill.empty() ? " " : fill;
    size_t w = static_cast<size_t>(width);
    while (result.size() < w) result += f;
    return result.substr(0, w);
}

// ── Dispatcher ────────────────────────────────────────────────
static inline std::string evalText(const std::string& fn,
                                    const std::vector<std::string>& args) {
    if (fn == "SOUNDEX") {
        if (args.empty()) throw std::runtime_error("soundex() requires 1 argument");
        return soundex(args[0]);
    }
    if (fn == "LEVENSHTEIN") {
        if (args.size() < 2) throw std::runtime_error("levenshtein() requires 2 arguments");
        return std::to_string(levenshtein(args[0], args[1]));
    }
    if (fn == "SIMILARITY") {
        if (args.size() < 2) throw std::runtime_error("similarity() requires 2 arguments");
        double sim = similarity(args[0], args[1]);
        // Format to 6 decimal places
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.6f", sim);
        return buf;
    }
    if (fn == "INITCAP") {
        if (args.empty()) throw std::runtime_error("initcap() requires 1 argument");
        return initcap(args[0]);
    }
    if (fn == "REPEAT") {
        if (args.size() < 2) throw std::runtime_error("repeat() requires 2 arguments");
        int n = 0;
        try { n = std::stoi(args[1]); } catch (...) {}
        return repeat(args[0], n);
    }
    if (fn == "LPAD") {
        if (args.size() < 3) throw std::runtime_error("lpad() requires 3 arguments");
        int width = 0;
        try { width = std::stoi(args[1]); } catch (...) {}
        return lpad(args[0], width, args[2]);
    }
    if (fn == "RPAD") {
        if (args.size() < 3) throw std::runtime_error("rpad() requires 3 arguments");
        int width = 0;
        try { width = std::stoi(args[1]); } catch (...) {}
        return rpad(args[0], width, args[2]);
    }
    return "";
}

} // namespace text_ext
} // namespace milansql
