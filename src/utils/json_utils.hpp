#pragma once
// Phase 56: JSON Utility-Funktionen für MilanSQL
// Einfacher rekursiver JSON-Parser + Extractor
// Keine externen Abhängigkeiten – reines C++17

#include <string>
#include <vector>
#include <map>
#include <stdexcept>
#include <sstream>
#include <algorithm>
#include <cctype>

namespace milansql {
namespace jsonutils {

// ── JsonValue ────────────────────────────────────────────────────────────────

struct JsonValue {
    enum class Type { OBJECT, ARRAY, STRING, NUMBER, BOOL, NULLVAL };
    Type type = Type::NULLVAL;

    std::string                         str;   // STRING / NUMBER (raw)
    bool                                b   = false;
    std::map<std::string, JsonValue>    obj;   // OBJECT
    std::vector<JsonValue>              arr;   // ARRAY
};

// ── Vorwärts-Deklarationen ───────────────────────────────────────────────────

static JsonValue parseValue(const std::string& s, size_t& pos);
static std::string serializeValue(const JsonValue& v);

// ── Hilfsfunktionen ──────────────────────────────────────────────────────────

static inline void skipWhitespace(const std::string& s, size_t& pos) {
    while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos])))
        ++pos;
}

static inline std::string parseString(const std::string& s, size_t& pos) {
    // pos zeigt auf '"'
    ++pos; // skip opening "
    std::string result;
    while (pos < s.size() && s[pos] != '"') {
        if (s[pos] == '\\' && pos + 1 < s.size()) {
            ++pos;
            switch (s[pos]) {
                case '"':  result += '"';  break;
                case '\\': result += '\\'; break;
                case '/':  result += '/';  break;
                case 'n':  result += '\n'; break;
                case 'r':  result += '\r'; break;
                case 't':  result += '\t'; break;
                default:   result += s[pos]; break;
            }
        } else {
            result += s[pos];
        }
        ++pos;
    }
    if (pos < s.size()) ++pos; // skip closing "
    return result;
}

static inline JsonValue parseObject(const std::string& s, size_t& pos) {
    JsonValue v;
    v.type = JsonValue::Type::OBJECT;
    ++pos; // skip '{'
    skipWhitespace(s, pos);
    if (pos < s.size() && s[pos] == '}') { ++pos; return v; }
    while (pos < s.size()) {
        skipWhitespace(s, pos);
        if (pos >= s.size() || s[pos] != '"')
            throw std::runtime_error("JSON: Schlüssel erwartet");
        std::string key = parseString(s, pos);
        skipWhitespace(s, pos);
        if (pos >= s.size() || s[pos] != ':')
            throw std::runtime_error("JSON: ':' erwartet");
        ++pos;
        skipWhitespace(s, pos);
        v.obj[key] = parseValue(s, pos);
        skipWhitespace(s, pos);
        if (pos < s.size() && s[pos] == ',') { ++pos; continue; }
        if (pos < s.size() && s[pos] == '}') { ++pos; break; }
        throw std::runtime_error("JSON: ',' oder '}' erwartet");
    }
    return v;
}

static inline JsonValue parseArray(const std::string& s, size_t& pos) {
    JsonValue v;
    v.type = JsonValue::Type::ARRAY;
    ++pos; // skip '['
    skipWhitespace(s, pos);
    if (pos < s.size() && s[pos] == ']') { ++pos; return v; }
    while (pos < s.size()) {
        skipWhitespace(s, pos);
        v.arr.push_back(parseValue(s, pos));
        skipWhitespace(s, pos);
        if (pos < s.size() && s[pos] == ',') { ++pos; continue; }
        if (pos < s.size() && s[pos] == ']') { ++pos; break; }
        throw std::runtime_error("JSON: ',' oder ']' erwartet");
    }
    return v;
}

static JsonValue parseValue(const std::string& s, size_t& pos) {
    skipWhitespace(s, pos);
    if (pos >= s.size()) throw std::runtime_error("JSON: unerwartetes Ende");

    char c = s[pos];

    if (c == '{') return parseObject(s, pos);
    if (c == '[') return parseArray(s, pos);
    if (c == '"') {
        JsonValue v;
        v.type = JsonValue::Type::STRING;
        v.str  = parseString(s, pos);
        return v;
    }
    // null
    if (s.substr(pos, 4) == "null") {
        pos += 4;
        JsonValue v; v.type = JsonValue::Type::NULLVAL; return v;
    }
    // true
    if (s.substr(pos, 4) == "true") {
        pos += 4;
        JsonValue v; v.type = JsonValue::Type::BOOL; v.b = true; return v;
    }
    // false
    if (s.substr(pos, 5) == "false") {
        pos += 5;
        JsonValue v; v.type = JsonValue::Type::BOOL; v.b = false; return v;
    }
    // Zahl (inkl. negativ, Dezimal, Exponent)
    if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) {
        size_t start = pos;
        if (s[pos] == '-') ++pos;
        while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos]))) ++pos;
        if (pos < s.size() && s[pos] == '.') {
            ++pos;
            while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos]))) ++pos;
        }
        if (pos < s.size() && (s[pos] == 'e' || s[pos] == 'E')) {
            ++pos;
            if (pos < s.size() && (s[pos] == '+' || s[pos] == '-')) ++pos;
            while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos]))) ++pos;
        }
        JsonValue v;
        v.type = JsonValue::Type::NUMBER;
        v.str  = s.substr(start, pos - start);
        return v;
    }
    throw std::runtime_error(std::string("JSON: unbekanntes Zeichen '") + c + "'");
}

// ── Serialisierung ────────────────────────────────────────────────────────────

static std::string escapeJsonString(const std::string& s) {
    std::string r;
    r.reserve(s.size() + 4);
    for (char c : s) {
        if (c == '"')  { r += "\\\""; }
        else if (c == '\\') { r += "\\\\"; }
        else if (c == '\n') { r += "\\n"; }
        else if (c == '\r') { r += "\\r"; }
        else if (c == '\t') { r += "\\t"; }
        else r += c;
    }
    return r;
}

static std::string serializeValue(const JsonValue& v) {
    switch (v.type) {
        case JsonValue::Type::NULLVAL: return "null";
        case JsonValue::Type::BOOL:    return v.b ? "true" : "false";
        case JsonValue::Type::NUMBER:  return v.str;
        case JsonValue::Type::STRING:  return "\"" + escapeJsonString(v.str) + "\"";
        case JsonValue::Type::ARRAY: {
            std::string r = "[";
            for (size_t i = 0; i < v.arr.size(); ++i) {
                if (i) r += ",";
                r += serializeValue(v.arr[i]);
            }
            return r + "]";
        }
        case JsonValue::Type::OBJECT: {
            std::string r = "{";
            bool first = true;
            for (const auto& [k, val] : v.obj) {
                if (!first) r += ",";
                first = false;
                r += "\"" + escapeJsonString(k) + "\":" + serializeValue(val);
            }
            return r + "}";
        }
    }
    return "null";
}

// ── Öffentliche API ───────────────────────────────────────────────────────────

// Parst JSON-String → JsonValue (wirft bei Fehler)
inline JsonValue parse(const std::string& s) {
    // Umgebende Anführungszeichen entfernen, falls vorhanden (gespeicherter Wert)
    std::string clean = s;
    if (clean.size() >= 2 && clean.front() == '\'' && clean.back() == '\'')
        clean = clean.substr(1, clean.size() - 2);
    size_t pos = 0;
    skipWhitespace(clean, pos);
    JsonValue v = parseValue(clean, pos);
    skipWhitespace(clean, pos);
    if (pos != clean.size())
        throw std::runtime_error("JSON: unerwartete Zeichen nach Wert");
    return v;
}

// Überprüft ob ein String valides JSON ist
inline bool isValid(const std::string& s) {
    if (s.empty() || s == "NULL" || s == "null") return false;
    try { parse(s); return true; }
    catch (...) { return false; }
}

// ── JSON_EXTRACT ─────────────────────────────────────────────────────────────
// Pfad-Syntax: $.key, $.key.subkey, $.arr[0], $[0]

static const JsonValue* navigatePath(const JsonValue& root,
                                      const std::string& path) {
    // path muss mit '$' beginnen
    size_t i = 0;
    if (path.empty() || path[0] != '$') return nullptr;
    ++i; // skip '$'

    const JsonValue* cur = &root;
    while (i < path.size()) {
        if (path[i] == '.') {
            ++i; // skip '.'
            // Schlüsselname lesen
            size_t start = i;
            while (i < path.size() && path[i] != '.' && path[i] != '[')
                ++i;
            std::string key = path.substr(start, i - start);
            if (key.empty()) return nullptr;
            if (cur->type != JsonValue::Type::OBJECT) return nullptr;
            auto it = cur->obj.find(key);
            if (it == cur->obj.end()) return nullptr;
            cur = &it->second;
        } else if (path[i] == '[') {
            ++i; // skip '['
            size_t start = i;
            while (i < path.size() && path[i] != ']') ++i;
            std::string idxStr = path.substr(start, i - start);
            if (i < path.size()) ++i; // skip ']'
            if (cur->type != JsonValue::Type::ARRAY) return nullptr;
            try {
                size_t idx = static_cast<size_t>(std::stoul(idxStr));
                if (idx >= cur->arr.size()) return nullptr;
                cur = &cur->arr[idx];
            } catch (...) { return nullptr; }
        } else {
            return nullptr;
        }
    }
    return cur;
}

// JSON_EXTRACT(json, path) → String-Repräsentation des Werts
inline std::string extract(const std::string& jsonStr, const std::string& path) {
    try {
        JsonValue root = parse(jsonStr);
        const JsonValue* node = navigatePath(root, path);
        if (!node) return "NULL";
        switch (node->type) {
            case JsonValue::Type::NULLVAL: return "NULL";
            case JsonValue::Type::BOOL:    return node->b ? "true" : "false";
            case JsonValue::Type::NUMBER:  return node->str;
            case JsonValue::Type::STRING:  return node->str; // ohne Anführungszeichen
            default: return serializeValue(*node);
        }
    } catch (...) { return "NULL"; }
}

// ── JSON_SET ─────────────────────────────────────────────────────────────────
// JSON_SET(json, path, value) → neues JSON als String

static void setValueAtPath(JsonValue& root,
                            const std::string& path,
                            const JsonValue& newVal) {
    // Navigiert zum Parent und setzt den Wert
    if (path.size() < 2 || path[0] != '$') return;

    // Pfad in Segmente zerlegen
    std::vector<std::string> segs;
    size_t i = 1;
    while (i < path.size()) {
        if (path[i] == '.') {
            ++i;
            size_t start = i;
            while (i < path.size() && path[i] != '.' && path[i] != '[') ++i;
            segs.push_back(path.substr(start, i - start));
        } else if (path[i] == '[') {
            ++i;
            size_t start = i;
            while (i < path.size() && path[i] != ']') ++i;
            segs.push_back("[" + path.substr(start, i - start) + "]");
            if (i < path.size()) ++i;
        } else break;
    }
    if (segs.empty()) { root = newVal; return; }

    // Navigate to parent
    JsonValue* cur = &root;
    for (size_t si = 0; si + 1 < segs.size(); ++si) {
        const std::string& seg = segs[si];
        if (seg.size() >= 2 && seg.front() == '[' && seg.back() == ']') {
            size_t idx = static_cast<size_t>(std::stoul(seg.substr(1, seg.size() - 2)));
            if (cur->type != JsonValue::Type::ARRAY || idx >= cur->arr.size()) return;
            cur = &cur->arr[idx];
        } else {
            if (cur->type != JsonValue::Type::OBJECT) return;
            auto it = cur->obj.find(seg);
            if (it == cur->obj.end()) return;
            cur = &it->second;
        }
    }

    // Set at last segment
    const std::string& last = segs.back();
    if (last.size() >= 2 && last.front() == '[' && last.back() == ']') {
        size_t idx = static_cast<size_t>(std::stoul(last.substr(1, last.size() - 2)));
        if (cur->type == JsonValue::Type::ARRAY && idx < cur->arr.size())
            cur->arr[idx] = newVal;
    } else {
        if (cur->type == JsonValue::Type::OBJECT)
            cur->obj[last] = newVal;
    }
}

// Parst einen Wert-String als JsonValue (für JSON_SET)
static JsonValue parseScalar(const std::string& valStr) {
    // Versuche als JSON zu parsen
    std::string clean = valStr;
    // Strip outer single quotes (from SQL literal)
    if (clean.size() >= 2 && clean.front() == '\'' && clean.back() == '\'')
        clean = clean.substr(1, clean.size() - 2);
    try {
        return parse(clean);
    } catch (...) {
        // Fallback: als String
        JsonValue v;
        v.type = JsonValue::Type::STRING;
        v.str  = clean;
        return v;
    }
}

inline std::string set(const std::string& jsonStr,
                        const std::string& path,
                        const std::string& newValStr) {
    try {
        JsonValue root = parse(jsonStr);
        JsonValue newVal = parseScalar(newValStr);
        setValueAtPath(root, path, newVal);
        return serializeValue(root);
    } catch (...) { return jsonStr; }
}

// ── JSON_KEYS ─────────────────────────────────────────────────────────────────
// Gibt Keys eines JSON-Objekts als JSON-Array zurück: ["key1","key2",...]

inline std::string keys(const std::string& jsonStr) {
    try {
        JsonValue root = parse(jsonStr);
        if (root.type != JsonValue::Type::OBJECT) return "NULL";
        std::string result = "[";
        bool first = true;
        for (const auto& [k, _] : root.obj) {
            if (!first) result += ",";
            first = false;
            result += "\"" + escapeJsonString(k) + "\"";
        }
        return result + "]";
    } catch (...) { return "NULL"; }
}

// ── JSON_LENGTH ───────────────────────────────────────────────────────────────
// Gibt Anzahl der Keys (OBJECT) oder Elemente (ARRAY) zurück

inline std::string length(const std::string& jsonStr) {
    try {
        JsonValue root = parse(jsonStr);
        if (root.type == JsonValue::Type::OBJECT)
            return std::to_string(root.obj.size());
        if (root.type == JsonValue::Type::ARRAY)
            return std::to_string(root.arr.size());
        if (root.type == JsonValue::Type::STRING)
            return std::to_string(root.str.size());
        return "1";
    } catch (...) { return "NULL"; }
}

// ── JSON_CONTAINS ─────────────────────────────────────────────────────────────
// JSON_CONTAINS(json, val, path) → 1 oder 0
// Prüft ob der Wert an path gleich val ist

inline std::string contains(const std::string& jsonStr,
                              const std::string& searchVal,
                              const std::string& path = "$") {
    try {
        JsonValue root = parse(jsonStr);
        const JsonValue* node = navigatePath(root, path);
        if (!node) return "0";

        // searchVal als string (SQL-Wert, evtl. single-quoted)
        std::string sv = searchVal;
        if (sv.size() >= 2 && sv.front() == '\'' && sv.back() == '\'')
            sv = sv.substr(1, sv.size() - 2);

        // Vergleich mit dem Knoteninhalt
        switch (node->type) {
            case JsonValue::Type::STRING:  return (node->str == sv)                 ? "1" : "0";
            case JsonValue::Type::NUMBER:  return (node->str == sv)                 ? "1" : "0";
            case JsonValue::Type::BOOL:    return ((node->b ? "true" : "false") == sv) ? "1" : "0";
            case JsonValue::Type::NULLVAL: return (sv == "null" || sv == "NULL")    ? "1" : "0";
            case JsonValue::Type::ARRAY: {
                // Prüfe ob Element im Array
                for (const auto& elem : node->arr) {
                    std::string es;
                    if (elem.type == JsonValue::Type::STRING) es = elem.str;
                    else if (elem.type == JsonValue::Type::NUMBER) es = elem.str;
                    else if (elem.type == JsonValue::Type::BOOL) es = elem.b ? "true" : "false";
                    if (es == sv) return "1";
                }
                return "0";
            }
            default: return "0";
        }
    } catch (...) { return "0"; }
}

// ── JSON_TYPE ─────────────────────────────────────────────────────────────────
// Gibt den JSON-Typ zurück: OBJECT, ARRAY, STRING, INTEGER, DOUBLE, BOOLEAN, NULL

inline std::string type(const std::string& jsonStr) {
    try {
        JsonValue root = parse(jsonStr);
        switch (root.type) {
            case JsonValue::Type::OBJECT:  return "OBJECT";
            case JsonValue::Type::ARRAY:   return "ARRAY";
            case JsonValue::Type::STRING:  return "STRING";
            case JsonValue::Type::NUMBER: {
                // Unterscheide INTEGER vs. DOUBLE
                const std::string& n = root.str;
                bool isFloat = (n.find('.') != std::string::npos ||
                                n.find('e') != std::string::npos ||
                                n.find('E') != std::string::npos);
                return isFloat ? "DOUBLE" : "INTEGER";
            }
            case JsonValue::Type::BOOL:    return "BOOLEAN";
            case JsonValue::Type::NULLVAL: return "NULL";
        }
    } catch (...) {}
    return "NULL";
}

} // namespace jsonutils
} // namespace milansql
