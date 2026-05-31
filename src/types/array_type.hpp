#pragma once
// ============================================================
// types/array_type.hpp — Phase 88: Array Data Type utilities
// ============================================================
#include <string>
#include <vector>
#include <algorithm>
#include <sstream>

namespace milansql {

class ArrayUtils {
public:
    // Returns true if the type string is an array type (e.g. "TEXT[]", "INT[]")
    static bool isArrayType(const std::string& t) {
        return t.size() >= 2 && t.back() == ']' && t[t.size()-2] == '[';
    }

    // Returns true if the value looks like an array literal: {a,b,c}
    static bool isArray(const std::string& s) {
        return s.size() >= 2 && s.front() == '{' && s.back() == '}';
    }

    // Parse {a,b,c} → ["a","b","c"]
    static std::vector<std::string> parse(const std::string& s) {
        std::vector<std::string> result;
        if (!isArray(s)) return result;
        std::string inner = s.substr(1, s.size() - 2);
        if (inner.empty()) return result;
        std::string cur;
        for (char c : inner) {
            if (c == ',') { result.push_back(cur); cur.clear(); }
            else cur += c;
        }
        result.push_back(cur);
        return result;
    }

    // Serialize vector to {a,b,c}
    static std::string serialize(const std::vector<std::string>& v) {
        std::string r = "{";
        for (size_t i = 0; i < v.size(); ++i) {
            if (i) r += ",";
            r += v[i];
        }
        return r + "}";
    }

    // array_length({a,b,c}) → "3"
    static std::string arrayLength(const std::string& arr) {
        if (!isArray(arr)) return "0";
        return std::to_string(static_cast<int>(parse(arr).size()));
    }

    // array_append({a,b}, c) → {a,b,c}
    static std::string arrayAppend(const std::string& arr, const std::string& elem) {
        auto v = parse(arr);
        v.push_back(elem);
        return serialize(v);
    }

    // array_remove({a,b,a}, a) → {b}
    static std::string arrayRemove(const std::string& arr, const std::string& elem) {
        auto v = parse(arr);
        v.erase(std::remove(v.begin(), v.end(), elem), v.end());
        return serialize(v);
    }

    // array_contains({a,b,c}, b) → "1", else "0"
    static std::string arrayContains(const std::string& arr, const std::string& elem) {
        auto v = parse(arr);
        return (std::find(v.begin(), v.end(), elem) != v.end()) ? "1" : "0";
    }

    // array_get({a,b,c}, 2) → "b"  (1-based)
    static std::string arrayGet(const std::string& arr, int idx) {
        auto v = parse(arr);
        if (idx < 1 || idx > static_cast<int>(v.size())) return "NULL";
        return v[static_cast<size_t>(idx - 1)];
    }

    // array_to_string({a,b,c}, ',') → "a,b,c"
    static std::string arrayToString(const std::string& arr, const std::string& delim) {
        auto v = parse(arr);
        std::string r;
        for (size_t i = 0; i < v.size(); ++i) {
            if (i) r += delim;
            r += v[i];
        }
        return r;
    }

    // string_to_array("a,b,c", ",") → {a,b,c}
    static std::string stringToArray(const std::string& str, const std::string& delim) {
        std::vector<std::string> v;
        if (delim.empty()) { v.push_back(str); return serialize(v); }
        size_t pos = 0, found;
        while ((found = str.find(delim, pos)) != std::string::npos) {
            v.push_back(str.substr(pos, found - pos));
            pos = found + delim.size();
        }
        v.push_back(str.substr(pos));
        return serialize(v);
    }
};

} // namespace milansql
