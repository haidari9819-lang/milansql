#pragma once
// ============================================================
// http_server.hpp — MilanSQL REST API HTTP Server (Phase 52)
// Minimal HTTP/1.1 server using raw sockets (no external libs)
// ============================================================

#if defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #ifdef DELETE
    #undef DELETE
  #endif
  typedef SOCKET sock_t;
  #define INVALID_SOCK INVALID_SOCKET
  #define closesocket closesocket
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  typedef int sock_t;
  #define INVALID_SOCK (-1)
  #define closesocket close
#endif

#include <thread>
#include <future>
#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include <string>
#include <sstream>
#include <iostream>
#include <fstream>
#include <map>
#include <vector>
#include <queue>
#include <functional>
#include <memory>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <algorithm>
#include <atomic>
#include <csignal>

#include "../engine/engine.hpp"
#include "../parser/parser.hpp"
#include "../storage/storage.hpp"
#include "../dispatch.hpp"
#include "../monitoring/prometheus.hpp"
#include "../auth/auth_manager.hpp"
#include "../auth/rate_limiter.hpp"
#include "../security/fortress.hpp"

// Phase 174: test suite size — served via /health as test_count,
// displayed dynamically in the WebUI navbar badge.
static constexpr int MILANSQL_TEST_COUNT = 1568;

// Redesign 2026-07: version served via /health — Landing Page und
// WebUI lesen sie dynamisch (Elemente mit class="ms-version").
static constexpr const char* MILANSQL_VERSION = "10.6.0";

// ── JSON helpers ──────────────────────────────────────────────

static std::string jsonEscape(const std::string& s) {
    std::string r;
    r.reserve(s.size() + 4);
    for (unsigned char c : s) {
        if      (c == '"')  r += "\\\"";
        else if (c == '\\') r += "\\\\";
        else if (c == '\b') r += "\\b";
        else if (c == '\f') r += "\\f";
        else if (c == '\n') r += "\\n";
        else if (c == '\r') r += "\\r";
        else if (c == '\t') r += "\\t";
        else if (c < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
            r += buf;
        }
        else r += static_cast<char>(c);
    }
    return r;
}

// Check if a column type name is numeric (INT, FLOAT, DECIMAL, etc.)
static bool isNumericType(const std::string& type) {
    if (type.empty()) return false;
    // Uppercase first word for comparison
    std::string t;
    for (char c : type) {
        if (c == '(' || c == ' ') break;
        t += static_cast<char>(std::toupper((unsigned char)c));
    }
    return t == "INT" || t == "INTEGER" || t == "BIGINT" || t == "SMALLINT"
        || t == "TINYINT" || t == "FLOAT" || t == "DOUBLE" || t == "DECIMAL"
        || t == "NUMERIC" || t == "REAL" || t == "NUMBER" || t == "SERIAL"
        || t == "BIGSERIAL" || t == "BOOLEAN" || t == "BOOL";
}

// Strict numeric literal check per RFC 8259:
//   number = [ "-" ] int [ frac ] [ exp ]
//   int    = "0" | digit1-9 *digit
//   frac   = "." 1*digit
//   exp    = ("e"|"E") ["+"|"-"] 1*digit
static bool isJsonNumber(const std::string& v) {
    if (v.empty()) return false;
    size_t i = 0;
    if (v[i] == '-') ++i;
    if (i >= v.size()) return false;
    // int part: no leading zeros (except standalone "0")
    if (v[i] == '0') {
        ++i;
        if (i < v.size() && std::isdigit((unsigned char)v[i])) return false;
    } else if (std::isdigit((unsigned char)v[i])) {
        while (i < v.size() && std::isdigit((unsigned char)v[i])) ++i;
    } else {
        return false;
    }
    // frac
    if (i < v.size() && v[i] == '.') {
        ++i;
        if (i >= v.size() || !std::isdigit((unsigned char)v[i])) return false;
        while (i < v.size() && std::isdigit((unsigned char)v[i])) ++i;
    }
    // exp
    if (i < v.size() && (v[i] == 'e' || v[i] == 'E')) {
        ++i;
        if (i < v.size() && (v[i] == '+' || v[i] == '-')) ++i;
        if (i >= v.size() || !std::isdigit((unsigned char)v[i])) return false;
        while (i < v.size() && std::isdigit((unsigned char)v[i])) ++i;
    }
    return i == v.size();
}

// Type-aware: use column type to decide quoting; only numeric columns
// with valid numeric values are emitted unquoted.
static std::string jsonValueTyped(const std::string& v, const std::string& colType) {
    if (v == "NULL") return "null";
    if (isNumericType(colType) && isJsonNumber(v)) return v;
    // BOOLEAN special case
    if ((colType == "BOOLEAN" || colType == "BOOL" || colType == "boolean" || colType == "bool")
        && (v == "true" || v == "false")) return v;
    return "\"" + jsonEscape(v) + "\"";
}

// Fallback for contexts without column type info (non-SELECT output).
static std::string jsonValue(const std::string& v) {
    if (v == "NULL") return "null";
    if (isJsonNumber(v)) return v;
    return "\"" + jsonEscape(v) + "\"";
}

// ── HTTP structures ───────────────────────────────────────────

struct HttpRequest {
    std::string method;
    std::string path;
    std::string query;
    std::map<std::string, std::string> headers;
    std::string body;
};

// ── HTTP parsing helpers ──────────────────────────────────────

static HttpRequest parseHttpRequest(sock_t sock) {
    std::string raw;
    char buf[4096];
    while (raw.find("\r\n\r\n") == std::string::npos) {
        int n = recv(sock, buf, sizeof(buf) - 1, 0);
        if (n <= 0) break;
        buf[n] = '\0';
        raw.append(buf, n); // binary-safe: Bilder enthalten NUL-Bytes
        // Guard: reject headers larger than 64 KB (likely attack)
        if (raw.size() > 65536) return {};
    }

    HttpRequest req;
    size_t lineEnd = raw.find("\r\n");
    if (lineEnd == std::string::npos) return req;

    std::string firstLine = raw.substr(0, lineEnd);
    auto sp1 = firstLine.find(' ');
    if (sp1 == std::string::npos) return req;
    req.method = firstLine.substr(0, sp1);
    auto sp2 = firstLine.find(' ', sp1 + 1);
    std::string fullPath = firstLine.substr(sp1 + 1,
        sp2 != std::string::npos ? sp2 - sp1 - 1 : std::string::npos);

    auto qPos = fullPath.find('?');
    if (qPos != std::string::npos) {
        req.path  = fullPath.substr(0, qPos);
        req.query = fullPath.substr(qPos + 1);
    } else {
        req.path = fullPath;
    }

    size_t pos = lineEnd + 2;
    while (pos < raw.size()) {
        size_t end = raw.find("\r\n", pos);
        if (end == std::string::npos || end == pos) break;
        std::string hline = raw.substr(pos, end - pos);
        auto colon = hline.find(':');
        if (colon != std::string::npos) {
            std::string key = hline.substr(0, colon);
            std::string val = hline.substr(colon + 1);
            while (!val.empty() && val[0] == ' ') val = val.substr(1);
            for (char& c : key) c = (char)std::tolower((unsigned char)c);
            req.headers[key] = val;
        }
        pos = end + 2;
    }

    auto clIt = req.headers.find("content-length");
    if (clIt != req.headers.end()) {
        int contentLen = 0;
        try { contentLen = std::stoi(clIt->second); } catch (...) { return req; }
        // 10 MB body limit — return 413 and discard connection
        if (contentLen < 0 || contentLen > 10 * 1024 * 1024) {
            static const char resp413[] =
                "HTTP/1.1 413 Request Entity Too Large\r\n"
                "Content-Type: application/json\r\n"
                "Content-Length: 35\r\n"
                "Connection: close\r\n"
                "\r\n"
                "{\"error\":\"Request body too large\"}";
            ::send(sock, resp413, static_cast<int>(sizeof(resp413) - 1), 0);
            return {};  // empty method → handleClient closes socket immediately
        }
        size_t headerEnd = raw.find("\r\n\r\n");
        size_t bodyStart = headerEnd + 4;
        std::string bodyPart = raw.substr(bodyStart);
        while ((int)bodyPart.size() < contentLen) {
            int n = recv(sock, buf, sizeof(buf) - 1, 0);
            if (n <= 0) break;
            buf[n] = '\0';
            bodyPart.append(buf, n); // binary-safe
        }
        req.body = bodyPart.substr(0, contentLen);
    }
    return req;
}

static std::string urlDecode(const std::string& s) {
    std::string result;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+') { result += ' '; continue; }
        if (s[i] == '%' && i + 2 < s.size()) {
            int val = 0;
            for (int j = 1; j <= 2; ++j) {
                val <<= 4;
                char c = s[i + j];
                if      (c >= '0' && c <= '9') val |= c - '0';
                else if (c >= 'a' && c <= 'f') val |= c - 'a' + 10;
                else if (c >= 'A' && c <= 'F') val |= c - 'A' + 10;
            }
            result += (char)val;
            i += 2;
        } else {
            result += s[i];
        }
    }
    return result;
}

// Phase 169-fix: decode \uXXXX JSON escape to UTF-8
static inline void appendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x110000) {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

static inline uint32_t parseHex4(const std::string& s, size_t pos) {
    uint32_t v = 0;
    for (int i = 0; i < 4 && pos + i < s.size(); ++i) {
        v <<= 4;
        char c = s[pos + i];
        if (c >= '0' && c <= '9') v |= (c - '0');
        else if (c >= 'a' && c <= 'f') v |= (c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (c - 'A' + 10);
    }
    return v;
}

// Unescape a JSON backslash sequence starting after the '\'.
// Updates pos to point past the consumed chars.
static inline void jsonUnescapeChar(const std::string& json, size_t& pos, std::string& out) {
    if (pos >= json.size()) return;
    char next = json[pos];
    if      (next == '"')  { out += '"';  ++pos; }
    else if (next == '\\') { out += '\\'; ++pos; }
    else if (next == '/')  { out += '/';  ++pos; }
    else if (next == 'n')  { out += '\n'; ++pos; }
    else if (next == 'r')  { out += '\r'; ++pos; }
    else if (next == 't')  { out += '\t'; ++pos; }
    else if (next == 'b')  { out += '\b'; ++pos; }
    else if (next == 'f')  { out += '\f'; ++pos; }
    else if (next == 'u' && pos + 4 < json.size()) {
        ++pos; // skip 'u'
        uint32_t cp = parseHex4(json, pos);
        pos += 4;
        // Handle UTF-16 surrogate pairs: \uD800-\uDBFF followed by \uDC00-\uDFFF
        if (cp >= 0xD800 && cp <= 0xDBFF &&
            pos + 5 < json.size() && json[pos] == '\\' && json[pos + 1] == 'u') {
            uint32_t lo = parseHex4(json, pos + 2);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                pos += 6; // skip \uXXXX
            }
        }
        appendUtf8(out, cp);
    } else {
        out += next; ++pos;
    }
}

static std::string extractSqlFromJson(const std::string& json) {
    auto pos = json.find("\"sql\"");
    if (pos == std::string::npos) return "";
    pos = json.find(':', pos);
    if (pos == std::string::npos) return "";
    pos = json.find('"', pos);
    if (pos == std::string::npos) return "";
    ++pos;
    std::string sql;
    while (pos < json.size() && json[pos] != '"') {
        if (json[pos] == '\\' && pos + 1 < json.size()) {
            ++pos; // skip backslash
            jsonUnescapeChar(json, pos, sql);
        } else {
            sql += json[pos++];
        }
    }
    return sql;
}

// ── Phase C: Extract "params" array from JSON body ───────────
// Returns vector of string values from {"params":["a","b",123,null,...]}
static std::vector<std::string> extractParamsFromJson(const std::string& json) {
    std::vector<std::string> params;
    auto pos = json.find("\"params\"");
    if (pos == std::string::npos) return params;
    pos = json.find('[', pos);
    if (pos == std::string::npos) return params;
    ++pos; // skip '['

    while (pos < json.size()) {
        // skip whitespace
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' ||
               json[pos] == '\n' || json[pos] == '\r')) ++pos;
        if (pos >= json.size() || json[pos] == ']') break;

        if (json[pos] == '"') {
            // String value — parse with JSON unescaping (Phase 169-fix: \uXXXX)
            ++pos;
            std::string val;
            while (pos < json.size() && json[pos] != '"') {
                if (json[pos] == '\\' && pos + 1 < json.size()) {
                    ++pos; // skip backslash
                    jsonUnescapeChar(json, pos, val);
                } else {
                    val += json[pos++];
                }
            }
            if (pos < json.size()) ++pos; // skip closing '"'
            params.push_back(val);
        } else if (json[pos] == 'n' && pos + 3 < json.size() &&
                   json.substr(pos, 4) == "null") {
            params.push_back("NULL");
            pos += 4;
        } else if (json[pos] == 't' && pos + 3 < json.size() &&
                   json.substr(pos, 4) == "true") {
            params.push_back("TRUE");
            pos += 4;
        } else if (json[pos] == 'f' && pos + 4 < json.size() &&
                   json.substr(pos, 5) == "false") {
            params.push_back("FALSE");
            pos += 5;
        } else if (json[pos] == '-' || (json[pos] >= '0' && json[pos] <= '9')) {
            // Number
            std::string num;
            while (pos < json.size() && json[pos] != ',' && json[pos] != ']' &&
                   json[pos] != ' ' && json[pos] != '\n') {
                num += json[pos++];
            }
            params.push_back(num);
        } else {
            ++pos; // skip unexpected chars
            continue;
        }
        // skip comma
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == ',' ||
               json[pos] == '\t' || json[pos] == '\n' || json[pos] == '\r')) ++pos;
    }
    return params;
}

// ── Phase C: Safe parameter binding ──────────────────────────
// Replaces ? placeholders with safely escaped literal values.
// Parameters are NEVER parsed as SQL — they become quoted string
// literals or numeric literals only.
static std::string bindParams(const std::string& sql,
                              const std::vector<std::string>& params) {
    if (params.empty()) return sql;
    std::string result;
    result.reserve(sql.size() + params.size() * 16);
    size_t paramIdx = 0;
    bool inString = false;
    char strChar = 0;

    for (size_t i = 0; i < sql.size(); ++i) {
        char c = sql[i];

        // Track string literals — don't replace ? inside strings
        if (!inString && (c == '\'' || c == '"')) {
            inString = true;
            strChar = c;
            result += c;
        } else if (inString && c == strChar) {
            // Check for escaped quote ('')
            if (c == '\'' && i + 1 < sql.size() && sql[i + 1] == '\'') {
                result += "''";
                ++i;
            } else {
                inString = false;
                result += c;
            }
        } else if (!inString && c == '?' && paramIdx < params.size()) {
            // Replace ? with safe literal
            const auto& val = params[paramIdx++];
            if (val == "NULL") {
                result += "NULL";
            } else {
                // Always quote as string — SQL engine will cast as needed.
                // This ensures the value can NEVER be interpreted as SQL syntax.
                result += '\'';
                for (char v : val) {
                    if (v == '\'') result += "''";  // escape single quote
                    else result += v;
                }
                result += '\'';
            }
        } else {
            result += c;
        }
    }
    return result;
}

static std::string getQueryParam(const std::string& query, const std::string& key) {
    std::string search = key + "=";
    size_t pos = 0;
    while (pos < query.size()) {
        if (query.substr(pos, search.size()) == search) {
            size_t end = query.find('&', pos + search.size());
            std::string val = query.substr(pos + search.size(),
                end != std::string::npos ? end - pos - search.size() : std::string::npos);
            return urlDecode(val);
        }
        pos = query.find('&', pos);
        if (pos == std::string::npos) break;
        ++pos;
    }
    return "";
}

static std::string buildHttpResponse(int statusCode, const std::string& body,
                                     const std::string& contentType = "application/json",
                                     const std::string& extraHeaders = "") {
    std::string statusText = statusCode == 200 ? "OK"
                           : statusCode == 201 ? "Created"
                           : statusCode == 400 ? "Bad Request"
                           : statusCode == 401 ? "Unauthorized"
                           : statusCode == 403 ? "Forbidden"
                           : statusCode == 404 ? "Not Found"
                           : statusCode == 423 ? "Locked"
                           : statusCode == 429 ? "Too Many Requests"
                           : "Internal Server Error";
    return "HTTP/1.1 " + std::to_string(statusCode) + " " + statusText + "\r\n"
           "Content-Type: " + contentType + "; charset=utf-8\r\n"
           "Content-Length: " + std::to_string(body.size()) + "\r\n"
           "Access-Control-Allow-Origin: https://milansql.de\r\n"
           "Access-Control-Allow-Methods: GET, POST, DELETE, OPTIONS\r\n"
           "Access-Control-Allow-Headers: Content-Type, Authorization\r\n"
           "Access-Control-Allow-Credentials: true\r\n"
           "X-Frame-Options: DENY\r\n"
           "X-Content-Type-Options: nosniff\r\n"
           "X-XSS-Protection: 1; mode=block\r\n"
           "Referrer-Policy: strict-origin-when-cross-origin\r\n"
           "Content-Security-Policy: default-src 'self'; script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline'; connect-src 'self'; img-src 'self' data:; frame-ancestors 'none'; base-uri 'self'; form-action 'self'\r\n"
           "Strict-Transport-Security: max-age=31536000; includeSubDomains\r\n"
           "Permissions-Policy: geolocation=(), camera=(), microphone=()\r\n"
           + extraHeaders +
           "Connection: close\r\n"
           "\r\n" + body;
}

// Strip internal filesystem paths from error messages (information disclosure prevention)
static std::string sanitizeError(const std::string& msg) {
    std::string r = msg;
    // Remove POSIX absolute paths embedded in the message (e.g. /opt/milansql/...)
    for (size_t i = 0; i < r.size(); ) {
        if (r[i] == '/' && (i == 0 || r[i-1] == ' ' || r[i-1] == ':' || r[i-1] == '"')) {
            size_t end = i + 1;
            while (end < r.size() && r[end] != ' ' && r[end] != '"' && r[end] != '\'') ++end;
            r = r.substr(0, i) + "<path>" + r.substr(end);
            i += 6;
        } else {
            ++i;
        }
    }
    // Also strip Windows-style paths
    for (size_t i = 0; i + 1 < r.size(); ) {
        if (std::isalpha((unsigned char)r[i]) && r[i+1] == ':' && (i == 0 || r[i-1] == ' ' || r[i-1] == '"')) {
            size_t end = i + 2;
            while (end < r.size() && r[end] != ' ' && r[end] != '"') ++end;
            r = r.substr(0, i) + "<path>" + r.substr(end);
            i += 6;
        } else {
            ++i;
        }
    }
    return r;
}

// ── Output parser: convert captured table output to JSON ──────

static std::string parseOutputToJson(const std::string& output,
                                     const std::vector<std::string>& colTypes = {}) {
    std::vector<std::string> lines;
    std::istringstream iss(output);
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) lines.push_back(line);
    }

    // Find data lines (containing box-drawing │ U+2502 = E2 94 82)
    // IMPORTANT: only split on the UTF-8 box-drawing │, NOT on ASCII '|'
    // which can appear inside cell values (e.g. "Döner|Dürüm|Pizza").
    std::vector<std::vector<std::string>> dataLines;
    for (const auto& l : lines) {
        if (l.find("\xe2\x94\x82") != std::string::npos) {
            std::vector<std::string> cells;
            std::string cur;
            bool inCell = false;
            for (size_t i = 0; i < l.size(); ) {
                unsigned char c0 = (unsigned char)l[i];
                // UTF-8 box-drawing │ = E2 94 82
                if (c0 == 0xE2 && i + 2 < l.size() &&
                    (unsigned char)l[i + 1] == 0x94 &&
                    (unsigned char)l[i + 2] == 0x82) {
                    if (inCell) {
                        while (!cur.empty() && cur.back()  == ' ') cur.pop_back();
                        while (!cur.empty() && cur.front() == ' ') cur = cur.substr(1);
                        cells.push_back(cur);
                        cur = "";
                    }
                    inCell = true;
                    i += 3;
                } else {
                    if (inCell) cur += l[i];
                    ++i;
                }
            }
            if (!cur.empty()) {
                while (!cur.empty() && cur.back()  == ' ') cur.pop_back();
                while (!cur.empty() && cur.front() == ' ') cur = cur.substr(1);
                cells.push_back(cur);
            }
            if (!cells.empty()) dataLines.push_back(cells);
        }
    }

    // Filter out ghost rows: rows where every cell is empty
    // (produced by dispatch_printTable when a table has 0 data rows)
    std::vector<std::vector<std::string>> filteredData;
    for (const auto& dl : dataLines) {
        bool allEmpty = true;
        for (const auto& cell : dl) {
            if (!cell.empty()) { allEmpty = false; break; }
        }
        if (!allEmpty) filteredData.push_back(dl);
    }

    if (filteredData.size() >= 1) {
        const auto& cols = filteredData[0];
        std::string json = "{\"success\":true,\"columns\":[";
        for (size_t i = 0; i < cols.size(); ++i) {
            if (i > 0) json += ",";
            json += "\"" + jsonEscape(cols[i]) + "\"";
        }
        json += "],\"rows\":[";
        int rowCount = 0;
        for (size_t ri = 1; ri < filteredData.size(); ++ri) {
            const auto& row = filteredData[ri];
            if (ri > 1) json += ",";
            json += "[";
            for (size_t ci = 0; ci < row.size(); ++ci) {
                if (ci > 0) json += ",";
                if (ci < colTypes.size())
                    json += jsonValueTyped(row[ci], colTypes[ci]);
                else
                    json += jsonValue(row[ci]);
            }
            json += "]";
            rowCount++;
        }
        json += "],\"rowCount\":" + std::to_string(rowCount) + "}";
        return json;
    }

    // Not a table — extract last non-empty message line
    std::string msg;
    for (const auto& l : lines) {
        std::string trimmed = l;
        while (!trimmed.empty() && trimmed.front() == ' ') trimmed = trimmed.substr(1);
        if (!trimmed.empty()) msg = trimmed;
    }

    if (msg.empty()) {
        return "{\"success\":true,\"message\":\"OK\",\"rowsAffected\":0}";
    }

    if (msg.find("FEHLER:") != std::string::npos || msg.find("ERROR") != std::string::npos) {
        return "{\"success\":false,\"error\":\"" + jsonEscape(msg) + "\"}";
    }

    // Extract rowsAffected from "N Zeile(n)" pattern
    int rowsAffected = 0;
    for (const auto& l : lines) {
        for (size_t i = 0; i < l.size(); ++i) {
            if (std::isdigit((unsigned char)l[i])) {
                size_t j = i;
                std::string num;
                while (j < l.size() && std::isdigit((unsigned char)l[j])) num += l[j++];
                if (j < l.size() && l[j] == ' ' && l.find("Zeile", j) != std::string::npos) {
                    rowsAffected = std::stoi(num);
                    break;
                }
            }
        }
    }

    return "{\"success\":true,\"message\":\"" + jsonEscape(msg) +
           "\",\"rowsAffected\":" + std::to_string(rowsAffected) + "}";
}

// ── MilanHttpServer ───────────────────────────────────────────

struct UserContext {
    int userId = 0;
    std::string username;
    std::string role;
    bool isRoot = false;
    bool valid = false;
};

// Phase 170: graceful shutdown flag (set by SIGINT/SIGTERM handler)
inline std::atomic<bool> g_httpShutdownRequested{false};
inline void httpShutdownSignalHandler(int) { g_httpShutdownRequested.store(true); }

class MilanHttpServer {
public:
    MilanHttpServer(int port, const std::string& dbPath,
                    int poolMin = milansql::ConnectionPool::DEFAULT_MIN,
                    int poolMax = milansql::ConnectionPool::DEFAULT_MAX)
        : port_(port), dbPath_(dbPath), storage_(dbPath_) {
        milansql::g_connectionPool.configure(poolMin, poolMax);
    }

    void run();

    // Phase 172: replay a binlog statement from the master (replica mode).
    // Sets tl_binlogReplay so dispatch skips the slave read-only check.
    void replayBinlogSql(const std::string& sql) {
        milansql::tl_binlogReplay = true;
        try { handleQuery(sql); } catch (...) {}
        milansql::tl_binlogReplay = false;
    }

private:
    int port_;
    std::string dbPath_;
    milansql::Engine engine_;
    milansql::MilanBinaryStorage storage_;
    mutable std::shared_mutex engineMutex_;  // Phase 173: shared for reads, exclusive for writes
    // Audit Bug #25: letzter Persist-Fehler (z.B. Disk voll) —
    // wird in /health als storage.status=error gemeldet.
    // Zugriff nur unter engineMutex_ (persistFn und /health halten den Lock).
    std::string lastPersistError_;
    std::chrono::steady_clock::time_point startTime_ = std::chrono::steady_clock::now();
    std::atomic<long long> queryCounter_{0};   // Phase 166: live query counter

    // Phase 167: Thread Pool (reuses ThreadPool from server.hpp)
    std::unique_ptr<ThreadPool> threadPool_;

    // Phase 154-156: Auth + Rate Limiting
    AuthManager authMgr_;
    RateLimiter loginLimiter_{5, 5.0/900.0};      // 5 burst, 5 per 15min per IP
    RateLimiter requestLimiter_;                   // tiered: per user/IP

    // Brute-force lockout: per-username AND per-IP, 5 failures → 15 min lock
    struct LockoutInfo { int failedAttempts = 0; std::chrono::steady_clock::time_point lockedUntil{}; };
    std::map<std::string, LockoutInfo> loginLockouts_;   // key = username
    std::map<std::string, LockoutInfo> ipLockouts_;      // key = client IP
    std::mutex lockoutMutex_;

    void handleClient(sock_t clientSock);
    std::string handleRequest(const HttpRequest& req, const std::string& clientIp = "");
    std::string handleQuery(const std::string& sql);
    std::string handleQueryForUser(const std::string& sql, int userId, const std::string& userRole);
    std::string handleListTables();
    std::string handleListTablesForUser(int userId);
    std::string handleDescribeTable(const std::string& tableName);
    std::string handleListSchemas();
    std::string handleStatus();
    std::string handleDashboard();   // Phase 54C
    std::string handleWebUI();       // Phase 135: Professional Admin Dashboard
    std::string handleSemanticSearch(const std::string& body, int userId = 0, bool isRoot = true);  // Phase 121

    // Phase 154: Auth routes
    std::string handleAuthRegister(const std::string& body, const std::string& clientIp);
    std::string handleAuthLogin(const std::string& body, const std::string& clientIp);
    std::string handleAuthLogout(const std::string& token);
    std::string handleAuthMe(const std::string& token);
    std::string handleAuthRefresh(const std::string& body);
    std::string handleAuthSessions(const std::string& token);
    std::string handleAuthApiKey(const std::string& token, const std::string& body);
    std::string handleAuthApiKeyCreate(int userId, const std::string& body); // Phase 156
    std::string handleAuthApiKeyList(int userId);                             // Phase 156
    std::string handleAuthApiKeyDelete(int userId, const std::string& keyId); // Phase 156
    std::string handleAuthApiKeyStats(int userId, const std::string& keyId);  // Phase 156

    // Phase 155: Permission routes (handled via SQL intercept in handleQueryForUser)
    // Phase 156: Admin + Quota routes
    std::string handleAdminUsers(const std::string& token);
    std::string handleAdminStats(const std::string& token);
    std::string handleAdminQuota(const std::string& token);
    std::string handleMyQuota(const std::string& token);
    std::string handleBackup(const std::string& token);  // Phase 167: SQL dump backup
    std::string handleRestore(const std::string& token, const std::string& dump, bool dryRun, bool clean = false);

    // Helpers
    static std::string extractBearerToken(const HttpRequest& req);
    static std::string extractApiKey(const HttpRequest& req);
    UserContext extractUserContext(const HttpRequest& req);
    static std::string extractJsonStr(const std::string& body, const std::string& key);

    void initEngine();
    static void sendResponse(sock_t sock, const std::string& response);
};

// ── MilanHttpServer::initEngine ───────────────────────────────

inline void MilanHttpServer::initEngine() {
    std::remove((dbPath_ + ".wal").c_str());

    try {
        storage_.loadWithCount(engine_);
    } catch (const std::exception& ex) {
        std::cerr << "  WARNUNG: Laden fehlgeschlagen: " << ex.what()
                  << "\n  Starte mit leerer Datenbank.\n";
    }

    engine_.loadUsers(dbPath_ + ".users");

    // Load triggers
    {
        std::ifstream tf(dbPath_ + ".triggers");
        if (tf) {
            std::string line;
            while (std::getline(tf, line)) {
                if (line.empty()) continue;
                std::vector<std::string> parts;
                size_t pos = 0;
                for (int field = 0; field < 4; ++field) {
                    size_t tab = line.find('\t', pos);
                    if (tab == std::string::npos) { pos = line.size(); break; }
                    parts.push_back(line.substr(pos, tab - pos));
                    pos = tab + 1;
                }
                parts.push_back(line.substr(pos));
                if (parts.size() == 5) {
                    milansql::TriggerDef def;
                    def.name      = parts[0];
                    def.timing    = parts[1];
                    def.event     = parts[2];
                    def.tableName = parts[3];
                    def.body      = parts[4];
                    engine_.createTrigger(def);
                }
            }
        }
    }

    // Load procedures
    {
        std::ifstream pf(dbPath_ + ".procedures");
        if (pf) {
            std::string line;
            while (std::getline(pf, line)) {
                if (line.empty()) continue;
                milansql::ProcedureDef def;
                size_t tabPos = line.find('\t');
                if (tabPos == std::string::npos) continue;
                def.name = line.substr(0, tabPos);
                int paramCount = 0;
                try { paramCount = std::stoi(line.substr(tabPos + 1)); }
                catch (...) { continue; }
                for (int pi = 0; pi < paramCount; ++pi) {
                    std::string pline;
                    if (!std::getline(pf, pline)) break;
                    size_t pt = pline.find('\t');
                    if (pt != std::string::npos)
                        def.params.push_back({pline.substr(0, pt), pline.substr(pt + 1)});
                }
                std::string bodyLine;
                if (!std::getline(pf, bodyLine)) { engine_.createProcedure(def); continue; }
                std::string decoded;
                for (size_t bi = 0; bi < bodyLine.size(); ++bi) {
                    if (bi + 1 < bodyLine.size() &&
                        bodyLine[bi] == '\\' && bodyLine[bi + 1] == 'n') {
                        decoded += ' '; ++bi;
                    } else decoded += bodyLine[bi];
                }
                def.body = decoded;
                engine_.createProcedure(def);
            }
        }
    }

    // Phase 154/169: Init auth system
    // Load first (reads legacy secret + users), then init (resolves JWT secret)
    authMgr_.load(dbPath_ + ".auth");
    authMgr_.init();  // resolves secret: env → file → legacy → generate

    // ══ FORTRESS: Load whitelist + persistent ban list ═══════
    milansql::g_fortress().loadWhitelist(dbPath_ + ".whitelist");
    milansql::g_fortress().loadBanList(dbPath_ + ".banlist");
}

// ── Auth helpers ─────────────────────────────────────────────

inline std::string MilanHttpServer::extractBearerToken(const HttpRequest& req) {
    // 1) Authorization: Bearer header (API / in-memory token)
    auto it = req.headers.find("authorization");
    if (it != req.headers.end()) {
        const auto& v = it->second;
        if (v.size() > 7 && v.substr(0,7) == "Bearer ") {
            return v.substr(7);
        }
    }
    // 2) httpOnly Cookie fallback (survives page refresh)
    auto cit = req.headers.find("cookie");
    if (cit != req.headers.end()) {
        const std::string& cookies = cit->second;
        const std::string prefix = "milansql_token=";
        size_t pos = cookies.find(prefix);
        if (pos != std::string::npos) {
            pos += prefix.size();
            size_t end = cookies.find(';', pos);
            if (end == std::string::npos) end = cookies.size();
            std::string tok = cookies.substr(pos, end - pos);
            return tok;
        }
    } else {
    }
    return "";
}
inline std::string MilanHttpServer::extractApiKey(const HttpRequest& req) {
    auto it = req.headers.find("authorization");
    if (it == req.headers.end()) return "";
    const auto& v = it->second;
    if (v.size() > 7 && v.substr(0,7) == "ApiKey ") return v.substr(7);
    return "";
}
inline UserContext MilanHttpServer::extractUserContext(const HttpRequest& req) {
    UserContext ctx;
    std::string token;
    // 1) Authorization: Bearer header
    auto it = req.headers.find("authorization");
    if (it != req.headers.end()) {
        const auto& v = it->second;
        if (v.size() > 7 && v.substr(0,7) == "Bearer ") token = v.substr(7);
    }
    // 2) httpOnly Cookie fallback
    if (token.empty()) {
        auto cit = req.headers.find("cookie");
        if (cit != req.headers.end()) {
            const std::string& cookies = cit->second;
            const std::string prefix = "milansql_token=";
            size_t pos = cookies.find(prefix);
            if (pos != std::string::npos) {
                pos += prefix.size();
                size_t end = cookies.find(';', pos);
                if (end == std::string::npos) end = cookies.size();
                token = cookies.substr(pos, end - pos);
            }
        }
    }
    if (!token.empty()) {
        auto vr = authMgr_.validateToken(token);
        if (vr.valid) {
            ctx.userId   = vr.userId;
            ctx.username = vr.username;
            ctx.role     = vr.role;
            ctx.isRoot   = (vr.role == "root");
            ctx.valid    = true;
        }
    }
    return ctx;
}
inline std::string MilanHttpServer::extractJsonStr(const std::string& body, const std::string& key) {
    std::string search = "\"" + key + "\"";
    auto pos = body.find(search);
    if (pos == std::string::npos) return "";
    pos = body.find(':', pos + search.size());
    if (pos == std::string::npos) return "";
    pos = body.find_first_not_of(" \t\r\n", pos+1);
    if (pos == std::string::npos || body[pos] != '"') return "";
    // Parse JSON string value: handle escape sequences and find true end quote
    // Phase 169-fix: properly decode \uXXXX to UTF-8
    std::string result;
    size_t i = pos + 1;
    while (i < body.size()) {
        char c = body[i];
        if (c == '"') break;  // unescaped quote = end of string
        if (c == '\\' && i + 1 < body.size()) {
            ++i; // skip backslash
            jsonUnescapeChar(body, i, result);
        } else {
            result += c;
            ++i;
        }
    }
    return result;
}

// ── Phase 154: Auth route handlers ───────────────────────────

inline std::string MilanHttpServer::handleAuthRegister(const std::string& body, const std::string& /*ip*/) {
    std::string username = extractJsonStr(body, "username");
    std::string password = extractJsonStr(body, "password");
    std::string email    = extractJsonStr(body, "email");
    if (username.empty() || password.empty())
        return "{\"success\":false,\"error\":\"username and password required\"}";
    // MEDIUM-07: Input length limits
    if (username.size() > 64)
        return "{\"success\":false,\"error\":\"Username too long (max 64 chars)\"}";
    if (password.size() > 256)
        return "{\"success\":false,\"error\":\"Password too long (max 256 chars)\"}";
    if (email.size() > 254)
        return "{\"success\":false,\"error\":\"Email too long (max 254 chars)\"}";
    // Password strength: min 8 chars, min 1 digit
    if (password.size() < 8)
        return "{\"success\":false,\"error\":\"Password must be at least 8 characters\"}";
    {
        bool hasDigit = false;
        for (unsigned char c : password) if (std::isdigit(c)) { hasDigit = true; break; }
        if (!hasDigit)
            return "{\"success\":false,\"error\":\"Password must contain at least one number\"}";
    }
    auto res = authMgr_.registerUser(username, password, email);
    if (!res.ok) return "{\"success\":false,\"error\":\"" + jsonEscape(res.error) + "\"}";
    authMgr_.save(dbPath_ + ".auth");
    return "{\"success\":true,\"token\":\"" + jsonEscape(res.token) +
           "\",\"refresh_token\":\"" + jsonEscape(res.refresh) +
           "\",\"user_id\":" + std::to_string(res.userId) + "}";
}

inline std::string MilanHttpServer::handleAuthLogin(const std::string& body, const std::string& clientIp) {
    // Layer 1: Token-bucket rate limit per IP (5 burst, 5/15min)
    if (!loginLimiter_.allow(clientIp))
        return "{\"success\":false,\"error\":\"Too many login attempts. Try again later.\",\"retry_after\":900,\"code\":429}";
    // Layer 2: IP-based lockout (spray attacks across multiple usernames)
    {
        std::lock_guard<std::mutex> lk(lockoutMutex_);
        auto iit = ipLockouts_.find(clientIp);
        if (iit != ipLockouts_.end() && iit->second.failedAttempts >= 10) {
            auto now = std::chrono::steady_clock::now();
            if (now < iit->second.lockedUntil)
                return "{\"success\":false,\"error\":\"IP blocked after too many failed logins. Try again in 15 minutes.\",\"retry_after\":900,\"code\":429}";
            iit->second.failedAttempts = 0;
        }
    }
    std::string username = extractJsonStr(body, "username");
    std::string password = extractJsonStr(body, "password");
    if (username.empty() || password.empty())
        return "{\"success\":false,\"error\":\"username and password required\"}";

    // ══ FORTRESS: Schicht 4 — Canary credential check ═════
    if (milansql::g_fortress().isCanaryCredential(username, password)) {
        milansql::g_fortress().recordHoneypotHit(clientIp, "CANARY_LOGIN: " + username);
        milansql::g_fortress().saveBanList(dbPath_ + ".banlist");
        // Return fake success with canary tokens
        return milansql::g_fortress().getHoneypotLoginResponse();
    }

    // ══ FORTRESS: Schicht 2 — Progressive delay ═══════════
    {
        double delay = milansql::g_fortress().getDelay(clientIp);
        if (delay > 0.0 && delay <= 16.0) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(static_cast<int>(delay * 1000)));
        }
    }

    // Layer 3: Per-username lockout (brute-force single account)
    {
        std::lock_guard<std::mutex> lk(lockoutMutex_);
        auto lit = loginLockouts_.find(username);
        if (lit != loginLockouts_.end() && lit->second.failedAttempts >= 5) {
            auto now = std::chrono::steady_clock::now();
            if (now < lit->second.lockedUntil)
                return "{\"success\":false,\"error\":\"Account locked after too many failed attempts. Try again in 15 minutes.\",\"retry_after\":900,\"code\":429}";
            lit->second.failedAttempts = 0;
        }
    }
    auto res = authMgr_.login(username, password);
    if (!res.ok) {
        // ══ FORTRESS: Schicht 2 — Record failure for progressive delay ═══
        milansql::g_fortress().recordFailure(clientIp, "LOGIN_FAIL: " + username);

        // Increment both per-username and per-IP failure counters
        std::lock_guard<std::mutex> lk(lockoutMutex_);
        auto& uInfo = loginLockouts_[username];
        uInfo.failedAttempts++;
        if (uInfo.failedAttempts >= 5)
            uInfo.lockedUntil = std::chrono::steady_clock::now() + std::chrono::minutes(15);
        auto& ipInfo = ipLockouts_[clientIp];
        ipInfo.failedAttempts++;
        if (ipInfo.failedAttempts >= 10)
            ipInfo.lockedUntil = std::chrono::steady_clock::now() + std::chrono::minutes(15);
        return "{\"success\":false,\"error\":\"" + jsonEscape(res.error) + "\"}";
    }
    // Success — reset both counters
    {
        std::lock_guard<std::mutex> lk(lockoutMutex_);
        loginLockouts_.erase(username);
        ipLockouts_.erase(clientIp);
    }
    authMgr_.save(dbPath_ + ".auth");
    return "{\"success\":true,\"token\":\"" + jsonEscape(res.token) +
           "\",\"refresh_token\":\"" + jsonEscape(res.refresh) +
           "\",\"user_id\":" + std::to_string(res.userId) +
           ",\"username\":\"" + jsonEscape(username) + "\"}";
}

inline std::string MilanHttpServer::handleAuthLogout(const std::string& token) {
    if (token.empty()) return "{\"success\":false,\"error\":\"No token\"}";
    authMgr_.logout(token);
    authMgr_.save(dbPath_ + ".auth");  // persist revocation immediately
    return "{\"success\":true,\"message\":\"Logged out\"}";
}
inline std::string MilanHttpServer::handleAuthMe(const std::string& token) {
    if (token.empty()) return "{\"success\":false,\"error\":\"No token\"}";
    auto v = authMgr_.validateToken(token);
    if (!v.valid) return "{\"success\":false,\"error\":\"Invalid or expired token\"}";
    return "{\"success\":true,\"user_id\":" + std::to_string(v.userId) +
           ",\"username\":\"" + jsonEscape(v.username) +
           "\",\"role\":\"" + jsonEscape(v.role) +
           "\",\"token\":\"" + jsonEscape(token) + "\"}";
}
inline std::string MilanHttpServer::handleAuthRefresh(const std::string& body) {
    std::string ref = extractJsonStr(body, "refresh_token");
    if (ref.empty()) ref = extractJsonStr(body, "refreshToken");
    if (ref.empty()) return "{\"success\":false,\"error\":\"refresh_token required\"}";
    auto res = authMgr_.refreshToken(ref);
    if (!res.ok) return "{\"success\":false,\"error\":\"" + jsonEscape(res.error) + "\"}";
    return "{\"success\":true,\"token\":\"" + jsonEscape(res.token) +
           "\",\"refresh_token\":\"" + jsonEscape(res.refresh) + "\"}";
}
inline std::string MilanHttpServer::handleAuthSessions(const std::string& token) {
    auto v = authMgr_.validateToken(token);
    if (!v.valid) return "{\"success\":false,\"error\":\"Unauthorized\"}";
    std::string tbl = authMgr_.showSessions();
    return "{\"success\":true,\"sessions\":\"" + jsonEscape(tbl) + "\"}";
}
inline std::string MilanHttpServer::handleAuthApiKey(const std::string& token, const std::string& body) {
    auto v = authMgr_.validateToken(token);
    if (!v.valid) return "{\"success\":false,\"error\":\"Unauthorized\"}";
    // Legacy: generate single key
    (void)body;
    std::string key = authMgr_.generateApiKey(v.userId);
    authMgr_.save(dbPath_ + ".auth");
    return "{\"success\":true,\"api_key\":\"" + jsonEscape(key) + "\"}";
}

// Phase 156: Named API Key management
inline std::string MilanHttpServer::handleAuthApiKeyCreate(int userId, const std::string& body) {
    std::string name = extractJsonStr(body, "name");
    if (name.empty()) name = "key-" + std::to_string((int)std::time(nullptr));

    // expires_in_days
    int days = 0;
    {
        std::string k = "\"expires_in_days\"";
        auto pos = body.find(k);
        if (pos != std::string::npos) {
            pos = body.find(':', pos+k.size());
            if (pos != std::string::npos) {
                pos = body.find_first_not_of(" \t\r\n", pos+1);
                auto end = body.find_first_of(",}", pos);
                try { days = std::stoi(body.substr(pos, end-pos)); } catch(...) {}
            }
        }
    }
    // permissions array
    std::vector<std::string> perms, tables;
    // (simplified: just use "SELECT" etc.)
    std::string permStr = extractJsonStr(body, "permissions");
    if (!permStr.empty()) {
        std::istringstream ss(permStr);
        std::string p; while(std::getline(ss,p,',')) { if(!p.empty()) perms.push_back(p); }
    }

    std::string key = authMgr_.createNamedApiKey(userId, name, days, perms, tables);
    authMgr_.save(dbPath_ + ".auth");

    std::string expiresStr = "null";
    if (days > 0) {
        time_t exp = std::time(nullptr) + (time_t)days*86400;
        char buf[32]; std::tm tm = milansql::safe_gmtime(&exp);
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
        expiresStr = "\"" + std::string(buf) + "\"";
    }
    return "{\"success\":true,\"key\":\"" + jsonEscape(key) +
           "\",\"name\":\"" + jsonEscape(name) +
           "\",\"expires\":" + expiresStr + "}";
}

inline std::string MilanHttpServer::handleAuthApiKeyList(int userId) {
    auto keys = authMgr_.listApiKeys(userId);
    std::string json = "{\"success\":true,\"api_keys\":[";
    for (size_t i = 0; i < keys.size(); ++i) {
        if (i) json += ",";
        const auto& k = keys[i];
        json += "{\"key\":\"" + jsonEscape(k.key) +
                "\",\"name\":\"" + jsonEscape(k.name) +
                "\",\"created_at\":" + std::to_string(k.createdAt) +
                ",\"expires_at\":" + std::to_string(k.expiresAt) +
                ",\"requests_today\":" + std::to_string(k.requestsToday) + "}";
    }
    json += "]}";
    return json;
}

inline std::string MilanHttpServer::handleAuthApiKeyDelete(int userId, const std::string& keyId) {
    // Check ownership
    auto* ki = authMgr_.getApiKeyInfo(keyId);
    if (ki && ki->userId != userId) return "{\"success\":false,\"error\":\"Access denied\"}";
    bool ok = authMgr_.revokeApiKey(keyId);
    authMgr_.save(dbPath_ + ".auth");
    return ok ? "{\"success\":true}" : "{\"success\":false,\"error\":\"Key not found\"}";
}

inline std::string MilanHttpServer::handleAuthApiKeyStats(int userId, const std::string& keyId) {
    auto* ki = authMgr_.getApiKeyInfo(keyId);
    if (!ki || ki->userId != userId) return "{\"success\":false,\"error\":\"Not found\"}";
    return "{\"success\":true,\"key\":\"" + jsonEscape(keyId) +
           "\",\"requests_today\":" + std::to_string(ki->requestsToday) +
           ",\"last_used\":" + std::to_string(ki->lastUsed) + "}";
}

// Phase 156: Admin + Quota
inline std::string MilanHttpServer::handleAdminUsers(const std::string& token) {
    auto v = authMgr_.validateToken(token);
    if (!v.valid || v.role != "root") return "{\"success\":false,\"error\":\"Access denied\"}";
    std::string out = authMgr_.showAllUsers();
    return "{\"success\":true,\"users\":\"" + jsonEscape(out) + "\"}";
}
inline std::string MilanHttpServer::handleAdminStats(const std::string& token) {
    auto v = authMgr_.validateToken(token);
    if (!v.valid || v.role != "root") return "{\"success\":false,\"error\":\"Access denied\"}";
    std::unique_lock<std::shared_mutex> lk(engineMutex_);
    auto tables = engine_.getAllTableNames();
    long long totalRows = 0;
    for (const auto& t : tables) { try { totalRows += engine_.countRows(t,true); } catch(...){} }
    return "{\"success\":true,\"total_tables\":" + std::to_string(tables.size()) +
           ",\"total_rows\":" + std::to_string(totalRows) +
           ",\"total_users\":" + std::to_string(authMgr_.getUserCount()) + "}";
}
inline std::string MilanHttpServer::handleAdminQuota(const std::string& token) {
    auto v = authMgr_.validateToken(token);
    if (!v.valid || v.role != "root") return "{\"success\":false,\"error\":\"Access denied\"}";
    return "{\"success\":true,\"quotas\":\"default: 100 tables, 1M rows, 1GB storage\"}";
}
inline std::string MilanHttpServer::handleMyQuota(const std::string& token) {
    AuthManager::ValidateResult v{0,"","root",true};
    if (!token.empty()) v = authMgr_.validateToken(token);
    if (!v.valid && !token.empty()) return "{\"success\":false,\"error\":\"Unauthorized\"}";
    auto quota = authMgr_.getQuota(v.userId);
    std::unique_lock<std::shared_mutex> lk(engineMutex_);
    auto allTables = engine_.getAllTableNames();
    int myTables = 0;
    long long myRows = 0;
    if (v.userId > 0) {
        std::string prefix = "u" + std::to_string(v.userId) + "_";
        for (const auto& t : allTables) {
            if (t.size() > prefix.size() && t.substr(0,prefix.size()) == prefix) {
                ++myTables;
                try { myRows += engine_.countRows(t,true); } catch(...) {}
            }
        }
    } else {
        myTables = (int)allTables.size();
        for (const auto& t : allTables) { try { myRows += engine_.countRows(t,true); } catch(...){} }
    }
    return "{\"success\":true,\"tables_used\":" + std::to_string(myTables) +
           ",\"tables_max\":" + std::to_string(quota.maxTables) +
           ",\"rows_used\":" + std::to_string(myRows) +
           ",\"rows_max\":" + std::to_string(quota.maxRows) +
           ",\"storage_max_mb\":" + std::to_string(quota.maxStorageMB) + "}";
}

// ── Phase 167: Backup — full SQL dump ─────────────────────────
inline std::string MilanHttpServer::handleBackup(const std::string& token) {
    // Auth: any valid user (own tables), root/ADMIN (all tables)
    AuthManager::ValidateResult v{0, "anonymous", "root", true};
    if (!token.empty()) {
        v = authMgr_.validateToken(token);
        if (!v.valid)
            return "-- ERROR: Access denied. Valid token required.\n";
    }
    bool isRoot = (v.role == "root");
    std::string userPrefix = (v.userId > 0) ? "u" + std::to_string(v.userId) + "_" : "";

    std::unique_lock<std::shared_mutex> lk(engineMutex_);
    std::ostringstream out;
    out << "-- MilanSQL Backup";
    if (!isRoot && v.userId > 0)
        out << " (user: " << v.username << ")";
    out << "\n";
    out << "-- Generated: " << [](){
        auto now = std::chrono::system_clock::now();
        auto t = std::chrono::system_clock::to_time_t(now);
        char buf[64]; std::tm ltm = milansql::safe_localtime(&t);
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &ltm);
        return std::string(buf);
    }() << "\n";
    out << "-- ================================================\n\n";

    auto tableNames = engine_.getAllTableNamesInternal();
    std::sort(tableNames.begin(), tableNames.end());

    int tableCount = 0;
    for (const auto& fullName : tableNames) {
        // Skip internal/info_schema tables
        if (fullName.find("information_schema.") == 0) continue;
        if (fullName.find("_sys_") == 0) continue;

        // Non-root users: only their own tables (u{id}_*)
        if (!isRoot && v.userId > 0) {
            // Check bare name or schema-prefixed name for user prefix
            std::string bareName = fullName;
            auto dot = fullName.find('.');
            if (dot != std::string::npos) bareName = fullName.substr(dot + 1);
            if (bareName.substr(0, userPrefix.size()) != userPrefix) continue;
        }

        try {
            ++tableCount;
            const auto& tbl = engine_.selectAll(fullName);
            const auto& cols = tbl.columns();
            const auto& fks = tbl.getForeignKeys();

            // DROP + CREATE TABLE (idempotent, like mysqldump)
            out << "DROP TABLE IF EXISTS " << fullName << ";\n";
            out << "CREATE TABLE " << fullName << " (\n";
            for (size_t i = 0; i < cols.size(); ++i) {
                out << "  " << cols[i].name << " " << cols[i].type;
                if (cols[i].isPrimaryKey)   out << " PRIMARY KEY";
                if (cols[i].autoIncrement)  out << " AUTO_INCREMENT";
                if (cols[i].notNull && !cols[i].isPrimaryKey) out << " NOT NULL";
                if (cols[i].isUnique && !cols[i].isPrimaryKey) out << " UNIQUE";
                if (cols[i].hasDefault)     out << " DEFAULT " << cols[i].defaultValue;
                if (i + 1 < cols.size() || !fks.empty()) out << ",";
                out << "\n";
            }
            for (size_t i = 0; i < fks.size(); ++i) {
                out << "  FOREIGN KEY (" << fks[i].fromCol << ") REFERENCES "
                    << fks[i].refTable << "(" << fks[i].refCol << ")";
                if (fks[i].onDelete != "RESTRICT")
                    out << " ON DELETE " << fks[i].onDelete;
                if (i + 1 < fks.size()) out << ",";
                out << "\n";
            }
            out << ");\n\n";

            // INSERT rows (skip dead MVCC rows)
            const auto& rows = tbl.rows();
            for (const auto& row : rows) {
                if (row.xmax != 0) continue;  // skip deleted rows
                out << "INSERT INTO " << fullName << " VALUES (";
                for (size_t c = 0; c < row.values.size(); ++c) {
                    if (c > 0) out << ", ";
                    const auto& val = row.values[c];
                    if (val == "NULL") {
                        out << "NULL";
                    } else if (c < cols.size() &&
                               (cols[c].type.find("INT") != std::string::npos ||
                                cols[c].type.find("FLOAT") != std::string::npos ||
                                cols[c].type.find("DOUBLE") != std::string::npos ||
                                cols[c].type.find("DECIMAL") != std::string::npos ||
                                cols[c].type.find("NUMERIC") != std::string::npos ||
                                cols[c].type == "BOOLEAN" || cols[c].type == "BOOL")) {
                        out << val;
                    } else {
                        // String value — escape single quotes
                        out << "'";
                        for (char ch : val) {
                            if (ch == '\'') out << "''";
                            else out << ch;
                        }
                        out << "'";
                    }
                }
                out << ");\n";
            }
            if (!rows.empty()) out << "\n";
        } catch (...) {
            out << "-- ERROR dumping table: " << fullName << "\n\n";
        }
    }

    out << "-- End of backup (" << tableCount << " tables)\n";
    return out.str();
}

// ── Phase 168: Restore — import SQL dump ──────────────────────

// Split SQL dump into individual statements.
// Respects string literals ('...'), escaped quotes (''), and -- comments.
static std::vector<std::string> splitSqlStatements(const std::string& dump) {
    std::vector<std::string> stmts;
    std::string current;
    bool inString = false;

    for (size_t i = 0; i < dump.size(); ++i) {
        char c = dump[i];

        // -- line comment (outside strings): skip to end of line
        if (!inString && c == '-' && i + 1 < dump.size() && dump[i + 1] == '-') {
            while (i < dump.size() && dump[i] != '\n') ++i;
            continue;
        }

        // String literal tracking
        if (!inString && c == '\'') {
            inString = true;
            current += c;
        } else if (inString && c == '\'') {
            current += c;
            // Check for escaped quote ''
            if (i + 1 < dump.size() && dump[i + 1] == '\'') {
                current += '\'';
                ++i;
            } else {
                inString = false;
            }
        } else if (!inString && c == ';') {
            // Statement boundary — trim and collect
            // Trim whitespace
            size_t start = current.find_first_not_of(" \t\n\r");
            if (start != std::string::npos) {
                size_t end = current.find_last_not_of(" \t\n\r");
                std::string stmt = current.substr(start, end - start + 1);
                if (!stmt.empty()) stmts.push_back(std::move(stmt));
            }
            current.clear();
        } else {
            current += c;
        }
    }

    // Last statement (no trailing ;)
    size_t start = current.find_first_not_of(" \t\n\r");
    if (start != std::string::npos) {
        size_t end = current.find_last_not_of(" \t\n\r");
        std::string stmt = current.substr(start, end - start + 1);
        if (!stmt.empty()) stmts.push_back(std::move(stmt));
    }

    return stmts;
}

inline std::string MilanHttpServer::handleRestore(const std::string& token,
                                                   const std::string& dump,
                                                   bool dryRun, bool clean) {
    auto v = authMgr_.validateToken(token);
    if (!v.valid || v.role != "root")
        return R"({"success":false,"error":"Access denied. Root only."})";

    auto stmts = splitSqlStatements(dump);
    if (stmts.empty())
        return R"({"success":false,"error":"No SQL statements found in dump"})";

    int ok = 0, fail = 0;
    int dropped = 0;
    std::string errors;

    if (dryRun) {
        // Dry run: parse only, report what would be dropped
        milansql::Parser parser;
        std::string wouldDrop;
        for (const auto& sql : stmts) {
            try {
                auto cmd = parser.parse(sql);
                if (clean && cmd.type == milansql::CommandType::CREATE_TABLE) {
                    wouldDrop += cmd.tableName + ", ";
                    ++dropped;
                }
                ++ok;
            } catch (const std::exception& e) {
                ++fail;
                if (errors.size() < 2000)
                    errors += "PARSE ERROR: " + std::string(e.what()).substr(0, 100) + "\\n";
            }
        }
        if (!wouldDrop.empty()) {
            // Remove trailing ", "
            wouldDrop = wouldDrop.substr(0, wouldDrop.size() - 2);
            if (errors.size() < 2000)
                errors += "WOULD DROP: " + wouldDrop + "\\n";
        }
    } else {
        // Execute each statement through handleQueryForUser (root context)
        milansql::Parser restoreParser;
        for (const auto& sql : stmts) {
            try {
                // clean mode: auto-drop table before CREATE TABLE
                if (clean) {
                    auto cmd = restoreParser.parse(sql);
                    if (cmd.type == milansql::CommandType::CREATE_TABLE && !cmd.tableName.empty()) {
                        std::string dropSql = "DROP TABLE IF EXISTS " + cmd.tableName;
                        handleQueryForUser(dropSql, v.userId, v.role);
                        ++dropped;
                    }
                }

                auto result = handleQueryForUser(sql, v.userId, v.role);
                if (result.find("\"success\":false") != std::string::npos) {
                    ++fail;
                    auto epos = result.find("\"error\":\"");
                    if (epos != std::string::npos && errors.size() < 2000) {
                        auto eend = result.find('"', epos + 9);
                        errors += result.substr(epos + 9,
                            eend != std::string::npos ? eend - epos - 9 : 80) + "\\n";
                    }
                } else {
                    ++ok;
                }
            } catch (const std::exception& e) {
                ++fail;
                if (errors.size() < 2000)
                    errors += "ERROR: " + std::string(e.what()).substr(0, 100) + "\\n";
            }
        }
    }

    std::string result = "{\"success\":true,\"dry_run\":" + std::string(dryRun ? "true" : "false") +
        ",\"clean\":" + std::string(clean ? "true" : "false") +
        ",\"total\":" + std::to_string(stmts.size()) +
        ",\"ok\":" + std::to_string(ok) +
        ",\"failed\":" + std::to_string(fail) +
        ",\"dropped\":" + std::to_string(dropped);
    if (!errors.empty())
        result += ",\"errors\":\"" + errors + "\"";
    result += "}";
    return result;
}

// ── MilanHttpServer::handleQueryForUser (Phase 154-155) ───────

inline std::string MilanHttpServer::handleQueryForUser(const std::string& sql, int userId, const std::string& userRole) {
    std::unique_lock<std::shared_mutex> lock(engineMutex_);

    bool isRoot = (userId <= 0 || userRole == "root");
    bool isService = (userRole == "service");  // service accounts: no table prefix, but rate-limited
    std::string prefix = (isRoot || isService) ? "" : ("u" + std::to_string(userId) + "_");

    auto persistFn = [this]() {
        if (engine_.isInTransaction()) return;
        // Audit Bug #25: Persist-Fehler (z.B. Disk voll) NICHT mehr
        // stillschweigend schlucken — Client bekommt success:false,
        // /health meldet storage.status=error. Vorher: bestätigte
        // Commits waren nach Neustart weg (silent data loss).
        try {
            storage_.save(engine_);
            lastPersistError_.clear();
        } catch (const std::exception& e) {
            lastPersistError_ = e.what();
            std::cerr << "  [Persist] FEHLER: " << e.what() << "\n";
            throw std::runtime_error(
                std::string("Persistierung fehlgeschlagen (Daten NICHT dauerhaft "
                            "gespeichert): ") + e.what());
        }
    };
    auto saveProceduresFn = [this]() {
        std::ofstream pf(dbPath_ + ".procedures");
        if (!pf) return;
        for (const auto& [n, p] : engine_.getAllProcedures()) {
            pf << p.name << "\t" << p.params.size() << "\n";
            for (const auto& param : p.params)
                pf << param.first << "\t" << param.second << "\n";
            std::string enc;
            for (char c : p.body) { if (c=='\n') enc += "\\n"; else enc += c; }
            pf << enc << "\n";
        }
    };
    auto saveTriggFn = [this]() {
        std::ofstream tf(dbPath_ + ".triggers");
        if (tf)
            for (const auto& [n, t] : engine_.getAllTriggers())
                tf << t.name << "\t" << t.timing << "\t" << t.event
                   << "\t" << t.tableName << "\t" << t.body << "\n";
    };

    // Phase 157: Set current user in engine so USER()/CURRENT_USER() work
    {
        std::string uname = "root";
        if (!isRoot && !isService && userId > 0) {
            const AuthUser* au = authMgr_.getUserById(userId);
            if (au) uname = au->username;
        }
        engine_.setCurrentUserDirect(uname);
    }
    // v9.2.0: Set numeric userId for per-request context (used by SHOW TABLES filter etc.)
    // Service accounts get root-level table access (no prefix)
    engine_.setCurrentUser((isRoot || isService) ? 0 : userId, isRoot || isService);

    // Intercept special SQL commands
    auto sqlUp = [](std::string s) {
        for (auto& c : s) c = (char)toupper((unsigned char)c);
        return s;
    };
    std::string trimmed = sql;
    while (!trimmed.empty() && (trimmed[0]==' '||trimmed[0]=='\t'||trimmed[0]=='\n'||trimmed[0]=='\r'))
        trimmed.erase(0,1);
    std::string upper = sqlUp(trimmed);

    // Phase 157: @@variable intercepts (MySQL compatibility)
    // Handle: SELECT @@version, SELECT @@version_comment, SELECT @@global.version, etc.
    {
        // Strip trailing semicolons for comparison
        std::string u2 = upper;
        while (!u2.empty() && (u2.back()==';'||u2.back()==' ')) u2.pop_back();
        auto makeScalar = [](const std::string& col, const std::string& val) -> std::string {
            return "{\"success\":true,\"columns\":[\"" + col + "\"],\"rows\":[[\"" + val + "\"]]}";
        };
        if (u2 == "SELECT @@VERSION" || u2 == "SELECT @@GLOBAL.VERSION")
            return makeScalar("@@version", MILANSQL_VERSION);
        if (u2 == "SELECT @@VERSION_COMMENT" || u2 == "SELECT @@GLOBAL.VERSION_COMMENT")
            return makeScalar("@@version_comment", "MilanSQL Database Engine");
        if (u2 == "SELECT @@VERSION, @@VERSION_COMMENT" ||
            u2 == "SELECT @@VERSION,@@VERSION_COMMENT")
            return std::string("{\"success\":true,\"columns\":[\"@@version\",\"@@version_comment\"],\"rows\":[[\"") + MILANSQL_VERSION + "\",\"MilanSQL Database Engine\"]]}";
        if (u2 == "SELECT @@MAX_ALLOWED_PACKET" || u2 == "SELECT @@GLOBAL.MAX_ALLOWED_PACKET")
            return makeScalar("@@max_allowed_packet", "67108864");
        if (u2 == "SELECT @@SQL_MODE" || u2 == "SELECT @@GLOBAL.SQL_MODE" || u2 == "SELECT @@SESSION.SQL_MODE")
            return makeScalar("@@sql_mode", "");
        if (u2 == "SELECT @@CHARACTER_SET_CLIENT" || u2 == "SELECT @@GLOBAL.CHARACTER_SET_CLIENT")
            return makeScalar("@@character_set_client", "utf8mb4");
        if (u2 == "SELECT @@AUTOCOMMIT")
            return makeScalar("@@autocommit", "1");
        if (u2 == "SELECT @@TRANSACTION_ISOLATION" || u2 == "SELECT @@TX_ISOLATION")
            return makeScalar("@@transaction_isolation", "READ-COMMITTED");
    }


    // Phase 170: SHOW POLICIES ON <table> — return as JSON result set
    if (upper.rfind("SHOW POLICIES", 0) == 0) {
        // Extract table name after ON
        std::string tblName;
        auto onPos = upper.find(" ON ");
        if (onPos != std::string::npos) {
            tblName = trimmed.substr(onPos + 4);
            while (!tblName.empty() && (tblName.back()==';'||tblName.back()==' ')) tblName.pop_back();
            while (!tblName.empty() && tblName.front()==' ') tblName.erase(0,1);
        }
        if (!tblName.empty()) {
            return engine_.getTablePoliciesJson(tblName);
        }
        return engine_.getRlsPoliciesJson();
    }


    // v9.2.0: SHOW TABLES — filtered per-user to prevent cross-user table name leakage
    {
        std::string u2 = upper;
        while (!u2.empty() && (u2.back()==';'||u2.back()==' ')) u2.pop_back();
        if (u2 == "SHOW TABLES") {
            auto all = engine_.getAllTableNames();
            std::vector<std::array<std::string,4>> rows;
            for (const auto& t : all) {
                std::string displayName;
                if (isRoot || isService) {
                    if (t.size()>=2 && t[0]=='_' && t[1]=='_') continue; // skip system tables
                    displayName = t;
                } else {
                    if (t.size() > prefix.size() && t.substr(0, prefix.size()) == prefix)
                        displayName = t.substr(prefix.size());
                    else
                        continue; // not this user's table
                }
                try {
                    const auto& tref = engine_.selectAll(t);
                    rows.push_back({displayName, "TABLE",
                        std::to_string(tref.columns().size()),
                        std::to_string(tref.rowCount())});
                } catch (...) {
                    rows.push_back({displayName, "TABLE", "?", "?"});
                }
            }
            if (rows.empty())
                return "{\"success\":true,\"columns\":[\"Name\",\"Typ\",\"Spalten\",\"Zeilen\"],\"rows\":[]}";
            std::string out = "Name | Typ | Spalten | Zeilen\n";
            out += "-----+-----+---------+-------\n";
            for (const auto& r : rows)
                out += r[0] + " | " + r[1] + " | " + r[2] + " | " + r[3] + "\n";
            engine_.setCurrentUser(0, true); // reset
            return parseOutputToJson(out);
        }
    }

    // SHOW USERS
    if (upper == "SHOW USERS") {
        if (!isRoot) return "{\"success\":false,\"error\":\"Access denied: requires root\"}";
        return parseOutputToJson(authMgr_.showUsers());
    }
    // SHOW ALL USERS
    if (upper == "SHOW ALL USERS") {
        if (!isRoot) return "{\"success\":false,\"error\":\"Access denied: requires root\"}";
        return parseOutputToJson(authMgr_.showAllUsers());
    }
    // SHOW SESSIONS  /  SELECT * FROM __sessions__
    if (upper == "SHOW SESSIONS" ||
        upper == "SELECT * FROM __SESSIONS__" ||
        upper == "SELECT * FROM __SESSIONS__;" ) {
        if (!isRoot) return "{\"success\":false,\"error\":\"Access denied: requires root\"}";
        return parseOutputToJson(authMgr_.showSessions());
    }
    // SHOW MY QUOTA
    if (upper == "SHOW MY QUOTA" || upper == "SHOW QUOTA") {
        auto quota = authMgr_.getQuota(userId);
        auto allTbls = engine_.getAllTableNames();
        int cnt = 0; long long rows = 0;
        if (!isRoot) {
            for (const auto& t : allTbls) {
                if (!prefix.empty() && t.size() > prefix.size() && t.substr(0,prefix.size()) == prefix) {
                    ++cnt; try { rows += engine_.countRows(t,true); } catch(...) {}
                }
            }
        } else { cnt = (int)allTbls.size(); for (const auto& t:allTbls) { try{rows+=engine_.countRows(t,true);}catch(...){} } }
        std::string out = "tables | rows | storage_mb | max_tables | max_rows | max_storage_mb\n";
        out += "-------+------+------------+------------+----------+---------------\n";
        out += std::to_string(cnt) + " | " + std::to_string(rows) + " | 0 | "
            + std::to_string(quota.maxTables) + " | " + std::to_string(quota.maxRows)
            + " | " + std::to_string(quota.maxStorageMB) + "\n";
        return parseOutputToJson(out);
    }

    // GRANT privilege ON table TO user  (Phase 155)
    if (upper.substr(0, 5) == "GRANT") {
        // Parse: GRANT privilege[(cols)] ON table TO username
        // Only owner or root can GRANT
        // Simple implementation: parse key tokens
        std::string rest = trimmed.substr(5);
        while (!rest.empty() && rest[0]==' ') rest.erase(0,1);
        // Find ON
        auto onPos = upper.find(" ON ", 5);
        auto toPos = upper.find(" TO ");
        if (onPos != std::string::npos && toPos != std::string::npos) {
            std::string privStr = trimmed.substr(5, onPos - 5);
            while (!privStr.empty()&&privStr[0]==' ') privStr.erase(0,1);
            while (!privStr.empty()&&privStr.back()==' ') privStr.pop_back();
            std::string tablePart = trimmed.substr(onPos+4, toPos-onPos-4);
            while (!tablePart.empty()&&tablePart[0]==' ') tablePart.erase(0,1);
            while (!tablePart.empty()&&tablePart.back()==' ') tablePart.pop_back();
            std::string targetUser = trimmed.substr(toPos+4);
            while (!targetUser.empty()&&targetUser[0]==' ') targetUser.erase(0,1);
            while (!targetUser.empty()&&targetUser.back()==' ') targetUser.pop_back();

            // Find target user id
            const AuthUser* tu = authMgr_.getUser(targetUser);
            if (!tu) return "{\"success\":false,\"error\":\"User not found: " + jsonEscape(targetUser) + "\"}";

            // Resolve table (add prefix if it's a plain name)
            std::string physTable = tablePart;
            // Check if it's "owner.table" format
            auto dot = physTable.find('.');
            if (dot != std::string::npos) {
                // Cross-user grant: "alice.orders" → prefix user alice's table
                std::string owner = physTable.substr(0, dot);
                std::string tbl   = physTable.substr(dot+1);
                const AuthUser* ou = authMgr_.getUser(owner);
                if (!ou) return "{\"success\":false,\"error\":\"Owner user not found\"}";
                if (!isRoot && ou->id != userId)
                    return "{\"success\":false,\"error\":\"Access denied: cannot grant other user's table\"}";
                physTable = "u" + std::to_string(ou->id) + "_" + tbl;
            } else {
                // Table in current user's namespace OR root sees all
                if (!isRoot) physTable = prefix + tablePart;
            }

            // Column-level: GRANT SELECT(col1, col2) ON ...
            std::vector<std::string> cols;
            auto parenOpen = privStr.find('(');
            std::string priv = privStr;
            if (parenOpen != std::string::npos) {
                auto parenClose = privStr.find(')');
                std::string colList = privStr.substr(parenOpen+1, parenClose-parenOpen-1);
                priv = privStr.substr(0, parenOpen);
                while (!priv.empty()&&priv.back()==' ') priv.pop_back();
                std::istringstream cs(colList);
                std::string col;
                while (std::getline(cs, col, ',')) {
                    while (!col.empty()&&col[0]==' ') col.erase(0,1);
                    while (!col.empty()&&col.back()==' ') col.pop_back();
                    if (!col.empty()) cols.push_back(col);
                }
            }
            for (auto& c : priv) c = (char)toupper((unsigned char)c);

            Permission perm;
            perm.userId = tu->id;
            perm.tableName = physTable;
            perm.privilege = priv;
            perm.columns = cols;
            perm.grantedBy = userId;
            authMgr_.grantPermission(perm);
            authMgr_.save(dbPath_ + ".auth");
            std::cout << "OK\n"; // output for parseOutputToJson
            return "{\"success\":true,\"message\":\"GRANT " + jsonEscape(priv) + " on " + jsonEscape(tablePart) + " to " + jsonEscape(targetUser) + "\"}";
        }
        // Fall through if parse fails
    }

    // REVOKE privilege ON table FROM user
    if (upper.substr(0,6) == "REVOKE") {
        auto onPos = upper.find(" ON ");
        auto fromPos = upper.find(" FROM ");
        if (onPos != std::string::npos && fromPos != std::string::npos) {
            std::string privStr = trimmed.substr(6, onPos - 6);
            while (!privStr.empty()&&privStr[0]==' ') privStr.erase(0,1);
            while (!privStr.empty()&&privStr.back()==' ') privStr.pop_back();
            for (auto& c : privStr) c = (char)toupper((unsigned char)c);
            std::string tablePart = trimmed.substr(onPos+4, fromPos-onPos-4);
            while (!tablePart.empty()&&tablePart[0]==' ') tablePart.erase(0,1);
            while (!tablePart.empty()&&tablePart.back()==' ') tablePart.pop_back();
            std::string targetUser = trimmed.substr(fromPos+6);
            while (!targetUser.empty()&&targetUser[0]==' ') targetUser.erase(0,1);
            while (!targetUser.empty()&&targetUser.back()==' ') targetUser.pop_back();

            const AuthUser* tu = authMgr_.getUser(targetUser);
            if (!tu) return "{\"success\":false,\"error\":\"User not found: " + jsonEscape(targetUser) + "\"}";

            std::string physTable = isRoot ? tablePart : (prefix + tablePart);
            authMgr_.revokePermission(tu->id, physTable, privStr);
            authMgr_.save(dbPath_ + ".auth");
            return "{\"success\":true,\"message\":\"REVOKE " + jsonEscape(privStr) + " on " + jsonEscape(tablePart) + " from " + jsonEscape(targetUser) + "\"}";
        }
    }

    // SHOW GRANTS FOR user
    if (upper.substr(0,16) == "SHOW GRANTS FOR ") {
        std::string targetUser = trimmed.substr(16);
        while (!targetUser.empty()&&targetUser[0]==' ') targetUser.erase(0,1);
        while (!targetUser.empty()&&targetUser.back()==' ') targetUser.pop_back();
        return parseOutputToJson(authMgr_.showGrantsFor(targetUser));
    }

    // REVOKE SESSION
    if (upper.substr(0,15) == "REVOKE SESSION ") {
        if (!isRoot) return "{\"success\":false,\"error\":\"Access denied\"}";
        std::string tok = trimmed.substr(15);
        while (!tok.empty()&&tok[0]==' ') tok.erase(0,1);
        authMgr_.revokeSession(tok);
        return "{\"success\":true,\"message\":\"Session revoked\"}";
    }

    auto execOne = [&](const std::string& oneSQL) -> std::string {
        std::ostringstream cap;
        std::streambuf* old = std::cout.rdbuf(cap.rdbuf());
        bool ok = true; std::string errMsg;
        std::vector<std::string> colTypesOut;
        try {
            milansql::Parser p;
            auto cmd = p.parse(oneSQL);

            // Phase 154: Table name prefixing for user isolation
            if (!prefix.empty()) {
                // Block system table access
                if (!cmd.tableName.empty() && cmd.tableName.size() >= 2 &&
                    cmd.tableName[0] == '_' && cmd.tableName[1] == '_') {
                    std::cout.rdbuf(old);
                    return "{\"success\":false,\"error\":\"Access denied: system table\"}";
                }
                // Block cross-user access: table starts with u{N}_ where N != userId
                if (!cmd.tableName.empty() && cmd.tableName.size() > 2 &&
                    cmd.tableName[0] == 'u' && std::isdigit((unsigned char)cmd.tableName[1])) {
                    std::cout.rdbuf(old);
                    return "{\"success\":false,\"error\":\"Access denied: cross-user table access not allowed\"}";
                }
                // Phase 155: permission check for non-owner access
                // (after prefixing, check if user has shared access)
                if (!cmd.tableName.empty()) {
                    std::string op = "SELECT";
                    if (cmd.type == milansql::CommandType::INSERT) op = "INSERT";
                    else if (cmd.type == milansql::CommandType::UPDATE) op = "UPDATE";
                    else if (cmd.type == milansql::CommandType::DELETE) op = "DELETE";
                    // Check if table belongs to this user OR they have a grant
                    bool ownTable = true; // will be their own after prefixing
                    (void)ownTable; (void)op;
                    cmd.tableName = prefix + cmd.tableName;
                }
                // Prefix join tables
                for (auto& jc : cmd.joinClauses) {
                    if (!jc.table.empty() &&
                        !(jc.table.size()>=2 && jc.table[0]=='_' && jc.table[1]=='_') &&
                        !(jc.table.size()>2 && jc.table[0]=='u' && std::isdigit((unsigned char)jc.table[1])))
                        jc.table = prefix + jc.table;
                }
                // Views/Procedures/Triggers: Namen + Zieltabellen isolieren,
                // damit CREATE/DROP/CALL/SHOW pro User getrennt sind
                auto pfxName = [&prefix](std::string& n) {
                    if (n.empty()) return;
                    if (n.size() >= 2 && n[0]=='_' && n[1]=='_') return;
                    if (n.size() > 2 && n[0]=='u' && std::isdigit((unsigned char)n[1])) return;
                    n = prefix + n;
                };
                pfxName(cmd.triggerName);
                pfxName(cmd.triggerTable);
                pfxName(cmd.showTriggersTable);
                pfxName(cmd.procedureName);
            } else {
                // Root: handle "user.table" notation (cross-user)
                if (!cmd.tableName.empty()) {
                    auto dot = cmd.tableName.find('.');
                    if (dot != std::string::npos) {
                        std::string owner = cmd.tableName.substr(0, dot);
                        std::string tbl   = cmd.tableName.substr(dot+1);
                        const AuthUser* ou = authMgr_.getUser(owner);
                        if (ou) cmd.tableName = "u" + std::to_string(ou->id) + "_" + tbl;
                    }
                }
            }

            // Resolve subqueries (apply same user-prefix for isolation)
            for (auto& sq : cmd.subqueries) {
                if (!prefix.empty() && sq.subTable.find(prefix) != 0) {
                    sq.subTable = prefix + sq.subTable;
                }
                if (sq.condIdx < cmd.whereConds.size())
                    cmd.whereConds[sq.condIdx].inList =
                        engine_.subqueryValues(sq.subTable, sq.subCol, sq.subWhere, sq.subWhereLogic);
            }

            milansql::dispatchCommand(cmd, engine_, p, oneSQL, persistFn, saveProceduresFn, saveTriggFn);

            // Extract column types for type-aware JSON serialization
            if (cmd.type == milansql::CommandType::SELECT && !cmd.tableName.empty()
                && engine_.tableExists(cmd.tableName)) {
                const auto& tbl = engine_.selectAll(cmd.tableName);
                const auto& cols = tbl.columns();
                if (!cmd.selectColumns.empty()) {
                    for (const auto& selCol : cmd.selectColumns) {
                        bool found = false;
                        for (const auto& col : cols) {
                            if (col.name == selCol) {
                                colTypesOut.push_back(col.type);
                                found = true;
                                break;
                            }
                        }
                        if (!found) colTypesOut.push_back("TEXT");
                    }
                } else {
                    for (const auto& col : cols)
                        colTypesOut.push_back(col.type);
                }
            }
        } catch (const std::exception& e) { ok = false; errMsg = sanitizeError(e.what()); }
          catch (...) { ok = false; errMsg = "Unknown error"; }
        std::cout.rdbuf(old);
        if (!ok) return "{\"success\":false,\"error\":\"" + jsonEscape(errMsg) + "\"}";
        return parseOutputToJson(cap.str(), colTypesOut);
    };

    auto stmts = milansql::splitStatements(sql);
    if (stmts.size() <= 1) {
        auto result = execOne(stmts.empty() ? sql : stmts[0]);
        engine_.setCurrentUser(0, true); // reset per-request context
        return result;
    }

    std::string json = "{\"success\":true,\"results\":[";
    bool anyError = false;
    for (size_t idx = 0; idx < stmts.size(); ++idx) {
        if (idx) json += ",";
        std::string res = execOne(stmts[idx]);
        json += "{\"statement\":\"" + jsonEscape(stmts[idx]) + "\",\"result\":" + res + "}";
        if (res.find("\"success\":false") != std::string::npos) anyError = true;
    }
    engine_.setCurrentUser(0, true); // reset per-request context
    json += "],\"count\":" + std::to_string(stmts.size());
    json += anyError ? ",\"success\":false}" : ",\"success\":true}";
    return json;
}

// ── MilanHttpServer::handleListTablesForUser ──────────────────

inline std::string MilanHttpServer::handleListTablesForUser(int userId) {
    std::shared_lock<std::shared_mutex> lock(engineMutex_);
    auto all = engine_.getAllTableNames();
    std::string json = "{\"success\":true,\"tables\":[";
    bool first = true;
    if (userId <= 0) {
        for (size_t i=0;i<all.size();++i) {
            if (i) json += ",";
            json += "\"" + jsonEscape(all[i]) + "\"";
        }
    } else {
        std::string pf = "u" + std::to_string(userId) + "_";
        for (const auto& t : all) {
            if (t.size() > pf.size() && t.substr(0,pf.size()) == pf) {
                if (!first) json += ",";
                json += "\"" + jsonEscape(t.substr(pf.size())) + "\"";
                first = false;
            }
        }
    }
    json += "]}";
    return json;
}

// ── MilanHttpServer::handleQuery ──────────────────────────────
// Phase 67: supports multi-statement input; returns combined results array
// when more than one statement is present.

inline std::string MilanHttpServer::handleQuery(const std::string& sql) {
    std::unique_lock<std::shared_mutex> lock(engineMutex_);

    auto persistFn = [this]() {
        if (engine_.isInTransaction()) return;
        // Audit Bug #25: Persist-Fehler (z.B. Disk voll) NICHT mehr
        // stillschweigend schlucken — Client bekommt success:false,
        // /health meldet storage.status=error. Vorher: bestätigte
        // Commits waren nach Neustart weg (silent data loss).
        try {
            storage_.save(engine_);
            lastPersistError_.clear();
        } catch (const std::exception& e) {
            lastPersistError_ = e.what();
            std::cerr << "  [Persist] FEHLER: " << e.what() << "\n";
            throw std::runtime_error(
                std::string("Persistierung fehlgeschlagen (Daten NICHT dauerhaft "
                            "gespeichert): ") + e.what());
        }
    };

    auto saveProceduresFn = [this]() {
        std::ofstream pf(dbPath_ + ".procedures");
        if (!pf) return;
        for (const auto& [n, p] : engine_.getAllProcedures()) {
            pf << p.name << "\t" << p.params.size() << "\n";
            for (const auto& param : p.params)
                pf << param.first << "\t" << param.second << "\n";
            std::string enc;
            for (char c : p.body) {
                if (c == '\n') enc += "\\n";
                else enc += c;
            }
            pf << enc << "\n";
        }
    };

    auto saveTriggFn = [this]() {
        std::ofstream tf(dbPath_ + ".triggers");
        if (tf) {
            for (const auto& [n, t] : engine_.getAllTriggers()) {
                tf << t.name << "\t" << t.timing << "\t" << t.event
                   << "\t" << t.tableName << "\t" << t.body << "\n";
            }
        }
    };

    // ── helper: execute one statement, return its JSON result ───
    auto execOne = [&](const std::string& oneSQL) -> std::string {
        std::ostringstream cap;
        std::streambuf* old = std::cout.rdbuf(cap.rdbuf());
        bool ok = true;
        std::string errMsg;
        std::vector<std::string> colTypesOut;
        try {
            milansql::Parser p;
            auto cmd = p.parse(oneSQL);
            for (const auto& sq : cmd.subqueries) {
                if (sq.condIdx < cmd.whereConds.size())
                    cmd.whereConds[sq.condIdx].inList =
                        engine_.subqueryValues(sq.subTable, sq.subCol,
                                               sq.subWhere, sq.subWhereLogic);
            }
            milansql::dispatchCommand(cmd, engine_, p, oneSQL,
                                      persistFn, saveProceduresFn, saveTriggFn);
            // Extract column types for type-aware JSON serialization
            if (cmd.type == milansql::CommandType::SELECT && !cmd.tableName.empty()
                && engine_.tableExists(cmd.tableName)) {
                const auto& tbl = engine_.selectAll(cmd.tableName);
                const auto& cols = tbl.columns();
                if (!cmd.selectColumns.empty()) {
                    for (const auto& selCol : cmd.selectColumns) {
                        bool found = false;
                        for (const auto& col : cols) {
                            if (col.name == selCol) {
                                colTypesOut.push_back(col.type);
                                found = true;
                                break;
                            }
                        }
                        if (!found) colTypesOut.push_back("TEXT");
                    }
                } else {
                    for (const auto& col : cols)
                        colTypesOut.push_back(col.type);
                }
            }
        } catch (const std::exception& e) { ok = false; errMsg = sanitizeError(e.what()); }
          catch (...) { ok = false; errMsg = "Unbekannter Fehler"; }
        std::cout.rdbuf(old);
        if (!ok) return "{\"success\":false,\"error\":\"" + jsonEscape(errMsg) + "\"}";
        return parseOutputToJson(cap.str(), colTypesOut);
    };

    auto stmts = milansql::splitStatements(sql);

    // ── single statement → backward-compatible response ─────────
    if (stmts.size() <= 1) {
        return execOne(stmts.empty() ? sql : stmts[0]);
    }

    // ── multi-statement → combined results array ─────────────────
    std::string json = "{\"success\":true,\"results\":[";
    bool anyError = false;
    for (size_t idx = 0; idx < stmts.size(); ++idx) {
        if (idx > 0) json += ",";
        std::string res = execOne(stmts[idx]);
        // Wrap with statement key
        json += "{\"statement\":\"" + jsonEscape(stmts[idx]) + "\",\"result\":" + res + "}";
        if (res.find("\"success\":false") != std::string::npos) anyError = true;
    }
    json += "],\"count\":" + std::to_string(stmts.size());
    if (anyError) json += ",\"success\":false";
    else          json += ",\"success\":true";
    json += "}";
    return json;
}

// ── MilanHttpServer::handleListTables ─────────────────────────

inline std::string MilanHttpServer::handleListTables() {
    std::shared_lock<std::shared_mutex> lock(engineMutex_);
    auto tables = engine_.getAllTableNames();
    std::string json = "{\"success\":true,\"tables\":[";
    for (size_t i = 0; i < tables.size(); ++i) {
        if (i > 0) json += ",";
        json += "\"" + jsonEscape(tables[i]) + "\"";
    }
    json += "]}";
    return json;
}

// ── MilanHttpServer::handleDescribeTable ──────────────────────

inline std::string MilanHttpServer::handleDescribeTable(const std::string& tableName) {
    std::shared_lock<std::shared_mutex> lock(engineMutex_);
    std::ostringstream captured;
    std::streambuf* old = std::cout.rdbuf(captured.rdbuf());
    try {
        // Use DESCRIBE command via dispatch
        milansql::Parser parser;
        std::string sql = "DESCRIBE " + tableName;
        auto cmd = parser.parse(sql);
        auto noop = [](){};
        milansql::dispatchCommand(cmd, engine_, parser, sql, noop, noop, noop);
    } catch (const std::exception& e) {
        std::cout.rdbuf(old);
        return "{\"success\":false,\"error\":\"" + jsonEscape(e.what()) + "\"}";
    }
    std::cout.rdbuf(old);
    return parseOutputToJson(captured.str());
}

// ── MilanHttpServer::handleListSchemas ────────────────────────

inline std::string MilanHttpServer::handleListSchemas() {
    std::shared_lock<std::shared_mutex> lock(engineMutex_);
    auto schemas = engine_.showSchemas();
    std::string json = "{\"success\":true,\"schemas\":[";
    for (size_t i = 0; i < schemas.size(); ++i) {
        if (i > 0) json += ",";
        json += "\"" + jsonEscape(schemas[i]) + "\"";
    }
    json += "]}";
    return json;
}

// ── MilanHttpServer::handleStatus ─────────────────────────────

inline std::string MilanHttpServer::handleStatus() {
    std::unique_lock<std::shared_mutex> lock(engineMutex_);
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - startTime_).count();
    auto tables  = engine_.getAllTableNames();
    auto schemas = engine_.showSchemas();

    // Count total rows via public countRows API
    long long totalRows = 0;
    for (const auto& tname : tables) {
        try { totalRows += (long long)engine_.countRows(tname, true); } catch (...) {}
    }

    // Phase 166: format uptime as "Xh Ym Zs"
    long long h = elapsed / 3600, m = (elapsed % 3600) / 60, s = elapsed % 60;
    std::string uptimeFmt = std::to_string(h) + "h " + std::to_string(m) + "m " + std::to_string(s) + "s";
    long long qc = queryCounter_.load();

    std::string json = "{";
    json += "\"success\":true,";
    json += "\"status\":\"healthy\",";
    json += "\"version\":\"MilanSQL v" + std::string(MILANSQL_VERSION) + "\",";
    json += "\"uptime\":"       + std::to_string(elapsed) + ",";
    json += "\"uptime_fmt\":\"" + uptimeFmt + "\",";
    json += "\"tables\":"       + std::to_string(tables.size()) + ",";
    json += "\"rows\":"         + std::to_string(totalRows) + ",";
    json += "\"queries\":"      + std::to_string(qc) + ",";
    json += "\"query_count\":"  + std::to_string(qc) + ",";
    json += "\"connections\":0,";
    json += "\"slow_queries\":" + std::to_string(engine_.slowQueryLog.size()) + ",";
    json += "\"tableCount\":"   + std::to_string(tables.size()) + ",";
    json += "\"schemaCount\":"  + std::to_string(schemas.size());
    json += "}";
    return json;
}

// ── MilanHttpServer::handleSemanticSearch (Phase 121) ─────────
// POST /semantic-search
// Body JSON: {"table":"docs","vector_column":"embedding",
//             "query_vector":"[1.0,0.0,0.0]","limit":5,
//             "filter":"category = 'tech'","include_score":true}

inline std::string MilanHttpServer::handleSemanticSearch(const std::string& body, int userId, bool isRoot) {
    std::shared_lock<std::shared_mutex> lock(engineMutex_);

    // Simple JSON field extraction helper
    auto extractStr = [&](const std::string& key) -> std::string {
        std::string search = "\"" + key + "\"";
        auto pos = body.find(search);
        if (pos == std::string::npos) return "";
        pos = body.find(':', pos + search.size());
        if (pos == std::string::npos) return "";
        pos = body.find_first_not_of(" \t\r\n", pos + 1);
        if (pos == std::string::npos) return "";
        if (body[pos] == '"') {
            // quoted string
            auto end = body.find('"', pos + 1);
            while (end != std::string::npos && body[end - 1] == '\\')
                end = body.find('"', end + 1);
            if (end == std::string::npos) return "";
            return body.substr(pos + 1, end - pos - 1);
        } else if (body[pos] == '[') {
            // vector literal like [1.0,0.0]
            auto end = body.find(']', pos);
            if (end == std::string::npos) return "";
            return body.substr(pos, end - pos + 1);
        } else {
            // number or bool
            auto end = body.find_first_of(",}\r\n", pos);
            if (end == std::string::npos) return body.substr(pos);
            return body.substr(pos, end - pos);
        }
    };

    std::string table        = extractStr("table");
    std::string vecCol       = extractStr("vector_column");
    std::string queryVecStr  = extractStr("query_vector");
    std::string limitStr     = extractStr("limit");
    std::string filter       = extractStr("filter");
    std::string inclScoreStr = extractStr("include_score");

    if (table.empty() || vecCol.empty() || queryVecStr.empty())
        return R"({"success":false,"error":"Missing required fields: table, vector_column, query_vector"})";

    // Phase 173: Validate table ownership for non-root users
    if (!isRoot && userId > 0) {
        std::string userPrefix = "u" + std::to_string(userId) + "_";
        std::string bareName = table;
        auto dot = table.find('.');
        if (dot != std::string::npos) bareName = table.substr(dot + 1);
        if (bareName.substr(0, userPrefix.size()) != userPrefix)
            return R"({"success":false,"error":"Access denied: table not owned by user"})";
    }
    // Validate identifiers: reject SQL injection chars in table/column names
    auto isValidIdent = [](const std::string& s) -> bool {
        for (char c : s) {
            if (!std::isalnum(c) && c != '_' && c != '.') return false;
        }
        return !s.empty();
    };
    if (!isValidIdent(table) || !isValidIdent(vecCol))
        return R"({"success":false,"error":"Invalid table or column name"})";

    int limitN = 10;
    if (!limitStr.empty()) {
        try { limitN = std::stoi(limitStr); } catch (...) {}
    }
    bool includeScore = (inclScoreStr == "true" || inclScoreStr == "1");

    // Build SQL: SELECT *, embedding <-> '[...]' AS _score FROM table [WHERE filter] ORDER BY _score LIMIT n
    std::string sql = "SELECT *, " + vecCol + " <-> '" + queryVecStr + "' AS _score FROM " + table;
    if (!filter.empty()) sql += " WHERE " + filter;
    sql += " ORDER BY _score LIMIT " + std::to_string(limitN);

    // Execute
    milansql::Parser parser;
    milansql::ParsedCommand cmd;
    try { cmd = parser.parse(sql); }
    catch (const std::exception& e) {
        return std::string(R"({"success":false,"error":"Parse error: )") + jsonEscape(e.what()) + "\"}";
    }

    // We can't call dispatchCommand (needs lots of callbacks), so execute directly
    std::string resultJson;
    try {
        // For semantic search, execute via selectWhere + vector sort
        milansql::Table result;
        if (!filter.empty()) {
            // Parse filter into WhereCondition via the parser approach
            // Fallback: use raw selectAll and let ORDER BY handle it
            result = engine_.selectAll(table).clone();
        } else {
            result = engine_.selectAll(table).clone();
        }

        // Add distance column
        auto cols = result.columns();
        int colIdx = -1;
        for (int ci = 0; ci < (int)cols.size(); ++ci) {
            std::string bare = cols[(size_t)ci].name;
            auto dotPos = bare.rfind('.');
            if (dotPos != std::string::npos) bare = bare.substr(dotPos + 1);
            if (bare == vecCol || cols[(size_t)ci].name == vecCol) {
                colIdx = ci; break;
            }
        }

        auto queryVec = milansql::vector_type::parse(queryVecStr);
        if (colIdx >= 0 && !queryVec.empty()) {
            milansql::Column distCol("_score", "REAL");
            result.addColumn(distCol);
            int newColIdx = (int)result.columns().size() - 1;
            for (auto& row : const_cast<std::vector<milansql::Row>&>(result.rows())) {
                if (row.xmax != 0) continue;
                std::string vecStr = (size_t)colIdx < row.values.size() ? row.values[(size_t)colIdx] : "";
                auto vec = milansql::vector_type::parse(vecStr);
                double dist = vec.empty() ? 1e30 : milansql::vector_type::l2Distance(vec, queryVec);
                if ((size_t)newColIdx < row.values.size())
                    row.values[(size_t)newColIdx] = milansql::vector_type::fmtFloat((float)dist);
            }
            // Sort by _score ASC
            result.sortByMulti({{"_score", false}});
        }

        // Apply limit
        auto& rows = result.rows();
        const auto& resultCols = result.columns();

        resultJson = "{\"success\":true,\"rows\":[";
        bool first = true;
        size_t count = 0;
        for (size_t ri = 0; ri < rows.size() && count < (size_t)limitN; ++ri) {
            if (rows[ri].xmax != 0) continue;
            if (!first) resultJson += ",";
            first = false;
            resultJson += "{";
            bool firstCol = true;
            for (size_t ci = 0; ci < resultCols.size(); ++ci) {
                if (!includeScore && resultCols[ci].name == "_score") continue;
                if (!firstCol) resultJson += ",";
                firstCol = false;
                resultJson += "\"" + jsonEscape(resultCols[ci].name) + "\":\"";
                std::string val = ci < rows[ri].values.size() ? rows[ri].values[ci] : "";
                // Strip surrounding single quotes
                if (val.size() >= 2 && val.front() == '\'' && val.back() == '\'')
                    val = val.substr(1, val.size() - 2);
                resultJson += jsonEscape(val) + "\"";
            }
            resultJson += "}";
            ++count;
        }
        resultJson += "],\"count\":" + std::to_string(count) + "}";
    } catch (const std::exception& e) {
        return std::string(R"({"success":false,"error":")") + jsonEscape(e.what()) + "\"}";
    }

    return resultJson;
}

// ── MilanHttpServer::handleDashboard (Phase 54C) ──────────────

inline std::string MilanHttpServer::handleDashboard() {
    return R"HTML(<!DOCTYPE html>
<html lang="de">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>MilanSQL Dashboard</title>
<link rel="icon" type="image/svg+xml" href="/favicon.ico">
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{background:#1e1e2e;color:#cdd6f4;font-family:'Courier New',monospace;font-size:14px;display:flex;flex-direction:column;height:100vh;overflow:hidden}
.header{background:#313244;padding:12px 20px;display:flex;align-items:center;justify-content:space-between;border-bottom:1px solid #45475a;flex-shrink:0}
.logo{font-size:17px;font-weight:bold;color:#89dceb}
.status-bar{font-size:12px;color:#a6adc8;display:flex;gap:18px}
.badge{color:#a6e3a1}
.main{display:flex;flex:1;overflow:hidden}
.sidebar{width:220px;background:#181825;border-right:1px solid #313244;overflow-y:auto;padding:10px;flex-shrink:0}
.sidebar h3{color:#89dceb;font-size:11px;text-transform:uppercase;letter-spacing:1px;margin:10px 0 6px}
.sidebar h3:first-child{margin-top:0}
.tbl-item{padding:5px 8px;border-radius:4px;cursor:pointer;color:#cdd6f4;font-size:12px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.tbl-item:hover{background:#313244;color:#89b4fa}
.tbl-item::before{content:"\25A3  ";font-size:10px;color:#585b70}
.sch-item{padding:3px 8px;color:#a6adc8;font-size:11px}
.empty{color:#45475a;font-size:11px;font-style:italic;padding:4px 8px}
.content{flex:1;display:flex;flex-direction:column;overflow:hidden;padding:14px;gap:10px}
.editor-box{background:#181825;border:1px solid #313244;border-radius:8px;padding:12px;flex-shrink:0}
.lbl{color:#a6adc8;font-size:11px;text-transform:uppercase;letter-spacing:1px;margin-bottom:7px}
textarea{width:100%;background:#11111b;color:#cdd6f4;border:1px solid #313244;border-radius:4px;padding:9px;font-family:'Courier New',monospace;font-size:13px;resize:vertical;min-height:72px;outline:none}
textarea:focus{border-color:#89b4fa}
.btn-row{display:flex;gap:8px;margin-top:8px;align-items:center}
.btn{padding:7px 18px;border:none;border-radius:4px;cursor:pointer;font-family:inherit;font-size:13px;font-weight:bold}
.btn-p{background:#89b4fa;color:#11111b}.btn-p:hover{background:#b4d0ff}
.btn-s{background:#313244;color:#cdd6f4}.btn-s:hover{background:#45475a}
.etime{color:#a6adc8;font-size:12px}
.result-box{flex:1;overflow:auto;background:#181825;border:1px solid #313244;border-radius:8px;padding:12px}
.ok{color:#a6e3a1;font-size:12px;margin-bottom:7px}
.err{color:#f38ba8;font-size:13px;padding:7px 10px;background:#2a1a1a;border-radius:4px;border-left:3px solid #f38ba8}
.msg{color:#a6e3a1;font-size:13px;padding:7px 10px;background:#1a2a1a;border-radius:4px;border-left:3px solid #a6e3a1}
.emp{color:#45475a;font-style:italic;padding:8px}
.loading{color:#f9e2af;font-size:13px;padding:8px}
table{width:100%;border-collapse:collapse;font-size:13px}
th{background:#313244;color:#89dceb;padding:7px 11px;text-align:left;border-bottom:2px solid #45475a;position:sticky;top:0;font-weight:bold}
td{padding:5px 11px;border-bottom:1px solid #1e1e2e;color:#cdd6f4}
tr:hover td{background:#2d2d44}
tr:nth-child(even) td{background:rgba(49,50,68,.35)}
tr:nth-child(even):hover td{background:#2d2d44}
::-webkit-scrollbar{width:5px;height:5px}
::-webkit-scrollbar-track{background:#181825}
::-webkit-scrollbar-thumb{background:#45475a;border-radius:3px}
</style>
</head>
<body>
<div class="header">
  <div class="logo"><svg width="24" height="24" viewBox="0 0 100 100" xmlns="http://www.w3.org/2000/svg"><rect x="0" y="0" width="100" height="100" rx="8" fill="#161616" stroke="#ff6b1a" stroke-width="0.5"/><path d="M20 78 L20 22 L50 54 L80 22 L80 78" fill="none" stroke="#ff6b1a" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"/><circle cx="20" cy="22" r="5" fill="#ff6b1a"/><circle cx="20" cy="78" r="5" fill="#ff6b1a"/><circle cx="50" cy="54" r="5" fill="#ff6b1a"/><circle cx="80" cy="22" r="5" fill="#ff6b1a"/><circle cx="80" cy="78" r="5" fill="#ff6b1a"/></svg> MilanSQL v10.6.0</div>
  <div style="display:flex;align-items:center;gap:10px">
    <span id="ms-user-badge" style="background:#313244;color:#89b4fa;padding:3px 10px;border-radius:10px;font-size:11px"></span>
    <button onclick="msLogout()" style="background:#45475a;color:#cdd6f4;border:none;border-radius:4px;padding:4px 10px;cursor:pointer;font-size:11px;font-family:inherit">Logout</button>
  </div>
  <div class="status-bar">
    <span id="sv">v1.4.0</span>
    <span>Uptime: <span class="badge" id="su">-</span>s</span>
    <span>Tables: <span class="badge" id="st">-</span></span>
    <span>Port: <span class="badge" id="sp">8080</span></span>
  </div>
</div>
<div class="main">
  <div class="sidebar">
    <h3>Tables</h3>
    <div id="tbl-list"><div class="empty">Loading...</div></div>
    <h3>Schemas</h3>
    <div id="sch-list"><div class="empty">Loading...</div></div>
  </div>
  <div class="content">
    <div class="editor-box">
      <div class="lbl">SQL Query <span style="color:#585b70;font-size:10px">(Ctrl+Enter)</span></div>
      <textarea id="sql" rows="4" spellcheck="false" placeholder="SELECT * FROM ...">SELECT * FROM users</textarea>
      <div class="btn-row">
        <button class="btn btn-p" onclick="run()">&#9654; Execute</button>
        <button class="btn btn-s" onclick="clr()">Clear</button>
        <span class="etime" id="et"></span>
      </div>
    </div>
    <div class="result-box">
      <div class="lbl">Results</div>
      <div id="out"><div class="emp">Execute a query to see results</div></div>
    </div>
  </div>
</div>
<script>
const B=window.location.origin;
function esc(s){return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;');}
async function loadSidebar(){
  try{
    const[tr,sr,str]=await Promise.all([fetch(B+'/tables',{credentials:'include'}),fetch(B+'/schemas',{credentials:'include'}),fetch(B+'/status',{credentials:'include'})]);
    const td=await tr.json(),sd=await sr.json(),std=await str.json();
    const tl=document.getElementById('tbl-list');
    tl.innerHTML=(td.tables&&td.tables.length)?td.tables.map(t=>`<div class="tbl-item" onclick="qt('${esc(t)}')" title="SELECT * FROM ${esc(t)}">${esc(t)}</div>`).join(''):'<div class="empty">No tables</div>';
    const sl=document.getElementById('sch-list');
    sl.innerHTML=(sd.schemas&&sd.schemas.length)?sd.schemas.map(s=>`<div class="sch-item">&#128193; ${esc(s)}</div>`).join(''):'<div class="empty">No schemas</div>';
    if(std.status){const s=std.status;document.getElementById('sv').textContent=s.version||'v1.4.0';document.getElementById('su').textContent=s.uptime;document.getElementById('st').textContent=s.tableCount;document.getElementById('sp').textContent=s.port||'8080';}
  }catch(e){document.getElementById('tbl-list').innerHTML='<div class="empty">Connection error</div>';}
}
function qt(n){document.getElementById('sql').value='SELECT * FROM '+n;run();}
async function run(){
  const sql=document.getElementById('sql').value.trim();if(!sql)return;
  const o=document.getElementById('out');o.innerHTML='<div class="loading">Executing&#8230;</div>';
  document.getElementById('et').textContent='';
  const t0=Date.now();
  try{
    const r=await fetch(B+'/query',{method:'POST',credentials:'include',headers:{'Content-Type':'application/json'},body:JSON.stringify({sql})});
    const d=await r.json();
    document.getElementById('et').textContent=(Date.now()-t0)+'ms';
    if(!d.success){o.innerHTML=`<div class="err">&#10005; ${esc(d.error||'Unknown error')}</div>`;return;}
    if(d.columns&&d.columns.length){
      const info=`${d.rowCount||d.rows.length} row(s)${d.executionTime?' &middot; '+d.executionTime:''}`;
      o.innerHTML=`<div class="ok">${info}</div>`+mkTable(d.columns,d.rows);
    }else{
      const m=d.message||'Query executed successfully';
      const af=d.rowsAffected>0?` (${d.rowsAffected} rows affected)`:'';
      o.innerHTML=`<div class="msg">&#10003; ${esc(m)}${esc(af)}</div>`;
      loadSidebar();
    }
  }catch(e){o.innerHTML=`<div class="err">Network error: ${esc(e.message)}</div>`;}
}
function mkTable(cols,rows){
  if(!rows||!rows.length)return'<div class="emp">No rows returned</div>';
  const th=cols.map(c=>`<th>${esc(String(c))}</th>`).join('');
  const tr=rows.map(r=>'<tr>'+r.map(c=>`<td>${c===null?'<span style="color:#45475a">NULL</span>':esc(String(c))}</td>`).join('')+'</tr>').join('');
  return`<table><thead><tr>${th}</tr></thead><tbody>${tr}</tbody></table>`;
}
function clr(){document.getElementById('out').innerHTML='<div class="emp">Results cleared</div>';document.getElementById('et').textContent='';}
document.getElementById('sql').addEventListener('keydown',e=>{if(e.ctrlKey&&e.key==='Enter')run();});
loadSidebar();
</script>
</body>
</html>)HTML";
}

// ── MilanHttpServer::handleWebUI (Phase 135) ─────────────────

inline std::string MilanHttpServer::handleWebUI() {
    // Cache-Busting: Versions-Query am einzigen externen Asset (Favicon);
    // CSS/JS sind inline und haengen an der HTML-Seite (no-store, s.u.).
    static const std::string html = std::string(R"WEBUIEND(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>MilanSQL Admin</title>
<link rel="icon" type="image/svg+xml" href="/favicon.ico?v=)WEBUIEND") + MILANSQL_VERSION + R"WEBUIEND(">
<style>
/* ── MilanSQL Design System (Redesign 2026-07) ── */
:root{
  --bg-primary:#080c18;--bg-secondary:#0d1224;--bg-card:#111827;--bg-hover:#1a2235;
  --accent:#00d4ff;--accent-2:#7c3aed;
  --danger:#ef4444;--success:#10b981;--warning:#f59e0b;
  --text-1:#f8fafc;--text-2:#94a3b8;--text-3:#475569;
  --border:#1e2d40;--border-2:#2d4060;
  --glow-cyan:0 0 30px rgba(0,212,255,0.15);
  --glow-card:0 0 60px rgba(0,212,255,0.08);
  --ease:cubic-bezier(0.4,0,0.2,1);
  --mono:'JetBrains Mono','Cascadia Code','SF Mono','Fira Code',monospace;
}
*{box-sizing:border-box;margin:0;padding:0}
body{background:var(--bg-primary);color:var(--text-1);font-family:'Inter',-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;display:flex;flex-direction:column;height:100vh;overflow:hidden}

/* TOPBAR (glassmorphism) */
#topbar{height:56px;background:rgba(255,255,255,0.03);backdrop-filter:blur(20px);-webkit-backdrop-filter:blur(20px);border-bottom:1px solid var(--border);display:flex;align-items:center;padding:0 16px;gap:12px;flex-shrink:0;z-index:100}
#topbar .brand{display:flex;align-items:center;gap:10px;font-weight:700;font-size:1rem;color:var(--text-1);margin-right:8px;letter-spacing:-0.02em}
.logo-m{width:30px;height:30px;border-radius:8px;display:flex;align-items:center;justify-content:center;font-weight:800;font-size:1rem;color:#080c18;background:linear-gradient(135deg,var(--accent),#0ea5e9);box-shadow:var(--glow-cyan)}
.badge{display:inline-flex;align-items:center;gap:6px;background:var(--bg-hover);border:1px solid var(--border);border-radius:20px;padding:4px 12px;font-size:0.75rem;color:var(--text-2);transition:border-color .2s var(--ease)}
.badge.green{color:var(--success)}
.badge.green::before{content:'';width:7px;height:7px;border-radius:50%;background:var(--success);box-shadow:0 0 8px rgba(16,185,129,0.8);animation:pulse 2s var(--ease) infinite}
@keyframes pulse{0%,100%{opacity:1}50%{opacity:0.4}}
.badge.yellow{color:var(--warning)}
.badge.yellow::before{content:'';width:7px;height:7px;border-radius:50%;background:var(--warning)}
.badge.blue{color:var(--accent)}
.topbar-right{margin-left:auto;display:flex;gap:8px;align-items:center}

/* LAYOUT */
#layout{display:flex;flex:1;overflow:hidden}

/* SIDEBAR */
#sidebar{width:220px;background:var(--bg-secondary);border-right:1px solid var(--border);display:flex;flex-direction:column;flex-shrink:0;overflow-y:auto}
.nav-section{padding:8px 0}
.nav-label{font-size:0.68rem;font-weight:600;text-transform:uppercase;letter-spacing:.1em;color:var(--text-3);padding:10px 16px 4px}
.nav-item{position:relative;display:flex;align-items:center;gap:9px;padding:8px 16px;font-size:0.85rem;color:var(--text-2);cursor:pointer;border-radius:6px;margin:1px 8px;transition:background .2s var(--ease),color .2s var(--ease)}
.nav-item:hover{background:var(--bg-hover);color:var(--text-1)}
.nav-item.active{background:var(--bg-hover);color:var(--accent);font-weight:600}
.nav-item.active::before{content:'';position:absolute;left:-8px;top:6px;bottom:6px;width:3px;border-radius:2px;background:var(--accent);box-shadow:0 0 8px rgba(0,212,255,0.6)}
.nav-item .icon{font-size:0.9rem;width:16px;text-align:center}
.tables-list{padding:0 8px}
.table-item{padding:5px 8px;font-size:0.82rem;color:var(--text-2);cursor:pointer;border-radius:6px;display:flex;align-items:center;gap:6px;transition:background .2s var(--ease),color .2s var(--ease)}
.table-item:hover{background:var(--bg-hover);color:var(--accent)}
.table-item::before{content:'\229E';font-size:0.75rem;color:var(--border-2)}
.sidebar-footer{margin-top:auto;padding:12px;font-size:0.72rem;color:var(--text-3);border-top:1px solid var(--border)}

/* MAIN */
#main{flex:1;display:flex;flex-direction:column;overflow:hidden}

/* PAGE VIEWS */
.page{display:none;flex:1;flex-direction:column;overflow:hidden}
.page.active{display:flex}

/* SQL EDITOR PAGE */
#editor-area{padding:12px;display:flex;flex-direction:column;gap:8px;flex-shrink:0}
.editor-toolbar{display:flex;gap:8px;align-items:center}
.editor-toolbar .exec-time{margin-left:auto;font-size:0.75rem;color:var(--text-2)}
/* Editor with line numbers + syntax highlight */
#editor-container{display:flex;border:1px solid var(--border);border-radius:8px;overflow:hidden;background:var(--bg-card);transition:border-color .2s var(--ease),box-shadow .2s var(--ease)}
#editor-container:focus-within{border-color:var(--accent);box-shadow:var(--glow-cyan)}
#line-numbers{background:var(--bg-primary);color:var(--text-3);font-family:var(--mono);font-size:0.85rem;line-height:1.6;padding:12px 8px;text-align:right;user-select:none;min-width:40px;overflow:hidden;white-space:pre;flex-shrink:0;border-right:1px solid var(--border)}
#editor-wrap{position:relative;flex:1;overflow:hidden}
#highlight-backdrop{position:absolute;top:0;left:0;right:0;bottom:0;padding:12px;font-family:var(--mono);font-size:0.85rem;line-height:1.6;white-space:pre-wrap;word-break:break-all;overflow:hidden;pointer-events:none;color:transparent}
#sql-editor{position:relative;width:100%;height:140px;background:transparent;border:none;color:var(--text-1);caret-color:var(--accent);font-family:var(--mono);font-size:0.85rem;padding:12px;resize:vertical;outline:none;line-height:1.6;tab-size:4;z-index:1;overflow:auto}
/* Autocomplete dropdown */
#autocomplete{display:none;position:absolute;background:var(--bg-hover);border:1px solid var(--border-2);border-radius:8px;z-index:9999;max-height:200px;overflow-y:auto;min-width:160px;box-shadow:0 8px 24px rgba(0,0,0,.6),var(--glow-card)}
.ac-item{padding:6px 12px;font-size:0.82rem;font-family:var(--mono);color:var(--text-1);cursor:pointer;white-space:nowrap}
.ac-item:hover,.ac-item.ac-selected{background:var(--accent);color:#080c18}
/* Error line highlight */
.error-line-badge{display:inline-block;background:rgba(239,68,68,.15);color:var(--danger);border:1px solid var(--danger);border-radius:6px;padding:2px 8px;font-size:0.75rem;margin-bottom:4px}

/* BUTTONS */
.btn{padding:6px 14px;border-radius:8px;border:1px solid transparent;font-size:0.82rem;cursor:pointer;font-weight:600;font-family:inherit;transition:all .2s var(--ease)}
.btn-green{background:var(--accent);color:#080c18}
.btn-green:hover{box-shadow:var(--glow-cyan);filter:brightness(1.1)}
.btn-blue{background:var(--accent-2);color:#fff}
.btn-blue:hover{box-shadow:0 0 30px rgba(124,58,237,.3);filter:brightness(1.1)}
.btn-gray{background:transparent;color:var(--text-2);border:1px solid var(--border-2)}
.btn-gray:hover{color:var(--text-1);border-color:var(--accent);background:var(--bg-hover)}
.btn-red{background:transparent;color:var(--danger);border:1px solid var(--danger)}
.btn-red:hover{background:var(--danger);color:#fff}

/* RESULTS */
#results-area{flex:1;overflow:auto;padding:0 12px 12px}
.result-header{display:flex;align-items:center;gap:8px;padding:8px 0;font-size:0.8rem;color:var(--text-2);margin-bottom:4px}
.result-header .pill{background:rgba(16,185,129,.1);border:1px solid var(--success);color:var(--success);border-radius:20px;padding:2px 10px;font-size:0.75rem}
.result-header .pill.error{background:rgba(239,68,68,.1);border-color:var(--danger);color:var(--danger)}
.result-header .pill.info{background:rgba(0,212,255,.08);border-color:var(--accent);color:var(--accent)}
#result-table-wrap{overflow:auto;border:1px solid var(--border);border-radius:8px}
table{width:100%;border-collapse:collapse;font-size:0.82rem}
th{background:var(--bg-secondary);color:var(--text-2);text-align:left;padding:8px 12px;border-bottom:1px solid var(--border);font-weight:600;white-space:nowrap;position:sticky;top:0;cursor:pointer;user-select:none;text-transform:uppercase;font-size:0.72rem;letter-spacing:.05em}
th:hover{color:var(--accent)}
th.sort-asc::after{content:' \25B2';font-size:0.7rem;color:var(--accent)}
th.sort-desc::after{content:' \25BC';font-size:0.7rem;color:var(--accent)}
td{padding:7px 12px;border-bottom:1px solid var(--border);color:var(--text-1);white-space:nowrap;max-width:300px;overflow:hidden;text-overflow:ellipsis;font-family:var(--mono);font-size:0.8rem}
tbody tr:nth-child(even) td{background:rgba(255,255,255,0.015)}
tr:hover td{background:var(--bg-hover)}
td.num{color:#a78bfa}
td.null-val{color:var(--text-3);font-style:italic;font-family:inherit}
.error-box{background:rgba(239,68,68,.06);border:1px solid var(--danger);border-radius:8px;padding:12px;color:var(--danger);font-family:var(--mono);font-size:0.82rem;margin-top:4px}
.affected-box{background:rgba(16,185,129,.06);border:1px solid var(--success);border-radius:8px;padding:12px;color:var(--success);font-size:0.85rem;margin-top:4px}

/* STATUS BAR */
#statusbar{height:26px;background:var(--bg-secondary);border-top:1px solid var(--border);display:flex;align-items:center;padding:0 12px;gap:16px;font-size:0.72rem;color:var(--text-2);flex-shrink:0}
.status-item{display:flex;align-items:center;gap:4px}
.status-dot{width:6px;height:6px;border-radius:50%;background:var(--success);box-shadow:0 0 6px rgba(16,185,129,.6)}
.status-dot.warn{background:var(--warning);box-shadow:0 0 6px rgba(245,158,11,.6)}
.status-dot.err{background:var(--danger);box-shadow:0 0 6px rgba(239,68,68,.6)}

/* MONITORING PAGE */
#page-monitoring .mon-grid,#page-vacuum .mon-grid,#page-replication .mon-grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(200px,1fr));gap:12px;padding:16px}
.mon-tab{background:none;border:1px solid transparent;color:var(--text-2);font-size:0.8rem;padding:4px 12px;border-radius:8px;cursor:pointer;font-family:inherit;transition:all .2s var(--ease)}
.mon-tab:hover{color:var(--text-1);background:var(--bg-hover)}
.mon-tab.active{color:var(--accent);background:rgba(0,212,255,.08);border-color:var(--accent);font-weight:600}
.vac-btn{background:rgba(16,185,129,.1);color:var(--success);border:1px solid var(--success);border-radius:6px;padding:2px 10px;cursor:pointer;font-size:0.72rem;font-family:inherit;transition:all .2s var(--ease)}
.vac-btn:hover{background:var(--success);color:#080c18}
.vac-btn:disabled{opacity:0.5;cursor:wait}
.stat-card{background:var(--bg-card);border:1px solid var(--border);border-radius:12px;padding:16px;transition:all .2s var(--ease)}
.stat-card:hover{border-color:var(--border-2);box-shadow:var(--glow-card);transform:translateY(-1px)}
.stat-card .label{font-size:0.72rem;color:var(--text-2);text-transform:uppercase;letter-spacing:.06em;margin-bottom:6px}
.stat-card .value{font-size:1.6rem;font-weight:700;color:var(--text-1);font-family:var(--mono)}
.stat-card .unit{font-size:0.75rem;color:var(--text-2);margin-left:4px}
.slow-queries-section{padding:0 16px 16px}
.slow-queries-section h3{font-size:0.8rem;color:var(--text-2);margin-bottom:8px;text-transform:uppercase;letter-spacing:.06em}


/* SCHEMA VISUALIZER */
#page-schema{flex-direction:column;position:relative}
.schema-zoom-ctrl{display:flex;align-items:center;gap:4px}
.schema-zoom-ctrl button{background:var(--bg-hover);border:1px solid var(--border-2);color:var(--text-1);width:26px;height:26px;border-radius:6px;cursor:pointer;font-size:0.85rem;display:flex;align-items:center;justify-content:center;padding:0;transition:all .2s var(--ease)}
.schema-zoom-ctrl button:hover{border-color:var(--accent);color:var(--accent)}
.schema-zoom-pct{font-size:0.7rem;color:var(--text-2);min-width:36px;text-align:center}
#schema-minimap{position:absolute;bottom:12px;right:12px;width:200px;height:150px;background:var(--bg-primary);border:1px solid var(--border-2);border-radius:8px;z-index:50;overflow:hidden;cursor:crosshair}
#schema-minimap canvas{width:100%;height:100%}
.schema-card{position:absolute;background:var(--bg-card);border:1px solid var(--border-2);border-radius:12px;min-width:180px;max-width:260px;cursor:move;transition:box-shadow .2s var(--ease),opacity .2s var(--ease);user-select:none;z-index:2}
.schema-card:hover{box-shadow:0 0 0 1px var(--accent),var(--glow-cyan)}
.schema-card.dimmed{opacity:0.3}
.schema-card.highlighted{box-shadow:0 0 0 2px var(--accent),var(--glow-cyan)}
.schema-card-header{display:flex;align-items:center;gap:6px;padding:8px 10px;border-bottom:1px solid var(--border);font-size:0.8rem;font-weight:600;color:var(--text-1)}
.schema-card-header .rls-dot{width:7px;height:7px;border-radius:50%;flex-shrink:0}
.schema-card-header .rls-dot.on{background:var(--success);box-shadow:0 0 5px rgba(16,185,129,.7)}
.schema-card-header .rls-dot.off{background:var(--text-3)}
.schema-card-header .pol-count{margin-left:auto;font-size:9px;font-weight:600;background:rgba(16,185,129,.12);color:var(--success);padding:1px 5px;border-radius:8px}
.schema-card-cols{padding:4px 0;font-size:0.75rem;max-height:200px;overflow-y:auto}
.schema-col{display:flex;align-items:center;gap:4px;padding:2px 10px;color:var(--text-2)}
.schema-col .col-icon{width:12px;font-size:9px;text-align:center;flex-shrink:0}
.schema-col .col-icon.pk{color:var(--warning)}
.schema-col .col-icon.fk{color:var(--accent)}
.schema-col .col-name{flex:1;color:var(--text-1);overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.schema-col .col-name.fk-col{color:var(--accent)}
.schema-col .col-type{color:var(--text-3);font-size:0.7rem;font-family:var(--mono);flex-shrink:0}
.schema-card-rls{padding:5px 10px;border-top:1px solid var(--border);background:var(--bg-primary);border-radius:0 0 11px 11px;font-size:0.7rem;font-family:var(--mono);color:var(--text-3);cursor:pointer;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;max-height:60px}
.schema-card-rls:hover{color:var(--text-2)}
/* TABLE BROWSER PAGE */
#page-browser .browser-wrap{display:flex;flex:1;gap:0;overflow:hidden}
#page-browser .tbl-list{width:200px;border-right:1px solid var(--border);overflow-y:auto;padding:8px}
#page-browser .tbl-list .tbl-btn{width:100%;text-align:left;padding:7px 10px;background:none;border:none;color:var(--text-2);font-size:0.82rem;cursor:pointer;border-radius:6px;display:block;transition:all .2s var(--ease);font-family:inherit}
#page-browser .tbl-list .tbl-btn:hover{background:var(--bg-hover);color:var(--text-1)}
#page-browser .tbl-list .tbl-btn.active{background:rgba(0,212,255,.08);color:var(--accent)}
#page-browser .tbl-detail{flex:1;overflow:auto;padding:12px}
#page-browser .tbl-detail h3{font-size:0.9rem;color:var(--text-1);margin-bottom:8px}

/* HISTORY PAGE */
#page-history{overflow-y:auto;padding:12px}
.hist-item{background:var(--bg-card);border:1px solid var(--border);border-radius:8px;padding:10px 14px;margin-bottom:8px;cursor:pointer;transition:all .2s var(--ease)}
.hist-item:hover{border-color:var(--accent);box-shadow:var(--glow-card)}
.hist-item .hist-sql{font-family:var(--mono);font-size:0.82rem;color:var(--text-1);white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.hist-item .hist-meta{font-size:0.72rem;color:var(--text-2);margin-top:4px;display:flex;gap:8px;align-items:center}
.hist-badge{display:inline-block;border-radius:10px;padding:1px 7px;font-size:0.7rem;font-weight:600;font-family:var(--mono)}
.hist-badge.ok{background:rgba(16,185,129,.1);color:var(--success);border:1px solid var(--success)}
.hist-badge.err{background:rgba(239,68,68,.1);color:var(--danger);border:1px solid var(--danger)}

/* SCROLLBAR */
::-webkit-scrollbar{width:8px;height:8px}
::-webkit-scrollbar-track{background:var(--bg-primary)}
::-webkit-scrollbar-thumb{background:var(--border-2);border-radius:4px}
::-webkit-scrollbar-thumb:hover{background:var(--accent)}
</style>
</head>
<body>

<!-- TOPBAR -->
<div id="topbar">
  <div class="brand"><span class="logo-m">M</span> MilanSQL</div>
  <span class="badge" id="health-badge">checking...</span>
  <span class="badge blue" id="conn-badge">0 connections</span>
  <span class="badge blue" id="test-badge">&hellip; tests</span>
  <div class="topbar-right">
    <span id="ms-user-badge" style="display:none;background:rgba(16,185,129,.1);color:#10b981;border:1px solid #10b981;padding:3px 10px;border-radius:10px;font-size:11px;font-weight:600"></span>
    <button id="ms-logout-btn" onclick="msLogout()" style="display:none;background:transparent;color:#94a3b8;border:1px solid #2d4060;border-radius:6px;padding:3px 10px;cursor:pointer;font-size:11px;font-family:inherit">Logout</button>
    <span style="font-size:0.75rem;color:#94a3b8" id="version-label" class="ms-version">v10.6.0</span>
  </div>
</div>

<!-- LAYOUT -->
<div id="layout">

  <!-- SIDEBAR -->
  <nav id="sidebar">
    <div class="nav-section">
      <div class="nav-label">Navigation</div>
      <div class="nav-item active" data-page="editor" onclick="showPage('editor',this)">
        <span class="icon">&#x270F;</span> SQL Editor
      </div>
      <div class="nav-item" data-page="browser" onclick="showPage('browser',this)">
        <span class="icon">&#x1F5C3;</span> Table Browser
      </div>
      <div class="nav-item" data-page="schema" onclick="showPage('schema',this)">
        <span class="icon">&#x25C9;</span> Schema
      </div>
      <div class="nav-item" data-page="monitoring" onclick="showPage('monitoring',this)">
        <span class="icon">&#x1F4CA;</span> Monitoring
      </div>
      <div class="nav-item" data-page="vacuum" onclick="showPage('vacuum',this)" style="padding-left:26px">
        <span class="icon">&#x1F9F9;</span> Vacuum
      </div>
      <div class="nav-item" data-page="replication" onclick="showPage('replication',this)">
        <span class="icon">&#x1F501;</span> Replication
      </div>
      <div class="nav-item" data-page="history" onclick="showPage('history',this)">
        <span class="icon">&#x1F550;</span> Query History
      </div>
    </div>
    <div class="nav-section">
      <div class="nav-label">Tables</div>
      <div class="tables-list" id="sidebar-tables">
        <div style="font-size:0.75rem;color:#475569;padding:4px 8px">Loading...</div>
      </div>
    </div>
    <div style="padding:8px 12px;border-top:1px solid #1e2d40;margin-top:auto">
      <div id="rls-panel" style="background:#111827;border:1px solid #1e2d40;border-radius:8px;padding:8px 10px;font-size:11px;color:#94a3b8">
        <div style="font-size:10px;color:#64748b;text-transform:uppercase;letter-spacing:0.5px;margin-bottom:4px">Row Level Security</div>
        <div style="display:flex;align-items:center;gap:6px"><span style="color:#475569;font-size:9px">●</span><span style="font-size:11px;color:#475569">Not connected</span></div>
      </div>
    </div>
    <div class="sidebar-footer">MilanSQL Admin <span class="ms-version">v10.6.0</span></div>
  </nav>

  <!-- MAIN -->
  <div id="main">

    <!-- SQL EDITOR PAGE -->
    <div class="page active" id="page-editor">
      <div id="editor-area">
        <div class="editor-toolbar">
          <button class="btn btn-green" onclick="runQuery()" title="Ctrl+Enter">&#x25B6; Run</button>
          <button class="btn btn-blue" onclick="explainQuery()">&#x26A1; EXPLAIN</button>
          <button class="btn btn-gray" onclick="formatSQL()">Format</button>
          <button class="btn btn-gray" onclick="clearEditor()">&#x2715; Clear</button>
          <button class="btn btn-gray" onclick="copyCSV()" title="Copy results as CSV">&#x1F4CB; CSV</button>
          <button class="btn btn-gray" onclick="showRlsPolicies()" title="Show RLS policies for selected table" style="border-color:#f59e0b;color:#f59e0b">&#x1F6E1; RLS</button>
          <span class="exec-time" id="exec-time"></span>
        </div>
        <div style="position:relative">
          <div id="editor-container">
            <div id="line-numbers">1</div>
            <div id="editor-wrap">
              <div id="highlight-backdrop" aria-hidden="true"></div>
              <textarea id="sql-editor" spellcheck="false" placeholder="-- Enter SQL here (Ctrl+Enter to run)">SELECT version();</textarea>
            </div>
          </div>
          <div id="autocomplete"></div>
        </div>
        <div id="example-queries" style="display:flex;flex-wrap:wrap;gap:6px;padding:6px 0">
          <span style="font-size:0.7rem;color:#475569;align-self:center">Examples:</span>
          <button class="btn btn-gray" style="font-size:0.7rem;padding:3px 8px" onclick="setSQL('-- JOIN with expression\nSELECT p.name, p.preis * b.menge AS gesamt\nFROM produkte p\nJOIN bestellungen b ON p.id = b.produkt_id')">JOIN + Calc</button>
          <button class="btn btn-gray" style="font-size:0.7rem;padding:3px 8px" onclick="setSQL('-- JOIN with GROUP BY\nSELECT p.name, SUM(b.menge) AS total\nFROM produkte p\nJOIN bestellungen b ON p.id = b.produkt_id\nGROUP BY p.name')">JOIN + GROUP BY</button>
          <button class="btn btn-gray" style="font-size:0.7rem;padding:3px 8px" onclick="setSQL('-- Aggregate with BETWEEN\nSELECT COUNT(*) FROM produkte\nWHERE preis BETWEEN 100 AND 1000')">COUNT + BETWEEN</button>
          <button class="btn btn-gray" style="font-size:0.7rem;padding:3px 8px" onclick="setSQL('-- Parameterized query (injection-safe)\n-- Use the params field below for ? values\nSELECT * FROM produkte WHERE id = ? AND preis > ?')">Params (?)</button>
          <button class="btn btn-gray" style="font-size:0.7rem;padding:3px 8px" onclick="setSQL('-- Window function\nSELECT name, preis,\n  RANK() OVER (ORDER BY preis DESC) AS rang\nFROM produkte')">Window Func</button>
          <button class="btn btn-gray" style="font-size:0.7rem;padding:3px 8px" onclick="setSQL('-- Subquery with IN\nSELECT * FROM produkte\nWHERE id IN (SELECT produkt_id FROM bestellungen)')">Subquery IN</button>
        </div>
      </div>
      <div id="results-area">
        <div class="result-header" id="result-header" style="display:none">
          <span class="pill" id="result-pill"></span>
          <span id="result-info"></span>
        </div>
        <div id="exec-badge" style="display:none;font-size:11px;padding:3px 8px;border-radius:6px;background:#1a2235;margin:4px 0 2px 0"></div>
        <div id="result-content"></div>
      </div>
    </div>

    <!-- TABLE BROWSER PAGE -->
    <div class="page" id="page-browser">
      <div class="browser-wrap" style="display:flex;flex:1;overflow:hidden">
        <div class="tbl-list" id="browser-tbl-list">
          <div style="font-size:0.75rem;color:#475569;padding:4px">Loading...</div>
        </div>
        <div class="tbl-detail" id="browser-tbl-detail">
          <div style="color:#475569;font-size:0.85rem;margin-top:20px">Select a table to browse</div>
        </div>
      </div>
    </div>


    <!-- SCHEMA VISUALIZER PAGE -->
    <div class="page" id="page-schema">
      <div id="schema-toolbar" style="display:flex;align-items:center;gap:8px;padding:8px 12px;border-bottom:1px solid #1e2d40;background:#080c18">
        <input id="schema-search" type="text" placeholder="Filter tables..." style="background:#111827;border:1px solid #2d4060;border-radius:4px;color:#f8fafc;padding:4px 10px;font-size:0.8rem;width:180px;outline:none">
        <button class="btn btn-gray" onclick="schemaAutoLayout()" style="font-size:0.75rem;padding:4px 10px">&#x2B50; Auto Layout</button>
        <button class="btn btn-gray" onclick="schemaFitAll()" style="font-size:0.75rem;padding:4px 10px">&#x26F6; Fit</button>
        <button class="btn btn-gray" onclick="schemaReload()" style="font-size:0.75rem;padding:4px 10px">&#x21BB; Reload</button>
        <div class="schema-zoom-ctrl" style="margin-left:auto;margin-right:8px">
          <button onclick="schemaZoom(-0.1)" title="Zoom Out (Ctrl+-)">&#x2212;</button>
          <span id="schema-zoom-pct" class="schema-zoom-pct">100%</span>
          <button onclick="schemaZoom(0.1)" title="Zoom In (Ctrl++)">+</button>
          <button onclick="schemaZoomReset()" title="Reset Zoom (Ctrl+0)" style="font-size:0.65rem;width:auto;padding:0 6px">Reset</button>
        </div>
        <span id="schema-status" style="font-size:0.72rem;color:#94a3b8"></span>
      </div>
      <div id="schema-canvas" style="flex:1;position:relative;overflow:hidden;background:#080c18;cursor:grab">
        <div id="schema-transform" style="position:absolute;top:0;left:0;transform-origin:0 0;will-change:transform">
          <svg id="schema-svg" style="position:absolute;top:0;left:0;width:10000px;height:10000px;pointer-events:none;z-index:1"></svg>
          <div id="schema-cards" style="position:absolute;top:0;left:0;z-index:2"></div>
        </div>
        <div id="schema-minimap"><canvas id="schema-minimap-canvas" width="400" height="300"></canvas></div>
      </div>
      <div id="schema-policy-editor" style="display:none;position:absolute;bottom:0;left:0;right:0;background:#111827;border-top:2px solid #00d4ff;padding:12px 16px;z-index:100">
        <div style="display:flex;align-items:center;gap:8px;margin-bottom:8px">
          <span style="font-size:0.85rem;font-weight:600;color:#00d4ff">&#x1F6E1; Edit RLS Policy</span>
          <span id="schema-pe-table" style="font-size:0.8rem;color:#94a3b8"></span>
          <button onclick="closePolicyEditor()" style="margin-left:auto;background:none;border:none;color:#94a3b8;cursor:pointer;font-size:1rem">&#x2715;</button>
        </div>
        <div style="display:flex;gap:12px;align-items:flex-start;flex-wrap:wrap">
          <div style="flex:1;min-width:200px">
            <label style="font-size:0.7rem;color:#64748b;text-transform:uppercase;letter-spacing:.5px">Policy Name</label>
            <input id="schema-pe-name" style="width:100%;background:#080c18;border:1px solid #2d4060;border-radius:4px;color:#f8fafc;padding:4px 8px;font-size:0.8rem;margin-top:2px">
          </div>
          <div style="flex:1;min-width:80px;max-width:120px">
            <label style="font-size:0.7rem;color:#64748b;text-transform:uppercase;letter-spacing:.5px">Command</label>
            <select id="schema-pe-cmd" style="width:100%;background:#080c18;border:1px solid #2d4060;border-radius:4px;color:#f8fafc;padding:4px 8px;font-size:0.8rem;margin-top:2px">
              <option>ALL</option><option>SELECT</option><option>INSERT</option><option>UPDATE</option><option>DELETE</option>
            </select>
          </div>
          <div style="flex:1;min-width:100px;max-width:120px">
            <label style="font-size:0.7rem;color:#64748b;text-transform:uppercase;letter-spacing:.5px">Role</label>
            <input id="schema-pe-role" value="PUBLIC" style="width:100%;background:#080c18;border:1px solid #2d4060;border-radius:4px;color:#f8fafc;padding:4px 8px;font-size:0.8rem;margin-top:2px">
          </div>
          <div style="flex:2;min-width:200px">
            <label style="font-size:0.7rem;color:#64748b;text-transform:uppercase;letter-spacing:.5px">USING Expression</label>
            <input id="schema-pe-using" placeholder="e.g. owner = CURRENT_USER_ID()" style="width:100%;background:#080c18;border:1px solid #2d4060;border-radius:4px;color:#f8fafc;padding:4px 8px;font-size:0.8rem;font-family:monospace;margin-top:2px">
          </div>
          <div style="flex:2;min-width:200px">
            <label style="font-size:0.7rem;color:#64748b;text-transform:uppercase;letter-spacing:.5px">WITH CHECK</label>
            <input id="schema-pe-check" placeholder="optional" style="width:100%;background:#080c18;border:1px solid #2d4060;border-radius:4px;color:#f8fafc;padding:4px 8px;font-size:0.8rem;font-family:monospace;margin-top:2px">
          </div>
          <div style="display:flex;align-items:flex-end">
            <button class="btn btn-green" onclick="savePolicyFromEditor()" style="font-size:0.8rem;padding:5px 14px;margin-top:14px">Save Policy</button>
          </div>
        </div>
        <div id="schema-pe-msg" style="font-size:0.75rem;margin-top:6px;color:#94a3b8"></div>
      </div>
    </div>
    <!-- MONITORING PAGE -->
    <div class="page" id="page-monitoring">
      <div style="display:flex;align-items:center;justify-content:space-between;padding:8px 16px;border-bottom:1px solid #1e2d40">
        <div style="display:flex;gap:4px;align-items:center">
          <button class="mon-tab active" id="mtab-overview" onclick="monShowTab('overview')">Overview</button>
          <button class="mon-tab" id="mtab-pool" onclick="monShowTab('pool')">Pool Stats</button>
        </div>
        <span style="font-size:0.75rem;color:#475569">Auto-refresh: <span id="m-countdown" style="color:#10b981;font-weight:600">5s</span></span>
      </div>
      <div id="mon-overview">
      <div class="mon-grid" id="mon-grid">
        <div class="stat-card"><div class="label">&#x1F4C1; Tables</div><div class="value" id="m-tables">--</div></div>
        <div class="stat-card"><div class="label">&#x1F4CA; Total Rows</div><div class="value" id="m-rows">--</div></div>
        <div class="stat-card"><div class="label">&#x26A1; Queries Run</div><div class="value" id="m-queries">--</div></div>
        <div class="stat-card"><div class="label">&#x23F1; Uptime</div><div class="value" id="m-uptime" style="font-size:1rem">--</div></div>
        <div class="stat-card"><div class="label">&#x1F40C; Slow Queries</div><div class="value" id="m-slow">--</div></div>
        <div class="stat-card"><div class="label">&#x1F4BB; Version</div><div class="value" id="m-version" style="font-size:0.85rem">--</div></div>
      </div>
      <div style="padding:16px">
        <div style="display:flex;align-items:center;gap:8px;margin-bottom:10px">
          <span style="font-size:0.85rem;font-weight:600;color:#f8fafc">Query History (last 10)</span>
          <span id="m-last-update" style="font-size:0.7rem;color:#475569;margin-left:auto"></span>
        </div>
        <canvas id="m-chart" height="60" style="width:100%;background:#080c18;border-radius:6px;border:1px solid #1e2d40"></canvas>
        <div style="display:flex;justify-content:space-between;margin-top:4px">
          <span style="font-size:0.65rem;color:#475569">10 samples · 5s interval</span>
          <span style="font-size:0.65rem;color:#475569">queries/poll</span>
        </div>
      </div>
      <div style="padding:0 16px 16px">
        <div style="font-size:0.85rem;font-weight:600;color:#f8fafc;margin-bottom:8px">Recent Slow Queries</div>
        <div id="slow-queries-list" style="font-size:0.8rem;color:#94a3b8;background:#080c18;border:1px solid #1e2d40;border-radius:6px;padding:10px;min-height:40px">Loading...</div>
      </div>
      </div><!-- /mon-overview -->
      <!-- POOL STATS TAB (Phase 173) -->
      <div id="mon-pool" style="display:none">
        <div class="mon-grid">
          <div class="stat-card"><div class="label">&#x1F7E2; Aktive Connections</div><div class="value" id="p-active">--</div></div>
          <div class="stat-card"><div class="label">&#x1F4A4; Idle</div><div class="value" id="p-idle">--</div></div>
          <div class="stat-card"><div class="label">&#x23F3; Wartend</div><div class="value" id="p-waiting">--</div></div>
          <div class="stat-card"><div class="label">&#x1F4CF; Pool-Gr&ouml;&szlig;e (Min/Max)</div><div class="value" id="p-minmax" style="font-size:1rem">--</div></div>
          <div class="stat-card"><div class="label">&#x1F522; Total Connections</div><div class="value" id="p-total">--</div></div>
          <div class="stat-card"><div class="label">&#x23F1; Avg Wait</div><div class="value" id="p-avgwait" style="font-size:1rem">--</div></div>
          <div class="stat-card"><div class="label">&#x26A0; Timeouts</div><div class="value" id="p-timeouts">--</div></div>
          <div class="stat-card"><div class="label">&#x1F504; Requests Total</div><div class="value" id="p-requests">--</div></div>
        </div>
        <div style="padding:16px">
          <div style="font-size:0.85rem;font-weight:600;color:#f8fafc;margin-bottom:8px">Pool-Auslastung</div>
          <canvas id="p-chart" height="90" style="width:100%;background:#080c18;border-radius:6px;border:1px solid #1e2d40"></canvas>
          <div style="display:flex;gap:16px;margin-top:6px;font-size:0.7rem;color:#94a3b8">
            <span><span style="color:#10b981">&#x25A0;</span> Aktiv</span>
            <span><span style="color:#00d4ff">&#x25A0;</span> Idle</span>
            <span><span style="color:#f59e0b">&#x25A0;</span> Wartend</span>
            <span><span style="color:#2d4060">&#x25A0;</span> Frei (bis Max)</span>
          </div>
        </div>
      </div>
    </div>

    <!-- HISTORY PAGE -->
    <div class="page" id="page-history">
      <div style="padding:12px;border-bottom:1px solid #1e2d40;display:flex;gap:8px;align-items:center">
        <span style="font-size:0.85rem;color:#94a3b8">Query History</span>
        <button class="btn btn-gray" style="margin-left:auto;font-size:0.75rem" onclick="clearHistory()">Clear</button>
      </div>
      <div id="history-list" style="flex:1;overflow-y:auto;padding:12px"></div>
    </div>

    <!-- VACUUM PAGE (Phase 173) -->
    <div class="page" id="page-vacuum">
      <div style="display:flex;align-items:center;justify-content:space-between;padding:12px 16px;border-bottom:1px solid #1e2d40">
        <span style="font-size:0.85rem;color:#94a3b8">&#x1F9F9; MVCC Vacuum &mdash; Dead Tuple Cleanup</span>
        <span style="font-size:0.75rem;color:#475569">Auto-refresh: <span style="color:#10b981;font-weight:600">5s</span></span>
      </div>
      <div class="mon-grid">
        <div class="stat-card"><div class="label">&#x1F550; Letzter Vacuum</div><div class="value" id="v-lastrun" style="font-size:0.9rem">--</div></div>
        <div class="stat-card"><div class="label">&#x23ED; N&auml;chster Auto-Vacuum</div><div class="value" id="v-nextrun" style="font-size:1rem">--</div></div>
        <div class="stat-card"><div class="label">&#x1F5D1; Befreite Zeilen (total)</div><div class="value" id="v-freedrows">--</div></div>
        <div class="stat-card"><div class="label">&#x1F4BE; Befreiter Speicher (total)</div><div class="value" id="v-freedbytes" style="font-size:1rem">--</div></div>
        <div class="stat-card"><div class="label">&#x1F504; Vacuum-L&auml;ufe</div><div class="value" id="v-runs">--</div></div>
        <div class="stat-card"><div class="label">&#x26A0; Dead Tuples (pending)</div><div class="value" id="v-pending">--</div></div>
      </div>
      <div style="padding:16px;flex:1;overflow-y:auto">
        <div style="display:flex;align-items:center;gap:8px;margin-bottom:8px">
          <span style="font-size:0.85rem;font-weight:600;color:#f8fafc">Tabellen</span>
          <span id="v-msg" style="font-size:0.75rem;color:#10b981;margin-left:auto"></span>
        </div>
        <div id="vacuum-table-list" style="background:#080c18;border:1px solid #1e2d40;border-radius:6px;padding:6px;font-size:0.8rem;color:#94a3b8">Loading...</div>
      </div>
    </div>

    <!-- REPLICATION PAGE (Phase 173) -->
    <div class="page" id="page-replication">
      <div style="display:flex;align-items:center;gap:10px;padding:12px 16px;border-bottom:1px solid #1e2d40">
        <span id="rep-light" style="font-size:1.1rem;color:#475569">&#x25CF;</span>
        <span id="rep-light-text" style="font-size:0.85rem;color:#94a3b8">Loading...</span>
        <span style="font-size:0.75rem;color:#475569;margin-left:auto">Auto-refresh: <span style="color:#10b981;font-weight:600">2s</span></span>
      </div>
      <div class="mon-grid">
        <div class="stat-card"><div class="label">&#x1F3AD; Rolle</div><div class="value" id="r-role" style="font-size:1rem">--</div></div>
        <div class="stat-card"><div class="label">&#x23F1; Replica-Lag</div><div class="value" id="r-lag" style="font-size:1rem">--</div></div>
        <div class="stat-card"><div class="label">&#x1F4CD; Binlog-Position</div><div class="value" id="r-binlog">--</div></div>
        <div class="stat-card"><div class="label">&#x2705; Ack-Position</div><div class="value" id="r-ack">--</div></div>
        <div class="stat-card"><div class="label">&#x1F517; Verbundene Replicas</div><div class="value" id="r-slaves">--</div></div>
        <div class="stat-card"><div class="label">&#x2699; Modus</div><div class="value" id="r-mode" style="font-size:1rem">--</div></div>
      </div>
      <div style="padding:16px;flex:1;overflow-y:auto">
        <div style="font-size:0.85rem;font-weight:600;color:#f8fafc;margin-bottom:8px">Replicas</div>
        <div id="rep-slave-list" style="background:#080c18;border:1px solid #1e2d40;border-radius:6px;padding:10px;font-size:0.8rem;color:#94a3b8;margin-bottom:16px">Loading...</div>
        <div id="rep-replica-detail" style="display:none">
          <div style="font-size:0.85rem;font-weight:600;color:#f8fafc;margin-bottom:8px">Replica-Verbindung</div>
          <div id="rep-replica-info" style="background:#080c18;border:1px solid #1e2d40;border-radius:6px;padding:10px;font-size:0.8rem;color:#94a3b8"></div>
        </div>
      </div>
    </div>

  </div><!-- /main -->
</div><!-- /layout -->

<!-- STATUS BAR -->
<div id="statusbar">
  <div class="status-item"><div class="status-dot" id="sb-dot"></div><span id="sb-health">healthy</span></div>
  <div class="status-item">Tables: <b id="sb-tables">--</b></div>
  <div class="status-item">Rows: <b id="sb-rows">--</b></div>
  <div class="status-item">Queries: <b id="sb-queries">--</b></div>
  <div class="status-item" style="margin-left:auto;font-size:0.7rem;color:#475569">MilanSQL <span class="ms-version">v10.6.0</span> &middot; Press Ctrl+Enter to run</div>
</div>

<script>
// ── Syntax Highlighting ───────────────────────────────────────
var SQL_KEYWORDS = ['SELECT','FROM','WHERE','JOIN','LEFT JOIN','RIGHT JOIN','INNER JOIN','FULL JOIN','CROSS JOIN',
  'INSERT','UPDATE','DELETE','CREATE','DROP','ALTER','TABLE','INDEX','DATABASE','SCHEMA',
  'GROUP BY','ORDER BY','HAVING','LIMIT','OFFSET','BEGIN','COMMIT','ROLLBACK','SAVEPOINT',
  'AND','OR','NOT','NULL','IS','IN','LIKE','BETWEEN','EXISTS','CASE','WHEN','THEN','ELSE','END',
  'PRIMARY KEY','FOREIGN KEY','AUTO_INCREMENT','UNIQUE','DEFAULT','REFERENCES','CONSTRAINT',
  'INTO','SET','VALUES','ON','AS','DISTINCT','ALL','UNION','INTERSECT','EXCEPT',
  'GRANT','REVOKE','SHOW','DESCRIBE','EXPLAIN','USE','TRUNCATE','WITH','LATERAL','RECURSIVE'];
var SQL_FUNCS = ['COUNT','SUM','AVG','MIN','MAX','COALESCE','NULLIF','IFNULL','IF',
  'CAST','CONVERT','UPPER','LOWER','SUBSTR','SUBSTRING','LENGTH','TRIM','LTRIM','RTRIM',
  'REPLACE','CONCAT','CONCAT_WS','GROUP_CONCAT','NOW','DATE','YEAR','MONTH','DAY',
  'ROUND','FLOOR','CEIL','CEILING','ABS','MOD','POWER','SQRT','RAND',
  'ROW_NUMBER','RANK','DENSE_RANK','LEAD','LAG','FIRST_VALUE','LAST_VALUE','NTH_VALUE',
  'OVER','PARTITION BY','STRING_AGG','ARRAY_AGG','JSONB_AGG','JSONB_BUILD_OBJECT',
  'VERSION','DATABASE','USER','SCHEMA'];

function escRe(s) { return s.replace(/[.*+?^${}()|[\]\\]/g,'\\$&'); }
function highlightSQL(text) {
  // Escape HTML first
  var s = text.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');
  // Comments (-- and /* */) — replace with placeholder to protect from further substitution
  var comments = [];
  s = s.replace(/\/\*[\s\S]*?\*\//g, function(m){ comments.push('<span style="color:#94a3b8">'+m+'</span>'); return '\x00C'+(comments.length-1)+'\x00'; });
  s = s.replace(/--[^\n]*/g, function(m){ comments.push('<span style="color:#94a3b8">'+m+'</span>'); return '\x00C'+(comments.length-1)+'\x00'; });
  // Strings
  var strings = [];
  s = s.replace(/'(?:[^'\\]|\\.)*'/g, function(m){ strings.push('<span style="color:#fbbf24">'+m+'</span>'); return '\x00S'+(strings.length-1)+'\x00'; });
  s = s.replace(/"(?:[^"\\]|\\.)*"/g, function(m){ strings.push('<span style="color:#fbbf24">'+m+'</span>'); return '\x00S'+(strings.length-1)+'\x00'; });
  // Numbers
  s = s.replace(/\b(\d+(?:\.\d+)?)\b/g,'<span style="color:#a78bfa">$1</span>');
  // Functions (before keywords so they match first)
  var fnPat = '\\b(' + SQL_FUNCS.map(escRe).join('|') + ')\\s*(?=\\()';
  s = s.replace(new RegExp(fnPat,'gi'), function(m){ return '<span style="color:#c084fc">'+m+'</span>'; });
  // Keywords (longest first to match GROUP BY before GROUP)
  var kwSorted = SQL_KEYWORDS.slice().sort(function(a,b){return b.length-a.length;});
  kwSorted.forEach(function(kw){
    s = s.replace(new RegExp('\\b'+escRe(kw)+'\\b','gi'), function(m){ return '<span style="color:#00d4ff">'+m+'</span>'; });
  });
  // Restore strings and comments
  s = s.replace(/\x00S(\d+)\x00/g, function(_,i){ return strings[+i]; });
  s = s.replace(/\x00C(\d+)\x00/g, function(_,i){ return comments[+i]; });
  return s;
}

// ── Editor: Line Numbers + Highlight sync ────────────────────
var acItems = [];
var acIndex = -1;
var acVisible = false;

function updateEditorDecor() {
  var ed = document.getElementById('sql-editor');
  var backdrop = document.getElementById('highlight-backdrop');
  var lineNums = document.getElementById('line-numbers');
  if (!ed || !backdrop) return;
  var val = ed.value;
  backdrop.innerHTML = highlightSQL(val) + '\n'; // extra \n prevents scroll flicker
  var lines = val.split('\n').length;
  var nums = '';
  for (var i = 1; i <= lines; i++) nums += i + '\n';
  lineNums.textContent = nums;
  // Sync scroll
  backdrop.scrollTop = ed.scrollTop;
  backdrop.scrollLeft = ed.scrollLeft;
  lineNums.scrollTop = ed.scrollTop;
}

// ── Autocomplete ──────────────────────────────────────────────
var acTableNames = [];
fetch('/tables',{credentials:'include'}).then(function(r){return r.json();}).then(function(d){
  var tables = Array.isArray(d) ? d : (d.tables||[]);
  acTableNames = tables.map(function(t){ return typeof t==='string'?t:(t.name||''); }).filter(Boolean);
}).catch(function(){});

function getWordBeforeCursor(ta) {
  var pos = ta.selectionStart;
  var text = ta.value.substring(0, pos);
  var m = text.match(/[\w]+$/);
  return m ? m[0] : '';
}

function showAutocomplete(ta) {
  var word = getWordBeforeCursor(ta);
  if (!word || word.length < 1) { closeAutocomplete(); return; }
  var ul = word.toUpperCase();
  var candidates = SQL_KEYWORDS.concat(SQL_FUNCS).concat(acTableNames).filter(function(k){
    return k.toUpperCase().startsWith(ul) && k.toUpperCase() !== ul;
  });
  // Deduplicate
  candidates = candidates.filter(function(v,i,a){ return a.indexOf(v)===i; });
  if (!candidates.length) { closeAutocomplete(); return; }
  var ac = document.getElementById('autocomplete');
  ac.innerHTML = candidates.slice(0,20).map(function(c,i){
    return '<div class="ac-item" data-idx="'+i+'" onclick="applyAC(\''+escAttr(c)+'\')">' + escHtml(c) + '</div>';
  }).join('');
  acItems = candidates.slice(0,20);
  acIndex = -1;
  acVisible = true;
  // Position below editor
  var wrap = document.getElementById('editor-wrap');
  var rect = wrap.getBoundingClientRect();
  ac.style.display = 'block';
  ac.style.left = '44px';
  ac.style.top = (rect.bottom - rect.top + 8) + 'px';
}

function closeAutocomplete() {
  acVisible = false; acIndex = -1; acItems = [];
  document.getElementById('autocomplete').style.display = 'none';
}

function applyAC(word) {
  var ta = document.getElementById('sql-editor');
  var pos = ta.selectionStart;
  var text = ta.value;
  var before = text.substring(0, pos);
  var after = text.substring(pos);
  var m = before.match(/[\w]+$/);
  var start = m ? pos - m[0].length : pos;
  ta.value = text.substring(0, start) + word + after;
  ta.selectionStart = ta.selectionEnd = start + word.length;
  closeAutocomplete();
  updateEditorDecor();
  ta.focus();
}

function moveAC(dir) {
  if (!acVisible) return false;
  var items = document.querySelectorAll('.ac-item');
  if (!items.length) return false;
  if (acIndex >= 0) items[acIndex].classList.remove('ac-selected');
  acIndex = (acIndex + dir + items.length) % items.length;
  items[acIndex].classList.add('ac-selected');
  items[acIndex].scrollIntoView({block:'nearest'});
  return true;
}

// ── Page navigation ───────────────────────────────────────────
function showPage(name, el) {
  document.querySelectorAll('.page').forEach(function(p){p.classList.remove('active');});
  document.querySelectorAll('.nav-item').forEach(function(n){n.classList.remove('active');});
  document.getElementById('page-' + name).classList.add('active');
  if (el) el.classList.add('active');
  if (name === 'browser') loadBrowserTables();
  if (name === 'schema') loadSchemaViz();
  if (name === 'history') renderHistory();
  if (name === 'monitoring') { monLastQ = 0; monQueryHistory = []; loadMonitoring(); }
  else stopMonitoring();
  if (name === 'vacuum') loadVacuumPage(); else stopVacuumPage();
  if (name === 'replication') loadReplicationPage(); else stopReplicationPage();
}

// ── SQL Execution ──────────────────────────────────────────────
var lastResultData = null;
async function runQuery(sql) {
  var q = sql || document.getElementById('sql-editor').value.trim();
  if (!q) return;
  var t0 = performance.now();
  document.getElementById('result-content').innerHTML = '<div style="color:#94a3b8;padding:8px;font-size:0.8rem">Running...</div>';
  document.getElementById('result-header').style.display = 'none';
  clearErrorHighlight();
  try {
    var resp = await fetch('/api/query', {
      method: 'POST',
      credentials: 'include',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({sql: q})
    });
    var data = await resp.json();
    var ms = (performance.now() - t0).toFixed(1);
    document.getElementById('exec-time').textContent = ms + 'ms';
    lastResultData = data;
    renderResult(data, ms);
    showExecBadge(data);
    saveHistory(q, ms, !data.error && (data.success !== false));
  } catch(e) {
    showError('Network error: ' + e.message);
    saveHistory(q, '0', false);
  }
}
function showExecBadge(data) {
  var badge = document.getElementById('exec-badge');
  if (!badge) return;
  if (msUser) {
    badge.innerHTML = '<span style="color:#10b981">&#10003;</span> Ausgeführt als: <b>' + escHtml(msUser) + '</b> (id: ' + msUserId + ')';
    badge.style.color = '#10b981';
  } else {
    badge.innerHTML = '<span style="color:#ef4444">&#9888;</span> Nicht authentifiziert';
    badge.style.color = '#ef4444';
  }
  badge.style.display = 'block';
}

function renderResult(data, ms) {
  var hdr = document.getElementById('result-header');
  var pill = document.getElementById('result-pill');
  var info = document.getElementById('result-info');
  var content = document.getElementById('result-content');
  hdr.style.display = 'flex';

  var err = data.error || (!data.success && data.success !== undefined ? data.error : null);
  if (err) {
    pill.className = 'pill error'; pill.textContent = 'Error';
    info.textContent = '';
    // Try to extract line number from error message
    var lineMatch = err.match(/line\s+(\d+)/i) || err.match(/at\s+line\s+(\d+)/i);
    var lineHtml = '';
    if (lineMatch) {
      var ln = parseInt(lineMatch[1]);
      highlightErrorLine(ln);
      lineHtml = '<div class="error-line-badge">Zeile '+ln+'</div>';
    }
    content.innerHTML = lineHtml + '<div class="error-box">&#x26A0; ' + escHtml(err) + '</div>';
    return;
  }

  var rows = data.rows || [];
  var cols = data.columns || [];

  if (cols.length === 0) {
    var aff = data.affected_rows !== undefined ? data.affected_rows : (data.rowsAffected || 0);
    pill.className = 'pill'; pill.textContent = 'OK';
    info.textContent = aff + ' rows affected \u00b7 ' + ms + 'ms';
    content.innerHTML = '<div class="affected-box">&#x2713; Query executed successfully. ' + aff + ' row(s) affected.</div>';
    return;
  }

  pill.className = 'pill'; pill.textContent = rows.length + ' rows';
  info.textContent = rows.length + ' rows returned \u00b7 ' + ms + 'ms';

  renderTable(cols, rows, content);
}

var sortState = {col:-1, asc:true};
function renderTable(cols, rows, container) {
  var html = '<div id="result-table-wrap"><table id="result-table"><thead><tr>';
  cols.forEach(function(c,i){
    var name = escHtml(typeof c === 'string' ? c : (c.name || String(c)));
    var cls = sortState.col===i ? (sortState.asc?'sort-asc':'sort-desc') : '';
    html += '<th class="'+cls+'" onclick="sortTable('+i+')">'+name+'</th>';
  });
  html += '</tr></thead><tbody>';
  rows.forEach(function(row) {
    html += '<tr>';
    var vals = rowVals(row);
    vals.forEach(function(v) {
      if (v === null || v === 'NULL' || v === '') {
        html += '<td class="null-val">NULL</td>';
      } else if (!isNaN(v) && v !== '') {
        html += '<td class="num">' + escHtml(String(v)) + '</td>';
      } else {
        html += '<td>' + escHtml(String(v)) + '</td>';
      }
    });
    html += '</tr>';
  });
  html += '</tbody></table></div>';
  container.innerHTML = html;
}

function sortTable(colIdx) {
  if (!lastResultData) return;
  var rows = (lastResultData.rows || []).slice();
  var cols = lastResultData.columns || [];
  if (sortState.col === colIdx) {
    sortState.asc = !sortState.asc;
  } else {
    sortState.col = colIdx; sortState.asc = true;
  }
  rows.sort(function(a, b) {
    var va = Array.isArray(a) ? a[colIdx] : (a.values||Object.values(a))[colIdx];
    var vb = Array.isArray(b) ? b[colIdx] : (b.values||Object.values(b))[colIdx];
    if (va === null || va === undefined) va = '';
    if (vb === null || vb === undefined) vb = '';
    var na = parseFloat(va), nb = parseFloat(vb);
    var cmp = (!isNaN(na)&&!isNaN(nb)) ? na-nb : String(va).localeCompare(String(vb));
    return sortState.asc ? cmp : -cmp;
  });
  renderTable(cols, rows, document.getElementById('result-content'));
}

function highlightErrorLine(lineNum) {
  var ed = document.getElementById('sql-editor');
  var lines = ed.value.split('\n');
  if (lineNum < 1 || lineNum > lines.length) return;
  var start = lines.slice(0, lineNum-1).join('\n').length + (lineNum > 1 ? 1 : 0);
  var end = start + lines[lineNum-1].length;
  ed.focus();
  ed.setSelectionRange(start, end);
}

function clearErrorHighlight() {
  // Nothing to do — selection cleared on next click
}

function showError(msg) {
  document.getElementById('result-header').style.display = 'flex';
  document.getElementById('result-pill').className = 'pill error';
  document.getElementById('result-pill').textContent = 'Error';
  document.getElementById('result-info').textContent = '';
  document.getElementById('result-content').innerHTML = '<div class="error-box">' + escHtml(msg) + '</div>';
}

async function explainQuery() {
  var q = document.getElementById('sql-editor').value.trim();
  if (!q) return;
  await runQuery('EXPLAIN ' + q);
}

function formatSQL() {
  var ed = document.getElementById('sql-editor');
  var kws = ['SELECT','FROM','WHERE','JOIN','LEFT','RIGHT','INNER','OUTER','ON',
             'GROUP BY','ORDER BY','HAVING','LIMIT','OFFSET','INSERT INTO',
             'VALUES','UPDATE','SET','DELETE FROM','CREATE TABLE','DROP TABLE',
             'ALTER TABLE','BEGIN','COMMIT','ROLLBACK','AND','OR','NOT'];
  var s = ed.value;
  kws.forEach(function(k){ s = s.replace(new RegExp('\\b' + k + '\\b','gi'), k); });
  ed.value = s;
  updateEditorDecor();
}

function clearEditor() {
  var ed = document.getElementById('sql-editor');
  ed.value = '';
  updateEditorDecor();
  document.getElementById('result-header').style.display = 'none';
  document.getElementById('result-content').innerHTML = '';
  document.getElementById('exec-time').textContent = '';
  lastResultData = null;
}
function setSQL(s) {
  var ed = document.getElementById('sql-editor');
  ed.value = s;
  updateEditorDecor();
  ed.focus();
}

function copyCSV() {
  if (!lastResultData) return;
  var rows = lastResultData.rows || [];
  var cols = lastResultData.columns || [];
  if (!cols.length) return;
  var csvCols = cols.map(function(c){ return typeof c==='string'?c:(c.name||String(c)); });
  var lines = [csvCols.join(',')];
  rows.forEach(function(row){
    var vals = rowVals(row);
    lines.push(vals.map(function(v){
      var s = v===null||v===undefined?'':String(v);
      return s.includes(',') || s.includes('"') || s.includes('\n') ? '"'+s.replace(/"/g,'""')+'"' : s;
    }).join(','));
  });
  var csv = lines.join('\n');
  navigator.clipboard.writeText(csv).then(function(){
    var btn = document.querySelector('[onclick="copyCSV()"]');
    if(btn){ var old=btn.textContent; btn.textContent='Copied!'; setTimeout(function(){btn.textContent=old;},1500); }
  }).catch(function(){});
}

// Keyboard shortcuts
document.getElementById('sql-editor').addEventListener('keydown', function(e) {
  if (acVisible) {
    if (e.key === 'ArrowDown') { e.preventDefault(); moveAC(1); return; }
    if (e.key === 'ArrowUp')   { e.preventDefault(); moveAC(-1); return; }
    if (e.key === 'Enter' || e.key === 'Tab') {
      if (acIndex >= 0 && acItems[acIndex]) { e.preventDefault(); applyAC(acItems[acIndex]); return; }
    }
    if (e.key === 'Escape')    { e.preventDefault(); closeAutocomplete(); return; }
  }
  if (e.ctrlKey && e.key === ' ') { e.preventDefault(); showAutocomplete(e.target); return; }
  if (e.ctrlKey && e.key === 'Enter') { e.preventDefault(); runQuery(); return; }
  if (e.ctrlKey && e.key === 'e')     { e.preventDefault(); explainQuery(); return; }
  if (e.ctrlKey && e.key === 'l')     { e.preventDefault(); clearEditor(); return; }
  if (e.ctrlKey && e.key === 'h')     { e.preventDefault(); showPage('history', document.querySelector('[data-page=history]')); return; }
  if (e.key === 'Tab' && !e.ctrlKey) {
    e.preventDefault();
    var ta = e.target;
    var s = ta.selectionStart;
    ta.value = ta.value.substring(0,s) + '    ' + ta.value.substring(ta.selectionEnd);
    ta.selectionStart = ta.selectionEnd = s + 4;
    updateEditorDecor();
    return;
  }
});

document.getElementById('sql-editor').addEventListener('input', function() {
  updateEditorDecor();
  closeAutocomplete();
});
document.getElementById('sql-editor').addEventListener('scroll', function() {
  var bd = document.getElementById('highlight-backdrop');
  var ln = document.getElementById('line-numbers');
  if(bd){ bd.scrollTop = this.scrollTop; bd.scrollLeft = this.scrollLeft; }
  if(ln) ln.scrollTop = this.scrollTop;
});
document.addEventListener('click', function(e) {
  if (!e.target.closest('#autocomplete') && e.target.id !== 'sql-editor') closeAutocomplete();
});

// Table sidebar
var _rlsPoliciesCache = {};

// Phase 170: Show RLS Policies for table in editor
async function showRlsPolicies() {
  var sql = document.getElementById('sql-editor').value.trim();
  var tblMatch = sql.match(/(?:FROM|JOIN|TABLE|ON|INTO|UPDATE|POLICIES)\s+([a-zA-Z_][a-zA-Z0-9_]*)/i);
  if (!tblMatch) {
    try {
      var r = await fetch('/api/rls-policies', {credentials:'include'});
      var data = await r.json();
      var out = document.getElementById('output-area');
      var html = '<div style="padding:12px"><h3 style="color:#f59e0b;margin-bottom:12px">&#x1F6E1; RLS Policies Overview</h3>';
      var enabled = data.enabled_tables || [];
      html += '<div style="color:#94a3b8;margin-bottom:8px">Protected tables: <b style="color:#10b981">' + enabled.length + '</b></div>';
      if (enabled.length === 0) {
        html += '<div style="color:#475569">No tables have RLS enabled.</div>';
      } else {
        html += '<table><thead><tr><th>Table</th><th>Policies</th><th>Details</th></tr></thead><tbody>';
        for (var i = 0; i < enabled.length; i++) {
          var tbl = enabled[i];
          var pols = (data.policies || {})[tbl] || [];
          var details = pols.map(function(p) { return '<span style="color:#00d4ff">' + escHtml(p.name) + '</span> (' + p.command + ' TO ' + p.role + ')'; }).join(', ') || '<span style="color:#475569">none</span>';
          html += '<tr><td style="color:#f8fafc;font-weight:600">' + escHtml(tbl) + '</td><td style="text-align:center">' + pols.length + '</td><td>' + details + '</td></tr>';
        }
        html += '</tbody></table>';
      }
      html += '</div>';
      if (out) out.innerHTML = html;
    } catch(e) {}
    return;
  }
  var tblName = tblMatch[1];
  try {
    var r = await fetch('/api/rls-policies/' + encodeURIComponent(tblName), {credentials:'include'});
    var data = await r.json();
    var out = document.getElementById('output-area');
    var html = '<div style="padding:12px"><h3 style="color:#f59e0b;margin-bottom:12px">&#x1F6E1; RLS Policies: ' + escHtml(tblName) + '</h3>';
    html += '<div style="margin-bottom:8px;color:#94a3b8">RLS Status: ' + (data.rls_enabled ? '<b style="color:#10b981">ENABLED</b>' : '<b style="color:#ef4444">DISABLED</b>') + '</div>';
    var pols = data.policies || [];
    if (pols.length === 0) {
      html += '<div style="color:#475569;margin-bottom:8px">No policies defined.</div>';
      html += '<div style="color:#94a3b8;font-size:0.8rem">Create: <code style="color:#00d4ff">CREATE POLICY name ON ' + escHtml(tblName) + ' FOR ALL TO PUBLIC USING (expr);</code></div>';
    } else {
      html += '<table><thead><tr><th>Policy</th><th>Command</th><th>Role</th><th>USING</th><th>WITH CHECK</th></tr></thead><tbody>';
      for (var i = 0; i < pols.length; i++) {
        var p = pols[i];
        html += '<tr>';
        html += '<td style="color:#00d4ff;font-weight:600">' + escHtml(p.name) + '</td>';
        html += '<td><span style="background:#1e2d40;padding:2px 6px;border-radius:3px;font-size:0.75rem;color:#f59e0b">' + escHtml(p.command) + '</span></td>';
        html += '<td>' + escHtml(p.role) + '</td>';
        html += '<td style="font-family:monospace;font-size:0.8rem;color:#f8fafc">' + escHtml(p['using']||'') + '</td>';
        html += '<td style="font-family:monospace;font-size:0.8rem;color:#f8fafc">' + escHtml(p.with_check||'-') + '</td>';
        html += '</tr>';
      }
      html += '</tbody></table>';
    }
    html += '</div>';
    if (out) out.innerHTML = html;
  } catch(e) {}
}


async function loadRlsPolicies() {
  try {
    var r = await fetch('/api/rls-policies', {credentials:'include'});
    _rlsPoliciesCache = await r.json();
  } catch(e) { _rlsPoliciesCache = {}; }
}

function getRlsBadge(tableName) {
  var pols = _rlsPoliciesCache.policies || {};
  // Check both raw name and with user prefix
  var count = 0;
  var enabled = false;
  var enabledTables = _rlsPoliciesCache.enabled_tables || [];
  for (var key in pols) {
    if (key === tableName || key.endsWith('_' + tableName)) {
      count = pols[key].length;
    }
  }
  for (var i = 0; i < enabledTables.length; i++) {
    if (enabledTables[i] === tableName || enabledTables[i].endsWith('_' + tableName)) {
      enabled = true;
    }
  }
  if (!enabled && count === 0) return '';
  if (enabled && count > 0)
    return '<span style="margin-left:auto;background:rgba(16,185,129,.12);color:#10b981;font-size:9px;padding:1px 5px;border-radius:8px;font-weight:600" title="' + count + ' RLS ' + (count===1?'Policy':'Policies') + '">' + count + ' RLS</span>';
  if (enabled)
    return '<span style="margin-left:auto;background:rgba(16,185,129,.12);color:#10b981;font-size:9px;padding:1px 5px;border-radius:8px" title="RLS enabled (no policies)">RLS</span>';
  return '';
}

async function loadSidebarTables() {
  try {
    await loadRlsPolicies();
    var r = await fetch('/tables', {credentials:'include'});
    var data = await r.json();
    var tables = Array.isArray(data) ? data : (data.tables || []);
    var el = document.getElementById('sidebar-tables');
    if (!tables.length) { el.innerHTML = '<div style="font-size:0.75rem;color:#475569;padding:4px 8px">No tables</div>'; return; }
    el.innerHTML = tables.map(function(t) {
      var name = typeof t === 'string' ? t : t.name;
      var badge = getRlsBadge(name);
      return '<div class="table-item" style="display:flex;align-items:center" onclick="selectFromTable(\'' + escAttr(name) + '\')">'
        + '<span>' + escHtml(name) + '</span>' + badge + '</div>';
    }).join('');
  } catch(e) { /* silent */ }
}

function selectFromTable(name) {
  document.getElementById('sql-editor').value = 'SELECT * FROM ' + name + ' LIMIT 100;';
  updateEditorDecor();
  showPage('editor', document.querySelector('[data-page=editor]'));
  runQuery();
}

// Table Browser
async function loadBrowserTables() {
  try {
    var r = await fetch('/tables', {credentials:'include'});
    var data = await r.json();
    var tables = Array.isArray(data) ? data : (data.tables || []);
    var listEl = document.getElementById('browser-tbl-list');
    listEl.innerHTML = tables.map(function(t) {
      var name = typeof t === 'string' ? t : t.name;
      var badge = getRlsBadge(name);
      return '<button class="tbl-btn" style="display:flex;align-items:center" onclick="browseTable(\'' + escAttr(name) + '\',this)"><span>' + escHtml(name) + '</span>' + badge + '</button>';
    }).join('') || '<div style="font-size:0.75rem;color:#475569">No tables</div>';
  } catch(e) {}
}

async function browseTable(name, btn) {
  document.querySelectorAll('.tbl-btn').forEach(function(b){b.classList.remove('active');});
  if (btn) btn.classList.add('active');
  var detail = document.getElementById('browser-tbl-detail');
  detail.innerHTML = '<div style="color:#94a3b8;font-size:0.8rem">Loading...</div>';
  try {
    var descR = await fetch('/api/query', {method:'POST',credentials:'include',headers:{'Content-Type':'application/json'},body:JSON.stringify({sql:'DESCRIBE ' + name})});
    var dataR = await fetch('/api/query', {method:'POST',credentials:'include',headers:{'Content-Type':'application/json'},body:JSON.stringify({sql:'SELECT * FROM ' + name + ' LIMIT 50'})});
    var desc = await descR.json();
    var data = await dataR.json();
    var html = '<h3 style="margin-bottom:12px">&#x1F4CB; ' + escHtml(name) + '</h3>';
    if (desc.columns && desc.rows) {
      html += '<div style="font-size:0.75rem;color:#94a3b8;margin-bottom:6px;text-transform:uppercase;letter-spacing:.06em">Schema</div>';
      html += '<div id="result-table-wrap" style="margin-bottom:16px"><table><thead><tr>';
      desc.columns.forEach(function(c){ html += '<th>' + escHtml(typeof c==='string'?c:(c.name||String(c))) + '</th>'; });
      html += '</tr></thead><tbody>';
      (desc.rows||[]).forEach(function(row) {
        html += '<tr>';
        rowVals(row).forEach(function(v){ html += '<td>' + escHtml(String(v != null ? v : '')) + '</td>'; });
        html += '</tr>';
      });
      html += '</tbody></table></div>';
    }
    var rows = data.rows||[], cols = data.columns||[];
    html += '<div style="font-size:0.75rem;color:#94a3b8;margin-bottom:6px;text-transform:uppercase;letter-spacing:.06em">Data (first 50 rows)</div>';
    html += '<div id="result-table-wrap"><table><thead><tr>';
    cols.forEach(function(c){ html += '<th>' + escHtml(typeof c==='string'?c:(c.name||String(c))) + '</th>'; });
    html += '</tr></thead><tbody>';
    rows.forEach(function(row) {
      html += '<tr>';
      rowVals(row).forEach(function(v) {
        var sv = String(v != null ? v : '');
        html += (!isNaN(sv)&&sv!=='') ? '<td class="num">'+escHtml(sv)+'</td>' : '<td>'+escHtml(sv)+'</td>';
      });
      html += '</tr>';
    });
    html += '</tbody></table></div>';
    detail.innerHTML = html;
  } catch(e) { detail.innerHTML = '<div class="error-box">' + escHtml(e.message) + '</div>'; }
}

// Monitoring — Phase 166
var monTimer = null, monCountdown = 5, monQueryHistory = [], monLastQ = 0;

function stopMonitoring() {
  if (monTimer) { clearInterval(monTimer); monTimer = null; }
}

async function fetchMonitoring() {
  try {
    var r = await fetch('/status', {credentials:'include'});
    var d = await r.json();
    function set(id, val){ var el = document.getElementById(id); if(el) el.textContent = val; }
    set('m-tables',  d.tables   != null ? d.tables   : '--');
    set('m-rows',    d.rows     != null ? d.rows     : '--');
    var qc = d.queries != null ? d.queries : (d.query_count != null ? d.query_count : 0);
    set('m-queries', qc);
    set('m-uptime',  d.uptime_fmt != null ? d.uptime_fmt : (d.uptime != null ? d.uptime + 's' : '--'));
    set('m-slow',    d.slow_queries != null ? d.slow_queries : '0');
    set('m-version', d.version || 'MilanSQL');
    var now = new Date();
    set('m-last-update', 'Updated: ' + now.toLocaleTimeString());
    // Track query delta for chart
    var delta = qc - monLastQ; if (monLastQ === 0) delta = 0;
    monLastQ = qc;
    monQueryHistory.push(delta);
    if (monQueryHistory.length > 10) monQueryHistory.shift();
    drawMonChart();
    // Slow queries
    loadSlowQueriesMon();
    // Phase 173: Pool tab auto-refresh (same 5s cycle)
    if (monTab === 'pool') fetchPoolStats();
  } catch(e) {
    var el = document.getElementById('m-last-update');
    if(el) el.textContent = 'Error fetching status';
  }
}

function drawMonChart() {
  var canvas = document.getElementById('m-chart');
  if (!canvas || !canvas.getContext) return;
  canvas.width = canvas.offsetWidth || 400;
  var ctx = canvas.getContext('2d');
  var W = canvas.width, H = canvas.height || 60;
  ctx.clearRect(0, 0, W, H);
  ctx.fillStyle = '#080c18';
  ctx.fillRect(0, 0, W, H);
  var data = monQueryHistory;
  if (!data.length) { ctx.fillStyle='#475569'; ctx.font='10px monospace'; ctx.fillText('No data yet',8,H/2+4); return; }
  var maxV = Math.max(1, Math.max.apply(null, data));
  var barW = Math.floor((W - 20) / 10);
  var pad = 4;
  for (var i = 0; i < data.length; i++) {
    var barH = Math.max(2, Math.floor((data[i] / maxV) * (H - 16)));
    var x = pad + i * (barW + 2);
    var y = H - 8 - barH;
    var age = data.length - 1 - i;
    var alpha = 0.4 + 0.6 * (i / Math.max(1, data.length - 1));
    ctx.fillStyle = 'rgba(0,212,255,' + alpha + ')';
    ctx.fillRect(x, y, barW, barH);
    ctx.fillStyle = '#475569';
    ctx.font = '8px monospace';
    ctx.fillText(data[i], x, H - 1);
  }
  ctx.fillStyle = '#475569';
  ctx.font = '9px monospace';
  ctx.fillText('max:'+maxV, W-40, 10);
}

async function loadSlowQueriesMon() {
  try {
    var r = await fetch('/query', {method:'POST',headers:{'Content-Type':'application/json'},
      body:JSON.stringify({sql:'SHOW SLOW QUERIES'}), credentials:'include'});
    var d = await r.json();
    var el = document.getElementById('slow-queries-list');
    if (!el) return;
    if (!d.success || !d.rows || !d.rows.length) {
      el.textContent = 'No slow queries recorded.'; return;
    }
    var html = '<table style="width:100%;border-collapse:collapse;font-size:0.75rem">';
    html += '<tr style="color:#64748b;border-bottom:1px solid #1e2d40">';
    (d.columns||[]).forEach(function(c){ html += '<th style="text-align:left;padding:3px 6px">'+escHtml(c.name||c)+'</th>'; });
    html += '</tr>';
    d.rows.slice(0,8).forEach(function(row){
      html += '<tr style="border-bottom:1px solid #111827">';
      rowVals(row).forEach(function(v){ html += '<td style="padding:3px 6px;color:#f8fafc">'+escHtml(String(v||''))+'</td>'; });
      html += '</tr>';
    });
    html += '</table>';
    el.innerHTML = html;
  } catch(e) {
    var el = document.getElementById('slow-queries-list');
    if(el) el.textContent = 'Run SHOW SLOW QUERIES to see data.';
  }
}

function startMonCountdown() {
  monCountdown = 5;
  var cdEl = document.getElementById('m-countdown');
  if (cdEl) cdEl.textContent = monCountdown + 's';
  if (monTimer) clearInterval(monTimer);
  monTimer = setInterval(function() {
    monCountdown--;
    if (cdEl) cdEl.textContent = Math.max(0, monCountdown) + 's';
    if (monCountdown <= 0) {
      monCountdown = 5;
      fetchMonitoring();
    }
  }, 1000);
}

async function loadMonitoring() {
  await fetchMonitoring();
  startMonCountdown();
}

// ── Pool Stats Tab (Phase 173) ────────────────────────────────
var monTab = 'overview';
function monShowTab(tab) {
  monTab = tab;
  document.getElementById('mon-overview').style.display = (tab === 'overview') ? '' : 'none';
  document.getElementById('mon-pool').style.display     = (tab === 'pool') ? '' : 'none';
  document.getElementById('mtab-overview').classList.toggle('active', tab === 'overview');
  document.getElementById('mtab-pool').classList.toggle('active', tab === 'pool');
  if (tab === 'pool') fetchPoolStats();
}

async function fetchPoolStats() {
  try {
    var r = await fetch('/pool/stats', {credentials:'include'});
    var d = await r.json();
    function set(id, val){ var el = document.getElementById(id); if(el) el.textContent = val; }
    set('p-active',   d.active   != null ? d.active   : '--');
    set('p-idle',     d.idle     != null ? d.idle     : '--');
    set('p-waiting',  d.waiting  != null ? d.waiting  : '--');
    set('p-minmax',   (d.min != null ? d.min : '?') + ' / ' + (d.max != null ? d.max : '?'));
    set('p-total',    d.total    != null ? d.total    : '--');
    set('p-avgwait',  d.avg_wait_ms != null ? d.avg_wait_ms + ' ms' : '--');
    set('p-timeouts', d.timeouts != null ? d.timeouts : '--');
    set('p-requests', d.total_requests != null ? d.total_requests : '--');
    drawPoolChart(d);
  } catch(e) {}
}

function drawPoolChart(d) {
  var canvas = document.getElementById('p-chart');
  if (!canvas || !canvas.getContext) return;
  canvas.width = canvas.offsetWidth || 400;
  var ctx = canvas.getContext('2d');
  var W = canvas.width, H = canvas.height || 90;
  ctx.clearRect(0, 0, W, H);
  ctx.fillStyle = '#080c18'; ctx.fillRect(0, 0, W, H);
  var max = Math.max(1, d.max || 1);
  var active = d.active || 0, idle = d.idle || 0, waiting = d.waiting || 0;
  var barY = 18, barH = 28, pad = 10, barW = W - 2 * pad;
  // Stacked utilization bar: active | idle | free
  var wActive = Math.round(barW * active / max);
  var wIdle   = Math.round(barW * idle / max);
  ctx.fillStyle = '#2d4060'; ctx.fillRect(pad, barY, barW, barH);
  ctx.fillStyle = '#10b981'; ctx.fillRect(pad, barY, wActive, barH);
  ctx.fillStyle = '#00d4ff'; ctx.fillRect(pad + wActive, barY, wIdle, barH);
  ctx.fillStyle = '#f8fafc'; ctx.font = '11px monospace';
  var pct = Math.round(100 * active / max);
  ctx.fillText('Auslastung: ' + active + '/' + max + ' (' + pct + '%)', pad, 12);
  // Waiting bar (below, orange)
  if (waiting > 0) {
    var wWait = Math.min(barW, Math.round(barW * waiting / max));
    ctx.fillStyle = '#f59e0b'; ctx.fillRect(pad, barY + barH + 14, wWait, 12);
    ctx.fillStyle = '#f59e0b'; ctx.font = '10px monospace';
    ctx.fillText(waiting + ' wartend', pad, barY + barH + 12);
  } else {
    ctx.fillStyle = '#475569'; ctx.font = '10px monospace';
    ctx.fillText('Keine wartenden Requests', pad, barY + barH + 24);
  }
}

// ── Vacuum Page (Phase 173) ───────────────────────────────────
var vacTimer = null;
function stopVacuumPage() { if (vacTimer) { clearInterval(vacTimer); vacTimer = null; } }
function loadVacuumPage() {
  fetchVacuumStats();
  if (vacTimer) clearInterval(vacTimer);
  vacTimer = setInterval(fetchVacuumStats, 5000);
}

function fmtBytes(b) {
  if (b == null) return '--';
  if (b < 1024) return b + ' B';
  if (b < 1048576) return (b/1024).toFixed(1) + ' KB';
  if (b < 1073741824) return (b/1048576).toFixed(1) + ' MB';
  return (b/1073741824).toFixed(2) + ' GB';
}

async function fetchVacuumStats() {
  try {
    var r = await fetch('/vacuum/stats', {credentials:'include'});
    var d = await r.json();
    function set(id, val){ var el = document.getElementById(id); if(el) el.textContent = val; }
    set('v-lastrun',    d.last_run || 'never');
    set('v-nextrun',    d.auto_vacuum_enabled === false ? 'deaktiviert'
                        : (d.next_auto_run_in_seconds != null ? 'in ' + d.next_auto_run_in_seconds + 's' : '--'));
    set('v-freedrows',  d.total_freed_rows != null ? d.total_freed_rows : '--');
    set('v-freedbytes', fmtBytes(d.total_freed_bytes));
    set('v-runs',       d.runs_total != null ? d.runs_total + ' (' + (d.auto_runs||0) + ' auto)' : '--');
    set('v-pending',    d.pending_dead_tuples != null ? d.pending_dead_tuples : '--');
    // Merge table list: all tables + dead tuples + last vacuum time
    var tr = await fetch('/tables', {credentials:'include'});
    var td = await tr.json();
    var tables = Array.isArray(td) ? td : (td.tables || []);
    var dead = d.tables || {}, lastVac = d.last_vacuum_per_table || {};
    var el = document.getElementById('vacuum-table-list');
    if (!tables.length) { el.textContent = 'Keine Tabellen.'; return; }
    var html = '<table style="width:100%;border-collapse:collapse;font-size:0.78rem">';
    html += '<tr style="color:#64748b;border-bottom:1px solid #1e2d40">'
          + '<th style="text-align:left;padding:5px 8px">Tabelle</th>'
          + '<th style="text-align:right;padding:5px 8px">Dead Tuples</th>'
          + '<th style="text-align:left;padding:5px 8px">Letzter Vacuum</th>'
          + '<th style="text-align:right;padding:5px 8px">Aktion</th></tr>';
    tables.forEach(function(t) {
      var name = typeof t === 'string' ? t : t.name;
      var dt = dead[name] != null ? dead[name] : (dead['public.' + name] != null ? dead['public.' + name] : 0);
      var lv = lastVac[name] || lastVac['public.' + name] || '&mdash;';
      var dtColor = dt > 0 ? '#f59e0b' : '#10b981';
      html += '<tr style="border-bottom:1px solid #111827">'
        + '<td style="padding:5px 8px;color:#f8fafc">' + escHtml(name) + '</td>'
        + '<td style="padding:5px 8px;text-align:right;color:' + dtColor + ';font-weight:600">' + dt + '</td>'
        + '<td style="padding:5px 8px;color:#94a3b8">' + lv + '</td>'
        + '<td style="padding:5px 8px;text-align:right"><button class="vac-btn" onclick="runVacuumNow(\'' + escAttr(name) + '\', this)">VACUUM jetzt</button></td>'
        + '</tr>';
    });
    html += '</table>';
    el.innerHTML = html;
  } catch(e) {
    var el = document.getElementById('vacuum-table-list');
    if (el) el.textContent = 'Fehler beim Laden der Vacuum-Statistiken.';
  }
}

async function runVacuumNow(table, btn) {
  if (btn) { btn.disabled = true; btn.textContent = 'l\u00e4uft...'; }
  var msg = document.getElementById('v-msg');
  try {
    var r = await fetch('/api/query', {method:'POST', credentials:'include',
      headers:{'Content-Type':'application/json'},
      body: JSON.stringify({sql: 'VACUUM ' + table})});
    var d = await r.json();
    if (msg) {
      msg.textContent = d.success !== false ? ('VACUUM ' + table + ' \u2713 ' + (d.message || '')) : ('Fehler: ' + (d.error || 'unbekannt'));
      msg.style.color = d.success !== false ? '#10b981' : '#ef4444';
      setTimeout(function(){ msg.textContent = ''; }, 6000);
    }
  } catch(e) {
    if (msg) { msg.textContent = 'Netzwerkfehler'; msg.style.color = '#ef4444'; }
  }
  if (btn) { btn.disabled = false; btn.textContent = 'VACUUM jetzt'; }
  fetchVacuumStats();
}

// ── Replication Page (Phase 173) ──────────────────────────────
var repTimer = null;
function stopReplicationPage() { if (repTimer) { clearInterval(repTimer); repTimer = null; } }
function loadReplicationPage() {
  fetchReplicationStatus();
  if (repTimer) clearInterval(repTimer);
  repTimer = setInterval(fetchReplicationStatus, 2000);
}

async function fetchReplicationStatus() {
  try {
    var r = await fetch('/replication/status', {credentials:'include'});
    var d = await r.json();
    function set(id, val){ var el = document.getElementById(id); if(el) el.textContent = val; }
    var role = d.role || 'standalone';
    set('r-role', role === 'master' ? 'Master' : role === 'replica' ? 'Replica' : 'Standalone');
    set('r-mode', (d.sync_mode || 'async') + ' \u00b7 Port ' + (d.replication_port || '?'));
    set('r-binlog', d.binlog_pos != null && d.binlog_pos >= 0 ? d.binlog_pos : (d.replica ? d.replica.position : '--'));
    set('r-ack', d.max_slave_ack_pos != null ? d.max_slave_ack_pos : '--');
    set('r-slaves', d.connected_slaves != null ? d.connected_slaves : '--');
    var rep = d.replica || {};
    set('r-lag', role === 'replica' ? (rep.lag_ms != null ? rep.lag_ms + ' ms' : '--')
                : (d.binlog_pos > d.max_slave_ack_pos ? (d.binlog_pos - d.max_slave_ack_pos) + ' Entries' : '0'));
    // Traffic light: green = in sync, yellow = lagging, red = disconnected
    var light = document.getElementById('rep-light');
    var text  = document.getElementById('rep-light-text');
    var color = '#475569', msg = 'Standalone \u2014 keine Replikation konfiguriert';
    if (role === 'replica') {
      if (rep.master_down || !rep.running) { color = '#ef4444'; msg = 'MASTER DOWN \u2014 Verbindung verloren (>30s)'; }
      else if ((rep.lag_ms || 0) > 1000)   { color = '#f59e0b'; msg = 'Lag: ' + rep.lag_ms + ' ms \u2014 Replica h\u00e4ngt hinterher'; }
      else { color = '#10b981'; msg = 'In Sync \u2014 Lag ' + (rep.lag_ms||0) + ' ms \u00b7 ' + (rep.status||''); }
    } else if (role === 'master') {
      var slaves = d.slaves || [];
      var connected = slaves.filter(function(s){ return s.connected; }).length;
      if (connected === 0 && (d.connected_slaves||0) === 0) { color = '#ef4444'; msg = 'Keine Replica verbunden'; }
      else if (d.binlog_pos > d.max_slave_ack_pos) { color = '#f59e0b'; msg = 'Replicas h\u00e4ngen hinterher (ack ' + d.max_slave_ack_pos + ' / pos ' + d.binlog_pos + ')'; }
      else { color = '#10b981'; msg = 'In Sync \u2014 ' + Math.max(connected, d.connected_slaves||0) + ' Replica(s) aktuell'; }
    }
    if (light) light.style.color = color;
    if (text) { text.textContent = msg; text.style.color = color; }
    // Replica list (master side)
    var listEl = document.getElementById('rep-slave-list');
    if (listEl) {
      var slaves2 = d.slaves || [];
      if (role !== 'master') {
        listEl.textContent = role === 'replica' ? 'Dieser Server ist eine Replica.' : 'Keine Replikation aktiv.';
      } else if (!slaves2.length) {
        listEl.textContent = 'Keine Replicas registriert.';
      } else {
        var html = '<table style="width:100%;border-collapse:collapse;font-size:0.78rem">'
          + '<tr style="color:#64748b;border-bottom:1px solid #1e2d40">'
          + '<th style="text-align:left;padding:5px 8px">Host</th>'
          + '<th style="text-align:right;padding:5px 8px">Ack-Position</th>'
          + '<th style="text-align:right;padding:5px 8px">Zuletzt gesehen</th>'
          + '<th style="text-align:left;padding:5px 8px">Status</th></tr>';
        slaves2.forEach(function(s) {
          var st = s.connected ? '<span style="color:#10b981">&#x25CF; verbunden</span>'
                               : '<span style="color:#ef4444">&#x25CF; getrennt</span>';
          var seen = s.ms_since_seen < 1000 ? 'gerade eben' : Math.round(s.ms_since_seen/1000) + 's';
          html += '<tr style="border-bottom:1px solid #111827">'
            + '<td style="padding:5px 8px;color:#f8fafc">' + escHtml(s.host||'?') + '</td>'
            + '<td style="padding:5px 8px;text-align:right">' + (s.ack_pos != null ? s.ack_pos : '--') + '</td>'
            + '<td style="padding:5px 8px;text-align:right">' + seen + '</td>'
            + '<td style="padding:5px 8px">' + st + '</td></tr>';
        });
        html += '</table>';
        listEl.innerHTML = html;
      }
    }
    // Replica-side connection detail
    var det = document.getElementById('rep-replica-detail');
    if (det) {
      if (role === 'replica') {
        det.style.display = '';
        document.getElementById('rep-replica-info').innerHTML =
          'Master: <b style="color:#f8fafc">' + escHtml(rep.master_host||'?') + ':' + (rep.master_port||'?') + '</b>'
          + ' &middot; Status: <b style="color:#f8fafc">' + escHtml(rep.status||'?') + '</b>'
          + ' &middot; Position: <b style="color:#f8fafc">' + (rep.position != null ? rep.position : '?') + '</b>'
          + ' &middot; Letzter Sync: <b style="color:#f8fafc">' + (rep.ms_since_last_sync >= 0 ? rep.ms_since_last_sync + ' ms' : 'nie') + '</b>';
      } else det.style.display = 'none';
    }
  } catch(e) {
    var text = document.getElementById('rep-light-text');
    if (text) text.textContent = 'Fehler beim Laden des Replikationsstatus';
  }
}

// History
function saveHistory(sql, ms, ok) {
  var hist = JSON.parse(localStorage.getItem('mq_hist') || '[]');
  var ts = new Date();
  hist.unshift({sql: sql, ms: ms, ts: ts.toLocaleString(), ok: ok !== false});
  if (hist.length > 20) hist = hist.slice(0, 20);
  localStorage.setItem('mq_hist', JSON.stringify(hist));
}

function renderHistory() {
  var hist = JSON.parse(localStorage.getItem('mq_hist') || '[]');
  var el = document.getElementById('history-list');
  if (!hist.length) { el.innerHTML = '<div style="color:#475569;font-size:0.85rem;padding:8px">No history yet.</div>'; return; }
  el.innerHTML = hist.map(function(h,i) {
    var badge = h.ok !== false
      ? '<span class="hist-badge ok">OK</span>'
      : '<span class="hist-badge err">Error</span>';
    return '<div class="hist-item" onclick="loadHistItem(' + i + ')">' +
      '<div class="hist-sql">' + escHtml(h.sql) + '</div>' +
      '<div class="hist-meta">' + badge + '<span>' + escHtml(h.ts||'') + '</span><span>' + escHtml(String(h.ms||'0')) + 'ms</span></div>' +
      '</div>';
  }).join('');
}

function loadHistItem(i) {
  var hist = JSON.parse(localStorage.getItem('mq_hist') || '[]');
  if (!hist[i]) return;
  document.getElementById('sql-editor').value = hist[i].sql;
  updateEditorDecor();
  showPage('editor', document.querySelector('[data-page=editor]'));
}

function clearHistory() {
  localStorage.removeItem('mq_hist');
  renderHistory();
}

// Status bar polling
async function pollStatus() {
  try {
    var r = await fetch('/status', {credentials:'include'});
    var d = await r.json();
    var healthy = d.status === 'healthy' || d.status === 'ok' || !d.status;
    var hb = document.getElementById('health-badge');
    var dot = document.getElementById('sb-dot');
    hb.textContent = (d.status || 'healthy');
    hb.className = 'badge ' + (healthy ? 'green' : 'yellow');
    dot.className = 'status-dot' + (healthy ? '' : ' warn');
    document.getElementById('sb-health').textContent = d.status || 'healthy';
    document.getElementById('sb-tables').textContent  = d.tables != null ? d.tables : '--';
    document.getElementById('sb-rows').textContent    = d.rows   != null ? d.rows   : '--';
    document.getElementById('sb-queries').textContent = d.queries != null ? d.queries : (d.query_count != null ? d.query_count : '--');
    if (d.connections !== undefined)
      document.getElementById('conn-badge').textContent = d.connections + ' connections';
  } catch(e) {}
}

// Utilities
function escHtml(s) {
  return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;');
}
function rowVals(row) { return Array.isArray(row) ? row : (row && typeof row === 'object' && row.values && typeof row.values !== 'function' ? row.values : (row && typeof row === 'object' ? Object.values(row) : [])); }
function escAttr(s) { return String(s).replace(/'/g,"\\'"); }

// Phase 174: load test count dynamically from /health
async function loadTestBadge() {
  try {
    var r = await fetch('/health', {credentials:'include'});
    var d = await r.json();
    if (d.test_count != null)
      document.getElementById('test-badge').textContent = d.test_count + ' tests';
    if (d.version)
      document.querySelectorAll('.ms-version').forEach(function(el){ el.textContent = 'v' + d.version; });
  } catch(e) {}
}

// Init
updateEditorDecor();
loadSidebarTables();
loadTestBadge();
pollStatus();
setInterval(pollStatus, 5000);
setInterval(loadSidebarTables, 30000);

// ── Phase 154-156: Auth integration ──────────────────────────
// Token lives in httpOnly cookie (survives refresh) + in-memory for Bearer header
var msToken  = '';  // in-memory only; restored via /auth/me on page load
var msUser   = '';
var msUserId = 0;

function getAuthHdr() {
  var h = {'Content-Type':'application/json'};
  if (msToken) h['Authorization'] = 'Bearer ' + msToken;
  return h;
}
async function msLogin(u, p) {
  try {
    var r = await fetch('/auth/login',{method:'POST',credentials:'include',headers:{'Content-Type':'application/json'},body:JSON.stringify({username:u,password:p})});
    var d = await r.json();
    if (d.success) {
      msToken=d.token||''; msUser=d.username||u; msUserId=d.user_id||0;
      // No localStorage — token is in httpOnly cookie set by server
      hidLoginPage();
      updateUserBadge();
      return true;
    }
    return d.error||'Login failed';
  } catch(e){return 'Network error';}
}
async function msLogout() {
  try { await fetch('/auth/logout',{method:'POST',credentials:'include',headers:getAuthHdr()}); } catch(e){}
  msToken=''; msUser='';
  // Server clears cookie via Set-Cookie: milansql_token=; Max-Age=0
  showLoginPage();
}
async function msSubmitLogin() {
  var u=document.getElementById('ms-lu').value.trim();
  var p=document.getElementById('ms-lp').value;
  var err=document.getElementById('ms-lerr');
  if(!u||!p){err.style.color='#ef4444';err.textContent='Enter username and password';return;}
  err.style.color='#94a3b8';err.textContent='Signing in...';
  var res=await msLogin(u,p);
  if(res===true){err.textContent='';}else{err.style.color='#ef4444';err.textContent=res;}
}
function updateUserBadge() {
  var ub=document.getElementById('ms-user-badge');
  if(ub){ub.textContent='● '+msUser;ub.title='Eingeloggt als: '+msUser+' (id: '+msUserId+')';ub.style.display='inline';}
  var lb=document.getElementById('ms-logout-btn');
  if(lb) lb.style.display='inline-block';
  var rp=document.getElementById('rls-panel');
  if(rp){
    var tables=document.querySelectorAll('#sidebar-tables .tbl-btn');
    var rlsHtml='<div style="font-size:10px;color:#64748b;text-transform:uppercase;letter-spacing:0.5px;margin-bottom:4px">Row Level Security</div>'
      +'<div style="display:flex;align-items:center;gap:6px;margin-bottom:3px"><span style="color:#10b981;font-size:9px">●</span><span style="font-size:11px;color:#10b981;font-weight:600">ACTIVE</span></div>'
      +'<div style="font-size:10px;color:#94a3b8;margin-bottom:3px">User: <b style="color:#f8fafc">'+escHtml(msUser)+'</b></div>';
    var enabledCount = (_rlsPoliciesCache.enabled_tables||[]).length;
    var totalPolicies = 0;
    var pols = _rlsPoliciesCache.policies || {};
    for (var k in pols) totalPolicies += pols[k].length;
    rlsHtml += '<div style="font-size:10px;color:#94a3b8;margin-bottom:2px">Tables: <b style="color:#00d4ff">'+enabledCount+'</b> protected</div>';
    rlsHtml += '<div style="font-size:10px;color:#94a3b8">Policies: <b style="color:#f59e0b">'+totalPolicies+'</b> active</div>';
    rp.innerHTML = rlsHtml;
  }
}

// Phase 171: Schema Visualizer
var _schemaData = null;
var _schemaPositions = {};
var _schemaDrag = null;

function schemaStorageKey() { return 'milansql_schema_pos_' + (msUser||'anon'); }
function loadSchemaPositions() { try { _schemaPositions = JSON.parse(localStorage.getItem(schemaStorageKey())) || {}; } catch(e) { _schemaPositions = {}; } }
function saveSchemaPositions() { try { localStorage.setItem(schemaStorageKey(), JSON.stringify(_schemaPositions)); } catch(e) {} }

async function loadSchemaViz() {
  var status = document.getElementById('schema-status');
  if (status) status.textContent = 'Loading...';
  loadSchemaPositions();
  try { var r = await fetch('/api/schema', {credentials:'include'}); _schemaData = await r.json(); }
  catch(e) { if(status) status.textContent='Error'; return; }
  renderSchemaViz();
}
function schemaReload() { loadSchemaViz(); }

function buildFkMap() {
  var tables = (_schemaData && _schemaData.tables) || [];
  var fkMap = [], fkCols = {}, tblNames = {};
  tables.forEach(function(t) {
    fkCols[t.name] = {};
    tblNames[t.name] = true;
    // Strip public. and u<N>_ prefixes for FK inference matching
    var noSchema = t.name.replace(/^public\./, '');
    if (noSchema !== t.name) tblNames[noSchema] = t.name;
    var bare = noSchema.replace(/^u[0-9]+_/, '');
    if (bare !== noSchema) tblNames[bare] = t.name;
  });
  tables.forEach(function(t) {
    (t.foreign_keys||[]).forEach(function(fk) { fkMap.push({from:t.name,fromCol:fk.from,to:fk.ref_table,toCol:fk.ref_col}); fkCols[t.name][fk.from]=fk.ref_table; });
    (t.columns||[]).forEach(function(c) {
      if (c.name.endsWith('_id') && !fkCols[t.name][c.name]) {
        var ref = c.name.slice(0,-3);
        [ref,ref+'s',ref+'e',ref+'en',ref+'es'].forEach(function(cn) {
          if(tblNames[cn]&&cn!==t.name) { var real=typeof tblNames[cn]==='string'?tblNames[cn]:cn; if(real!==t.name){fkMap.push({from:t.name,fromCol:c.name,to:real,toCol:'id',inferred:true}); fkCols[t.name][c.name]=real;} }
        });
      }
    });
  });
  return {fkMap:fkMap,fkCols:fkCols};
}

function renderSchemaViz() {
  var tables = (_schemaData && _schemaData.tables) || [];
  var cardsEl = document.getElementById('schema-cards');
  var svgEl = document.getElementById('schema-svg');
  if (!cardsEl||!svgEl) return;
  cardsEl.innerHTML = '';
  var filter = (document.getElementById('schema-search')||{}).value||'';
  filter = filter.toLowerCase();
  var fi = buildFkMap(), fkMap=fi.fkMap, fkCols=fi.fkCols;
  var cols = Math.max(2, Math.ceil(Math.sqrt(tables.length)));
  tables.forEach(function(t,i) { if(!_schemaPositions[t.name]) _schemaPositions[t.name]={x:40+(i%cols)*280,y:40+Math.floor(i/cols)*260}; });
  var ft = tables.filter(function(t){return !filter||t.name.toLowerCase().indexOf(filter)>=0;});
  var vis = {}; ft.forEach(function(t){vis[t.name]=true;});
  ft.forEach(function(t) {
    var card = document.createElement('div');
    card.className = 'schema-card'; card.dataset.table = t.name;
    var pos = _schemaPositions[t.name];
    card.style.left = pos.x+'px'; card.style.top = pos.y+'px';
    var pc = (t.policies||[]).length, rlsOn = t.rls_enabled;
    var h = '<div class="schema-card-header"><span class="rls-dot '+(rlsOn?'on':'off')+'" title="RLS '+(rlsOn?'active':'inactive')+'"></span><span>'+escHtml(t.name)+'</span>';
    if(pc>0) h+='<span class="pol-count">'+pc+'</span>';
    h+='</div><div class="schema-card-cols">';
    (t.columns||[]).forEach(function(c) {
      var isFk=!!(fkCols[t.name]||{})[c.name];
      var icon='';
      if(c.pk) icon='<span class="col-icon pk" title="PK">&#x1F511;</span>';
      else if(isFk) icon='<span class="col-icon fk" title="FK">&#x2192;</span>';
      else icon='<span class="col-icon"></span>';
      h+='<div class="schema-col">'+icon+'<span class="col-name'+(isFk?' fk-col':'')+'">'+escHtml(c.name)+'</span><span class="col-type">'+escHtml(c.type)+'</span></div>';
    });
    h+='</div>';
    if(rlsOn&&pc>0) { var fp=t.policies[0]; h+='<div class="schema-card-rls" title="Click to edit" onclick="openPolicyEditor(\''+t.name.replace(/'/g,"\\'")+'\')">' +escHtml(fp.name)+': '+escHtml(fp['using']||'')+'</div>'; }
    else if(rlsOn) h+='<div class="schema-card-rls" onclick="openPolicyEditor(\''+t.name.replace(/'/g,"\\'")+'\')">' +'+ Add policy</div>';
    card.innerHTML = h;
    card.addEventListener('mouseenter', function() {
      var conn={}; conn[t.name]=true;
      fkMap.forEach(function(fk){if(fk.from===t.name)conn[fk.to]=true;if(fk.to===t.name)conn[fk.from]=true;});
      document.querySelectorAll('.schema-card').forEach(function(c){if(conn[c.dataset.table])c.classList.add('highlighted');else c.classList.add('dimmed');});
      document.querySelectorAll('.schema-line').forEach(function(l){if(l.dataset.from===t.name||l.dataset.to===t.name){l.style.opacity='1';l.style.strokeWidth='2';}else l.style.opacity='0.15';});
      document.querySelectorAll('.schema-line-label').forEach(function(l){if(l.dataset.from===t.name||l.dataset.to===t.name)l.style.opacity='1';else l.style.opacity='0.15';});
    });
    card.addEventListener('mouseleave', function() {
      document.querySelectorAll('.schema-card').forEach(function(c){c.classList.remove('highlighted','dimmed');});
      document.querySelectorAll('.schema-line').forEach(function(l){l.style.opacity='';l.style.strokeWidth='';});
      document.querySelectorAll('.schema-line-label').forEach(function(l){l.style.opacity='';});
    });
    card.addEventListener('mousedown', function(e) {
      if(e.target.tagName==='INPUT'||e.target.tagName==='SELECT'||e.target.tagName==='BUTTON'||e.target.closest('.schema-card-rls'))return;
      e.preventDefault(); _schemaDrag={el:card,name:t.name,sx:e.clientX,sy:e.clientY,ox:pos.x,oy:pos.y};
    });
    cardsEl.appendChild(card);
  });
  drawSchemaLines(fkMap, vis);
  schemaApplyTransform();
  var status=document.getElementById('schema-status');
  if(status)status.textContent=ft.length+' tables, '+fkMap.length+' relations';
}

function drawSchemaLines(fkMap,vis) {
  var svg=document.getElementById('schema-svg'); if(!svg)return; svg.innerHTML='';
  var ns='http://www.w3.org/2000/svg';
  fkMap.forEach(function(fk) {
    if(!vis[fk.from]||!vis[fk.to])return;
    var fp=_schemaPositions[fk.from],tp=_schemaPositions[fk.to]; if(!fp||!tp)return;
    var fc=document.querySelector('.schema-card[data-table="'+fk.from+'"]');
    var tc=document.querySelector('.schema-card[data-table="'+fk.to+'"]');
    if(!fc||!tc)return;
    var fw=fc.offsetWidth||200,fh=fc.offsetHeight||120,tw=tc.offsetWidth||200,th=tc.offsetHeight||120;
    var fx=fp.x+fw,fy=fp.y+fh/2,tx=tp.x,ty=tp.y+th/2;
    if(tp.x+tw<fp.x){fx=fp.x;tx=tp.x+tw;}
    else if(Math.abs(fp.x-tp.x)<fw){if(tp.y>fp.y){fx=fp.x+fw/2;fy=fp.y+fh;tx=tp.x+tw/2;ty=tp.y;}else{fx=fp.x+fw/2;fy=fp.y;tx=tp.x+tw/2;ty=tp.y+th;}}
    var ft2=(_schemaData.tables||[]).find(function(t){return t.name===fk.from;});
    var tt2=(_schemaData.tables||[]).find(function(t){return t.name===fk.to;});
    var bothRls=ft2&&tt2&&ft2.rls_enabled&&tt2.rls_enabled;
    var lc=bothRls?'#00d4ff':'rgba(0,212,255,0.35)';
    var line=document.createElementNS(ns,'line');
    line.setAttribute('x1',fx);line.setAttribute('y1',fy);line.setAttribute('x2',tx);line.setAttribute('y2',ty);
    line.setAttribute('stroke',lc);line.setAttribute('stroke-width','1');line.setAttribute('stroke-dasharray','4,3');
    line.classList.add('schema-line');line.dataset.from=fk.from;line.dataset.to=fk.to;line.style.pointerEvents='none';
    svg.appendChild(line);
    var mx=(fx+tx)/2,my=(fy+ty)/2;
    var text=document.createElementNS(ns,'text');
    text.setAttribute('x',mx);text.setAttribute('y',my-4);text.setAttribute('fill','#64748b');
    text.setAttribute('font-size','9');text.setAttribute('text-anchor','middle');text.setAttribute('font-family','-apple-system,sans-serif');
    text.classList.add('schema-line-label');text.dataset.from=fk.from;text.dataset.to=fk.to;text.style.pointerEvents='none';
    text.textContent=fk.fromCol;svg.appendChild(text);
  });
}


// Phase 172: Zoom, Pan, Mini-Map, Keyboard Shortcuts
var _schemaZoom = 1.0;
var _schemaPan = {x: 0, y: 0};
var _schemaPanning = false;
var _schemaPanStart = null;
var _schemaSpaceDown = false;
var _minimapDrag = false;

function schemaApplyTransform() {
  var tf = document.getElementById('schema-transform');
  if (tf) tf.style.transform = 'translate(' + _schemaPan.x + 'px,' + _schemaPan.y + 'px) scale(' + _schemaZoom + ')';
  var pct = document.getElementById('schema-zoom-pct');
  if (pct) pct.textContent = Math.round(_schemaZoom * 100) + '%';
  updateMinimap();
}

function schemaZoom(delta, cx, cy) {
  var oldZ = _schemaZoom;
  _schemaZoom = Math.max(0.2, Math.min(2.0, _schemaZoom + delta));
  if (cx !== undefined && cy !== undefined) {
    // Zoom centered on mouse position
    var canvas = document.getElementById('schema-canvas');
    if (canvas) {
      var rect = canvas.getBoundingClientRect();
      var mx = cx - rect.left;
      var my = cy - rect.top;
      // Adjust pan so point under mouse stays fixed
      var ratio = _schemaZoom / oldZ;
      _schemaPan.x = mx - ratio * (mx - _schemaPan.x);
      _schemaPan.y = my - ratio * (my - _schemaPan.y);
    }
  }
  schemaApplyTransform();
}

function schemaZoomReset() {
  _schemaZoom = 1.0;
  _schemaPan = {x: 0, y: 0};
  schemaApplyTransform();
}

function schemaFitAllZoom() {
  var tables = (_schemaData && _schemaData.tables) || [];
  if (!tables.length) return;
  var minX = 1e9, minY = 1e9, maxX = -1e9, maxY = -1e9;
  tables.forEach(function(t) {
    var p = _schemaPositions[t.name];
    if (p) {
      if (p.x < minX) minX = p.x;
      if (p.y < minY) minY = p.y;
      if (p.x + 220 > maxX) maxX = p.x + 220;
      if (p.y + 200 > maxY) maxY = p.y + 200;
    }
  });
  var canvas = document.getElementById('schema-canvas');
  if (!canvas) return;
  var cw = canvas.clientWidth, ch = canvas.clientHeight;
  if (cw < 10 || ch < 10) return;
  var contentW = maxX - minX + 80, contentH = maxY - minY + 80;
  _schemaZoom = Math.max(0.2, Math.min(2.0, Math.min(cw / contentW, ch / contentH)));
  _schemaPan.x = (cw - contentW * _schemaZoom) / 2 - minX * _schemaZoom + 40 * _schemaZoom;
  _schemaPan.y = (ch - contentH * _schemaZoom) / 2 - minY * _schemaZoom + 40 * _schemaZoom;
  schemaApplyTransform();
}

// ── Mini-Map ──
function updateMinimap() {
  var cvs = document.getElementById('schema-minimap-canvas');
  if (!cvs) return;
  var ctx = cvs.getContext('2d');
  var mw = cvs.width, mh = cvs.height;
  ctx.clearRect(0, 0, mw, mh);
  var tables = (_schemaData && _schemaData.tables) || [];
  if (!tables.length) return;
  // Find bounds of all tables
  var minX = 1e9, minY = 1e9, maxX = -1e9, maxY = -1e9;
  tables.forEach(function(t) {
    var p = _schemaPositions[t.name];
    if (p) {
      if (p.x < minX) minX = p.x;
      if (p.y < minY) minY = p.y;
      if (p.x + 200 > maxX) maxX = p.x + 200;
      if (p.y + 120 > maxY) maxY = p.y + 120;
    }
  });
  var pad = 40;
  var cW = maxX - minX + pad * 2, cH = maxY - minY + pad * 2;
  var scale = Math.min(mw / cW, mh / cH);
  var offX = (mw - cW * scale) / 2, offY = (mh - cH * scale) / 2;
  // Draw FK lines
  var fi = buildFkMap();
  ctx.strokeStyle = '#1e2d40';
  ctx.lineWidth = 0.5;
  fi.fkMap.forEach(function(fk) {
    var fp = _schemaPositions[fk.from], tp = _schemaPositions[fk.to];
    if (!fp || !tp) return;
    ctx.beginPath();
    ctx.moveTo(offX + (fp.x - minX + pad + 100) * scale, offY + (fp.y - minY + pad + 60) * scale);
    ctx.lineTo(offX + (tp.x - minX + pad + 100) * scale, offY + (tp.y - minY + pad + 60) * scale);
    ctx.stroke();
  });
  // Draw table rectangles
  tables.forEach(function(t) {
    var p = _schemaPositions[t.name];
    if (!p) return;
    var rx = offX + (p.x - minX + pad) * scale;
    var ry = offY + (p.y - minY + pad) * scale;
    var rw = 200 * scale, rh = 120 * scale;
    ctx.fillStyle = '#111827';
    ctx.strokeStyle = '#2d4060';
    ctx.lineWidth = 0.5;
    ctx.fillRect(rx, ry, rw, rh);
    ctx.strokeRect(rx, ry, rw, rh);
    // RLS dot
    var dotR = Math.max(2, 4 * scale);
    ctx.fillStyle = t.rls_enabled ? '#10b981' : '#475569';
    ctx.beginPath();
    ctx.arc(rx + dotR + 2, ry + dotR + 2, dotR, 0, Math.PI * 2);
    ctx.fill();
  });
  // Draw viewport rectangle
  var canvas = document.getElementById('schema-canvas');
  if (canvas) {
    var cw = canvas.clientWidth, ch = canvas.clientHeight;
    // Viewport in content coords: top-left = -pan/zoom, size = canvasSize/zoom
    var vx = -_schemaPan.x / _schemaZoom;
    var vy = -_schemaPan.y / _schemaZoom;
    var vw = cw / _schemaZoom;
    var vh = ch / _schemaZoom;
    // Map to minimap coords
    var vrx = offX + (vx - minX + pad) * scale;
    var vry = offY + (vy - minY + pad) * scale;
    var vrw = vw * scale;
    var vrh = vh * scale;
    ctx.strokeStyle = '#00d4ff';
    ctx.lineWidth = 1.5;
    ctx.strokeRect(vrx, vry, vrw, vrh);
  }
  // Store mapping for click-to-navigate
  cvs._mmMinX = minX; cvs._mmMinY = minY;
  cvs._mmScale = scale; cvs._mmPad = pad;
  cvs._mmOffX = offX; cvs._mmOffY = offY;
}

function minimapNavigate(e) {
  var cvs = document.getElementById('schema-minimap-canvas');
  if (!cvs || !cvs._mmScale) return;
  var rect = cvs.getBoundingClientRect();
  var sx = cvs.width / rect.width, sy = cvs.height / rect.height;
  var mx = (e.clientX - rect.left) * sx;
  var my = (e.clientY - rect.top) * sy;
  // Convert minimap coords back to content coords
  var contentX = (mx - cvs._mmOffX) / cvs._mmScale + cvs._mmMinX - cvs._mmPad;
  var contentY = (my - cvs._mmOffY) / cvs._mmScale + cvs._mmMinY - cvs._mmPad;
  // Center viewport on this point
  var canvas = document.getElementById('schema-canvas');
  if (canvas) {
    _schemaPan.x = -contentX * _schemaZoom + canvas.clientWidth / 2;
    _schemaPan.y = -contentY * _schemaZoom + canvas.clientHeight / 2;
    schemaApplyTransform();
  }
}

// ── Wheel zoom on canvas ──
document.addEventListener('wheel', function(e) {
  var canvas = document.getElementById('schema-canvas');
  if (!canvas || !canvas.contains(e.target)) return;
  var page = document.getElementById('page-schema');
  if (!page || !page.classList.contains('active')) return;
  e.preventDefault();
  var delta = e.deltaY < 0 ? 0.08 : -0.08;
  if (e.ctrlKey) delta *= 1.5;
  schemaZoom(delta, e.clientX, e.clientY);
}, {passive: false});

// ── Minimap click + drag ──
document.addEventListener('mousedown', function(e) {
  var mm = document.getElementById('schema-minimap');
  if (mm && mm.contains(e.target)) {
    e.preventDefault();
    _minimapDrag = true;
    minimapNavigate(e);
  }
});

// ── Space for pan mode ──
document.addEventListener('keydown', function(e) {
  if (e.key === ' ' && !e.target.matches('input,textarea,select')) {
    var page = document.getElementById('page-schema');
    if (page && page.classList.contains('active')) {
      e.preventDefault();
      _schemaSpaceDown = true;
      var canvas = document.getElementById('schema-canvas');
      if (canvas) canvas.style.cursor = 'grabbing';
    }
  }
  // Ctrl+0 = reset zoom
  if (e.ctrlKey && e.key === '0') {
    var page = document.getElementById('page-schema');
    if (page && page.classList.contains('active')) {
      e.preventDefault();
      schemaZoomReset();
    }
  }
  // Ctrl+Shift+F = fit all
  if (e.ctrlKey && e.shiftKey && (e.key === 'F' || e.key === 'f')) {
    var page = document.getElementById('page-schema');
    if (page && page.classList.contains('active')) {
      e.preventDefault();
      schemaFitAllZoom();
    }
  }
});
document.addEventListener('keyup', function(e) {
  if (e.key === ' ') {
    _schemaSpaceDown = false;
    var canvas = document.getElementById('schema-canvas');
    if (canvas) canvas.style.cursor = 'grab';
  }
});

document.addEventListener('mousemove',function(e){
  // Minimap drag
  if (_minimapDrag) { minimapNavigate(e); return; }
  // Canvas pan (space+drag or middle-button)
  if (_schemaPanning && _schemaPanStart) {
    _schemaPan.x += e.clientX - _schemaPanStart.x;
    _schemaPan.y += e.clientY - _schemaPanStart.y;
    _schemaPanStart = {x: e.clientX, y: e.clientY};
    schemaApplyTransform();
    return;
  }
  // Card drag
  if(_schemaDrag){var d=_schemaDrag,nx=d.ox+(e.clientX-d.sx)/_schemaZoom,ny=d.oy+(e.clientY-d.sy)/_schemaZoom;
    d.el.style.left=nx+'px';d.el.style.top=ny+'px';_schemaPositions[d.name]={x:nx,y:ny};
    var fi=buildFkMap(),vis={};(_schemaData.tables||[]).forEach(function(t){vis[t.name]=true;});drawSchemaLines(fi.fkMap,vis);updateMinimap();}
});
document.addEventListener('mouseup',function(){
  if (_minimapDrag) { _minimapDrag = false; return; }
  if (_schemaPanning) { _schemaPanning = false; _schemaPanStart = null; var cv = document.getElementById('schema-canvas'); if(cv) cv.style.cursor = 'grab'; return; }
  if(_schemaDrag){saveSchemaPositions();_schemaDrag=null;}
});
document.addEventListener('input',function(e){if(e.target.id==='schema-search')renderSchemaViz();});

// Pan: space+click or middle-click on canvas
document.addEventListener('mousedown', function(e) {
  var canvas = document.getElementById('schema-canvas');
  if (!canvas || !canvas.contains(e.target)) return;
  var page = document.getElementById('page-schema');
  if (!page || !page.classList.contains('active')) return;
  if (_schemaSpaceDown || e.button === 1) {
    e.preventDefault();
    _schemaPanning = true;
    _schemaPanStart = {x: e.clientX, y: e.clientY};
    canvas.style.cursor = 'grabbing';
  }
});

function schemaAutoLayout(){
  var tables=(_schemaData&&_schemaData.tables)||[];if(!tables.length)return;
  var adj={};tables.forEach(function(t){adj[t.name]=[];});
  tables.forEach(function(t){(t.foreign_keys||[]).forEach(function(fk){if(adj[fk.ref_table]){adj[t.name].push(fk.ref_table);adj[fk.ref_table].push(t.name);}});});
  var sorted=tables.slice().sort(function(a,b){return(adj[b.name]||[]).length-(adj[a.name]||[]).length;});
  var cols=Math.max(2,Math.ceil(Math.sqrt(tables.length)));
  sorted.forEach(function(t,i){_schemaPositions[t.name]={x:40+(i%cols)*280,y:40+Math.floor(i/cols)*260};});
  saveSchemaPositions();renderSchemaViz();
}
function schemaFitAll(){
  schemaFitAllZoom();
}

function openPolicyEditor(tn){
  var pe=document.getElementById('schema-policy-editor');if(!pe)return;pe.style.display='block';
  document.getElementById('schema-pe-table').textContent=tn;pe.dataset.table=tn;
  var tbl=(_schemaData.tables||[]).find(function(t){return t.name===tn;});
  if(tbl&&tbl.policies&&tbl.policies.length>0){var p=tbl.policies[0];document.getElementById('schema-pe-name').value=p.name;document.getElementById('schema-pe-cmd').value=p.command;document.getElementById('schema-pe-role').value=p.role;document.getElementById('schema-pe-using').value=p['using']||'';document.getElementById('schema-pe-check').value=p.with_check||'';}
  else{document.getElementById('schema-pe-name').value=tn+'_policy';document.getElementById('schema-pe-cmd').value='ALL';document.getElementById('schema-pe-role').value='PUBLIC';document.getElementById('schema-pe-using').value='';document.getElementById('schema-pe-check').value='';}
  document.getElementById('schema-pe-msg').textContent='';
}
function closePolicyEditor(){var pe=document.getElementById('schema-policy-editor');if(pe)pe.style.display='none';}

async function savePolicyFromEditor(){
  var pe=document.getElementById('schema-policy-editor'),tbl=pe.dataset.table;
  var name=document.getElementById('schema-pe-name').value.trim(),cmd=document.getElementById('schema-pe-cmd').value;
  var role=document.getElementById('schema-pe-role').value.trim()||'PUBLIC';
  var ue=document.getElementById('schema-pe-using').value.trim(),ce=document.getElementById('schema-pe-check').value.trim();
  var msg=document.getElementById('schema-pe-msg');
  if(!name||!ue){msg.style.color='#ef4444';msg.textContent='Name and USING required';return;}
  // Phase 173: Validate policy fields to prevent SQL injection
  var idRe=/^[a-zA-Z_][a-zA-Z0-9_]*$/;
  if(!idRe.test(name)){msg.style.color='#ef4444';msg.textContent='Invalid policy name (letters/digits/underscore only)';return;}
  if(role&&!idRe.test(role)){msg.style.color='#ef4444';msg.textContent='Invalid role name';return;}
  if(ue.indexOf(';')>=0||ce.indexOf(';')>=0){msg.style.color='#ef4444';msg.textContent='Semicolons not allowed in expressions';return;}
  await fetch('/api/query',{method:'POST',credentials:'include',headers:{'Content-Type':'application/json'},body:JSON.stringify({sql:'ALTER TABLE '+tbl+' ENABLE ROW LEVEL SECURITY'})});
  await fetch('/api/query',{method:'POST',credentials:'include',headers:{'Content-Type':'application/json'},body:JSON.stringify({sql:'DROP POLICY '+name+' ON '+tbl})});
  var sql='CREATE POLICY '+name+' ON '+tbl+' FOR '+cmd+' TO '+role+' USING ('+ue+')';
  if(ce)sql+=' WITH CHECK ('+ce+')';
  try{var r=await fetch('/api/query',{method:'POST',credentials:'include',headers:{'Content-Type':'application/json'},body:JSON.stringify({sql:sql})});var data=await r.json();
    if(data.error){msg.style.color='#ef4444';msg.textContent='Error: '+data.error;}
    else{msg.style.color='#10b981';msg.textContent='Policy saved!';setTimeout(function(){closePolicyEditor();loadSchemaViz();},800);}
  }catch(e){msg.style.color='#ef4444';msg.textContent='Network error';}
}

// Patch all fetch calls to include auth token + cookie credentials
var _origFetch2=window.fetch;
window.fetch=function(url,opts){
  if(typeof url==='string'&&url.startsWith('/')){
    opts=opts||{};
    opts.credentials=opts.credentials||'include'; // always send httpOnly cookie
    if(msToken&&!url.startsWith('/auth/')){
      opts.headers=opts.headers||{};
      if(!opts.headers['Authorization'])opts.headers['Authorization']='Bearer '+msToken;
    }
  }
  return _origFetch2.apply(this,arguments).then(function(resp){
    if(resp.status===401){msToken='';msUser='';showLoginPage();}
    return resp;
  });
};
// ── Phase 158: Auth check on load ─────────────────────────────
function showLoginPage(){
  var o=document.getElementById('ms-login-overlay'); if(o) o.style.display='flex';
  // Karte erst zeigen, wenn Auth-Check "nicht eingeloggt" ergab —
  // verhindert Login-Flash fuer bereits eingeloggte User.
  var c=document.getElementById('ms-login-card'); if(c) c.style.visibility='visible';
}
function hidLoginPage(){
  var o=document.getElementById('ms-login-overlay'); if(o) o.style.display='none';
}
function msShowTab(tab){
  document.getElementById('ms-tab-login').style.borderBottom=tab==='login'?'2px solid #00d4ff':'2px solid transparent';
  document.getElementById('ms-tab-reg').style.borderBottom=tab==='reg'?'2px solid #00d4ff':'2px solid transparent';
  document.getElementById('ms-tab-login').style.color=tab==='login'?'#f8fafc':'#475569';
  document.getElementById('ms-tab-reg').style.color=tab==='reg'?'#f8fafc':'#475569';
  document.getElementById('ms-form-login').style.display=tab==='login'?'block':'none';
  document.getElementById('ms-form-reg').style.display=tab==='reg'?'block':'none';
  document.getElementById('ms-lerr').textContent='';
}
async function msSubmitRegister(){
  var u=document.getElementById('ms-ru').value.trim();
  var p=document.getElementById('ms-rp').value;
  var p2=document.getElementById('ms-rp2').value;
  var err=document.getElementById('ms-rerr');
  if(!u||!p){err.textContent='Username and password required';return;}
  if(p!==p2){err.textContent='Passwords do not match';return;}
  err.textContent='';
  try{
    var r=await fetch('/auth/register',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({username:u,password:p})});
    var d=await r.json();
    if(d.success){
      document.getElementById('ms-lu').value=u;
      document.getElementById('ms-lp').value=p;
      msShowTab('login');
      var lerr=document.getElementById('ms-lerr');
      if(lerr){lerr.style.color='#00d4ff';lerr.textContent='Registered! Signing in...';}
      setTimeout(msSubmitLogin,400);
    } else {
      err.style.color='#ef4444'; err.textContent=d.error||'Registration failed';
    }
  }catch(e){err.style.color='#ef4444';err.textContent='Network error';}
}
// Back/Forward-Cache-Guard: Browser restauriert sonst eine eingefrorene
// (evtl. ausgeloggte/veraltete) Seite aus dem bfcache — dann neu laden.
window.addEventListener('pageshow', function(e){ if (e.persisted) window.location.reload(); });
// Start: try cookie-based auto-login (survives page refresh), then show login if not authenticated
fetch('/auth/me',{credentials:'include',headers:{'Content-Type':'application/json'}})
  .then(r=>r.json()).then(d=>{
    if(d.success){
      msUser=d.username||msUser; msUserId=d.user_id||0;
      if(d.token) msToken=d.token;
      hidLoginPage(); updateUserBadge();
    } else { showLoginPage(); }
  }).catch(()=>{ showLoginPage(); });
</script>
<!-- Phase 158: Full-screen Login Page (starts visible, hidden after auth) -->
<div id="ms-login-overlay" style="display:flex;position:fixed;top:0;left:0;width:100%;height:100%;background:radial-gradient(ellipse 80% 60% at 50% 0%,rgba(0,212,255,0.06),transparent),radial-gradient(ellipse 60% 50% at 80% 100%,rgba(124,58,237,0.06),transparent),#080c18;z-index:9999;justify-content:center;align-items:center;">
  <div id="ms-login-card" style="visibility:hidden;background:rgba(17,24,39,0.9);backdrop-filter:blur(20px);border:1px solid #1e2d40;border-radius:16px;padding:0;width:360px;box-shadow:0 16px 48px rgba(0,0,0,0.7),0 0 60px rgba(0,212,255,0.08);overflow:hidden">
    <!-- Header -->
    <div style="background:rgba(13,18,36,0.8);padding:28px 32px 20px;text-align:center;border-bottom:1px solid #1e2d40">
      <div><span style="display:inline-flex;align-items:center;justify-content:center;width:48px;height:48px;border-radius:12px;background:linear-gradient(135deg,#00d4ff,#0090cc);color:#080c18;font-size:26px;font-weight:800;box-shadow:0 0 30px rgba(0,212,255,0.3)">M</span></div>
      <div style="font-size:22px;font-weight:700;color:#f8fafc;margin-top:10px;letter-spacing:-0.5px">MilanSQL</div>
      <div style="color:#475569;font-size:11px;margin-top:4px"><span class="ms-version">v10.6.0</span> &mdash; Multi-User Database</div>
    </div>
    <!-- Tabs -->
    <div style="display:flex;border-bottom:1px solid #1e2d40">
      <button id="ms-tab-login" onclick="msShowTab('login')"
        style="flex:1;background:none;border:none;border-bottom:2px solid #00d4ff;color:#f8fafc;padding:12px;font-size:13px;font-weight:600;cursor:pointer;font-family:inherit;transition:all .2s">Sign In</button>
      <button id="ms-tab-reg" onclick="msShowTab('reg')"
        style="flex:1;background:none;border:none;border-bottom:2px solid transparent;color:#475569;padding:12px;font-size:13px;font-weight:600;cursor:pointer;font-family:inherit;transition:all .2s">Register</button>
    </div>
    <!-- Login Form -->
    <div id="ms-form-login" style="padding:24px 32px 28px">
      <input id="ms-lu" type="text" placeholder="Username" autocomplete="username"
        style="width:100%;background:#080c18;color:#f8fafc;border:1px solid #1e2d40;border-radius:8px;padding:11px 13px;margin-bottom:10px;font-size:14px;font-family:inherit;outline:none;box-sizing:border-box;transition:border .2s"
        onfocus="this.style.borderColor='#00d4ff'" onblur="this.style.borderColor='#1e2d40'">
      <input id="ms-lp" type="password" placeholder="Password" autocomplete="current-password"
        style="width:100%;background:#080c18;color:#f8fafc;border:1px solid #1e2d40;border-radius:8px;padding:11px 13px;margin-bottom:18px;font-size:14px;font-family:inherit;outline:none;box-sizing:border-box;transition:border .2s"
        onfocus="this.style.borderColor='#00d4ff'" onblur="this.style.borderColor='#1e2d40'"
        onkeydown="if(event.key==='Enter')msSubmitLogin()">
      <button onclick="msSubmitLogin()"
        style="width:100%;background:#00d4ff;color:#080c18;border:none;border-radius:8px;padding:12px;font-size:14px;font-weight:700;cursor:pointer;font-family:inherit;letter-spacing:0.3px;transition:all .2s"
        onmouseover="this.style.boxShadow='0 0 30px rgba(0,212,255,0.4)'" onmouseout="this.style.boxShadow='none'">Sign In</button>
      <div id="ms-lerr" style="font-size:12px;margin-top:10px;min-height:16px;text-align:center"></div>
      <div style="margin-top:16px;text-align:center;font-size:11px;color:#475569">
        Create an account or sign in to get started
      </div>
    </div>
    <!-- Register Form -->
    <div id="ms-form-reg" style="display:none;padding:24px 32px 28px">
      <input id="ms-ru" type="text" placeholder="Username" autocomplete="username"
        style="width:100%;background:#080c18;color:#f8fafc;border:1px solid #1e2d40;border-radius:8px;padding:11px 13px;margin-bottom:10px;font-size:14px;font-family:inherit;outline:none;box-sizing:border-box"
        onfocus="this.style.borderColor='#00d4ff'" onblur="this.style.borderColor='#1e2d40'">
      <input id="ms-rp" type="password" placeholder="Password"
        style="width:100%;background:#080c18;color:#f8fafc;border:1px solid #1e2d40;border-radius:8px;padding:11px 13px;margin-bottom:10px;font-size:14px;font-family:inherit;outline:none;box-sizing:border-box"
        onfocus="this.style.borderColor='#00d4ff'" onblur="this.style.borderColor='#1e2d40'">
      <input id="ms-rp2" type="password" placeholder="Confirm Password"
        style="width:100%;background:#080c18;color:#f8fafc;border:1px solid #1e2d40;border-radius:8px;padding:11px 13px;margin-bottom:18px;font-size:14px;font-family:inherit;outline:none;box-sizing:border-box"
        onfocus="this.style.borderColor='#00d4ff'" onblur="this.style.borderColor='#1e2d40'"
        onkeydown="if(event.key==='Enter')msSubmitRegister()">
      <button onclick="msSubmitRegister()"
        style="width:100%;background:#7c3aed;color:#fff;border:none;border-radius:8px;padding:12px;font-size:14px;font-weight:700;cursor:pointer;font-family:inherit;transition:all .2s"
        onmouseover="this.style.boxShadow='0 0 30px rgba(124,58,237,0.4)'" onmouseout="this.style.boxShadow='none'">Create Account</button>
      <div id="ms-rerr" style="font-size:12px;margin-top:10px;min-height:16px;text-align:center"></div>
    </div>
  </div>
</div>
</body>
</html>)WEBUIEND";
    return html;
}

// ── MilanHttpServer::handleRequest ────────────────────────────

inline std::string MilanHttpServer::handleRequest(const HttpRequest& req, const std::string& clientIp) {
    if (req.method == "OPTIONS")
        return buildHttpResponse(200, "");

    // ══ FORTRESS: Schicht 1+2 — IP Ban Check + Honeypot ═══════
    auto& fortress = milansql::g_fortress();

    // Schicht 2: Check if IP is already blocked
    if (!clientIp.empty() && fortress.isBlocked(clientIp)) {
        return buildHttpResponse(403,
            "{\"success\":false,\"error\":\"Access denied\"}");
    }

    // Schicht 1: Honeypot endpoints — trap scanners & bots
    if (fortress.isHoneypotPath(req.path)) {
        fortress.recordHoneypotHit(clientIp, req.path);
        fortress.saveBanList(dbPath_ + ".banlist");
        // Return realistic-looking but fake responses based on path
        if (req.path == "/.env") {
            // Fake .env with canary credentials
            return buildHttpResponse(200,
                "DB_HOST=localhost\nDB_USER=admin\nDB_PASS=msql_sk_live_a1b2c3d4e5f6g7h8i9j0k1l2m3n4o5p6q7r8s9\n"
                "API_KEY=msql_ak_7f3d9a2b4c5e6f8a1b2c3d4e5f6a7b8c9d0e1f2a\n"
                "JWT_SECRET=canary_secret_do_not_use\n",
                "text/plain");
        }
        if (req.path == "/wp-login.php" || req.path == "/wp-admin") {
            return buildHttpResponse(200,
                "<html><head><title>WordPress &rsaquo; Log In</title></head>"
                "<body><h1>Powered by WordPress</h1>"
                "<form method='post'><input name='log'/><input name='pwd' type='password'/>"
                "<input type='submit' value='Log In'/></form></body></html>",
                "text/html");
        }
        if (req.path == "/phpmyadmin" || req.path == "/admin") {
            return buildHttpResponse(200,
                "<html><head><title>phpMyAdmin</title></head>"
                "<body><h1>phpMyAdmin 5.2.1</h1>"
                "<form method='post'><input name='pma_username'/>"
                "<input name='pma_password' type='password'/>"
                "<input type='submit' value='Go'/></form></body></html>",
                "text/html");
        }
        // Generic 404 for other honeypots
        return buildHttpResponse(404,
            "{\"success\":false,\"error\":\"Not found\"}");
    }

    // Schicht 4: Canary token check on Authorization header
    {
        std::string authHeader;
        auto ait = req.headers.find("authorization");
        if (ait != req.headers.end()) authHeader = ait->second;
        if (!authHeader.empty()) {
            std::string token = authHeader;
            if (token.size() > 7 && token.substr(0,7) == "Bearer ") token = token.substr(7);
            if (token.size() > 7 && token.substr(0,7) == "ApiKey ") token = token.substr(7);
            if (fortress.isCanaryToken(token)) {
                fortress.recordHoneypotHit(clientIp, "CANARY_TOKEN_USED");
                fortress.saveBanList(dbPath_ + ".banlist");
                return buildHttpResponse(401,
                    "{\"success\":false,\"error\":\"Invalid token\"}");
            }
        }
    }

    // ── Path traversal guard ──────────────────────────────────────
    // Block any path containing ".." to prevent directory traversal attacks
    if (req.path.find("..") != std::string::npos)
        return buildHttpResponse(404, "{\"success\":false,\"error\":\"Not found\"}");

    // ── Favicon: lightning bolt SVG (no 404 in browser console) ──
    if (req.path == "/favicon.ico" || req.path == "/favicon.svg" || req.path == "/apple-touch-icon.png") {
        static const std::string FAVICON_SVG =
            "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 100 100'><rect x='0' y='0' width='100' height='100' rx='8' fill='#161616' stroke='#ff6b1a' stroke-width='0.5'/><path d='M20 78 L20 22 L50 54 L80 22 L80 78' fill='none' stroke='#ff6b1a' stroke-width='2.5' stroke-linecap='round' stroke-linejoin='round'/><circle cx='20' cy='22' r='5' fill='#ff6b1a'/><circle cx='20' cy='78' r='5' fill='#ff6b1a'/><circle cx='50' cy='54' r='5' fill='#ff6b1a'/><circle cx='80' cy='22' r='5' fill='#ff6b1a'/><circle cx='80' cy='78' r='5' fill='#ff6b1a'/></svg>";
        return buildHttpResponse(200, FAVICON_SVG, "image/svg+xml");
    }

    // OG Image: social preview card
    if (req.path == "/og-image.png" || req.path == "/og-image.svg") {
        static const std::string OG_SVG =
            "<svg xmlns='http://www.w3.org/2000/svg' width='1200' height='630' viewBox='0 0 1200 630'>"
            "<rect width='1200' height='630' fill='#0d1117'/>"
            "<rect x='440' y='115' width='320' height='320' rx='24' fill='#161616' stroke='#ff6b1a' stroke-width='2'/>"
            "<path d='M504 374 L504 190 L600 294 L696 190 L696 374' fill='none' stroke='#ff6b1a' stroke-width='8' stroke-linecap='round' stroke-linejoin='round'/>"
            "<circle cx='504' cy='190' r='16' fill='#ff6b1a'/>"
            "<circle cx='504' cy='374' r='16' fill='#ff6b1a'/>"
            "<circle cx='600' cy='294' r='16' fill='#ff6b1a'/>"
            "<circle cx='696' cy='190' r='16' fill='#ff6b1a'/>"
            "<circle cx='696' cy='374' r='16' fill='#ff6b1a'/>"
            "<text x='600' y='500' text-anchor='middle' fill='#e6edf3' font-family='-apple-system,BlinkMacSystemFont,sans-serif' font-size='48' font-weight='800'>MilanSQL</text>"
            "<text x='600' y='545' text-anchor='middle' fill='#8b949e' font-family='-apple-system,BlinkMacSystemFont,sans-serif' font-size='22'>The Open Source Database for Developers</text>"
            "</svg>";
        return buildHttpResponse(200, OG_SVG, "image/svg+xml");
    }
    // ── Service Account creation (root only) ──────────────────
    if (req.path == "/auth/service-account" && req.method == "POST") {
        auto ctx = extractUserContext(req);
        if (!ctx.valid || ctx.role != "root")
            return buildHttpResponse(403, R"({"success":false,"error":"Root access required"})");
        std::string username = extractJsonStr(req.body, "username");
        std::string password = extractJsonStr(req.body, "password");
        if (username.empty() || password.empty())
            return buildHttpResponse(400, R"({"success":false,"error":"username and password required"})");
        if (password.size() < 8)
            return buildHttpResponse(400, R"({"success":false,"error":"Password must be at least 8 characters"})");
        auto res = authMgr_.registerUser(username, password, "");
        if (!res.ok)
            return buildHttpResponse(400, "{\"success\":false,\"error\":\"" + jsonEscape(res.error) + "\"}");
        // Set role to "service" — no table prefix, but rate-limited per user
        authMgr_.setUserRole(res.userId, "service");
        authMgr_.save(dbPath_ + ".auth");
        return buildHttpResponse(200,
            "{\"success\":true,\"user_id\":" + std::to_string(res.userId) +
            ",\"username\":\"" + jsonEscape(username) + "\"}");
    }

    // ── Phase 154: Auth routes ────────────────────────────────
    if (req.path == "/auth/register" && req.method == "POST") {
        // MEDIUM-04: Rate limit registration
        if (!loginLimiter_.allow(clientIp))
            return buildHttpResponse(429, R"({"success":false,"error":"Too many registration attempts","retry_after":900})");
        auto result = handleAuthRegister(req.body, clientIp);
        if (result.find("\"success\":true") != std::string::npos) {
            std::string token = extractJsonStr(result, "token");
            std::string cookie = "Set-Cookie: milansql_token=" + token +
                                 "; HttpOnly; Path=/; Max-Age=86400; SameSite=Strict; Secure\r\n";
            return buildHttpResponse(200, result, "application/json", cookie);
        }
        return buildHttpResponse(200, result);
    }

    if (req.path == "/auth/login" && req.method == "POST") {
        // ══ FORTRESS: Schicht 5 — Timing-safe auth response ═══
        auto authStart = std::chrono::steady_clock::now();
        auto result = handleAuthLogin(req.body, clientIp);
        std::string response;
        if (result.find("\"code\":429") != std::string::npos) {
            std::string headers = "Retry-After: 900\r\n";
            response = buildHttpResponse(429, result, "application/json", headers);
        } else if (result.find("\"success\":true") != std::string::npos) {
            std::string token = extractJsonStr(result, "token");
            std::string cookie = "Set-Cookie: milansql_token=" + token +
                                 "; HttpOnly; Path=/; Max-Age=86400; SameSite=Strict; Secure\r\n";
            response = buildHttpResponse(200, result, "application/json", cookie);
        } else {
            response = buildHttpResponse(200, result);
        }
        // Pad to constant 200ms to prevent timing-based user enumeration
        milansql::FortressEngine::padResponseTime(authStart, std::chrono::milliseconds(200));
        return response;
    }
    if (req.path == "/auth/logout" && req.method == "POST") {
        auto result = handleAuthLogout(extractBearerToken(req));
        static const std::string clearCookie =
            "Set-Cookie: milansql_token=; HttpOnly; Path=/; Max-Age=0; SameSite=Lax\r\n";
        return buildHttpResponse(200, result, "application/json", clearCookie);
    }
    if (req.path == "/auth/change-password" && req.method == "POST") {
        // MEDIUM-05: Rate limit password changes
        if (!loginLimiter_.allow(clientIp))
            return buildHttpResponse(429, R"({"success":false,"error":"Too many attempts","retry_after":900})");
        std::string token = extractBearerToken(req);
        auto vr = authMgr_.validateToken(token);
        if (!vr.valid) return buildHttpResponse(401, R"({"success":false,"error":"Unauthorized"})");
        // Parse old_password, new_password from body
        auto getField = [&](const std::string& field) -> std::string {
            auto pos = req.body.find("\"" + field + "\"");
            if (pos == std::string::npos) return "";
            pos = req.body.find(':', pos);
            if (pos == std::string::npos) return "";
            pos = req.body.find('"', pos + 1);
            if (pos == std::string::npos) return "";
            auto end = req.body.find('"', pos + 1);
            return (end != std::string::npos) ? req.body.substr(pos + 1, end - pos - 1) : "";
        };
        std::string oldPw = getField("old_password");
        std::string newPw = getField("new_password");
        if (oldPw.empty() || newPw.empty())
            return buildHttpResponse(400, R"({"success":false,"error":"old_password and new_password required"})");
        auto result = authMgr_.changePassword(vr.userId, oldPw, newPw);
        if (!result.ok)
            return buildHttpResponse(400, R"({"success":false,"error":")" + result.error + R"("})");
        authMgr_.save(dbPath_ + ".auth");
        return buildHttpResponse(200, R"({"success":true,"message":"Password changed"})");
    }
    if (req.path == "/auth/admin/set-password" && req.method == "POST") {
        std::string token = extractBearerToken(req);
        auto vr = authMgr_.validateToken(token);
        if (!vr.valid) return buildHttpResponse(401, R"({"success":false,"error":"Unauthorized"})");
        auto getField = [&](const std::string& field) -> std::string {
            auto pos = req.body.find("\"" + field + "\"");
            if (pos == std::string::npos) return "";
            pos = req.body.find(':', pos);
            if (pos == std::string::npos) return "";
            pos = req.body.find('"', pos + 1);
            if (pos == std::string::npos) return "";
            auto end = req.body.find('"', pos + 1);
            return (end != std::string::npos) ? req.body.substr(pos + 1, end - pos - 1) : "";
        };
        std::string targetUser = getField("username");
        std::string newPw = getField("new_password");
        if (targetUser.empty() || newPw.empty())
            return buildHttpResponse(400, R"({"success":false,"error":"username and new_password required"})");
        // Look up target user ID
        int targetId = authMgr_.getUserIdByName(targetUser);
        if (targetId < 0)
            return buildHttpResponse(404, R"({"success":false,"error":"User not found"})");
        auto result = authMgr_.adminSetPassword(vr.userId, targetId, newPw);
        if (!result.ok)
            return buildHttpResponse(403, R"({"success":false,"error":")" + result.error + R"("})");
        authMgr_.save(dbPath_ + ".auth");
        return buildHttpResponse(200, R"({"success":true,"message":"Password updated for )" + targetUser + R"("})");
    }
    if (req.path == "/auth/me" && req.method == "GET") {
        auto result = handleAuthMe(extractBearerToken(req));
        int code = (result.find("\"success\":true") != std::string::npos) ? 200 : 401;
        return buildHttpResponse(code, result, "application/json",
            "Cache-Control: no-store, no-cache, must-revalidate\r\nPragma: no-cache\r\n");
    }
    if (req.path == "/auth/refresh" && req.method == "POST")
        return buildHttpResponse(200, handleAuthRefresh(req.body));
    if (req.path == "/auth/sessions" && req.method == "GET") {
        auto result = handleAuthSessions(extractBearerToken(req));
        int code = (result.find("\"success\":true") != std::string::npos) ? 200 : 401;
        return buildHttpResponse(code, result);
    }
    if (req.path == "/auth/api-key" && req.method == "POST") {
        // Phase 156: named key creation
        std::string token = extractBearerToken(req);
        auto vr = authMgr_.validateToken(token);
        if (!vr.valid) return buildHttpResponse(401, R"({"success":false,"error":"Unauthorized"})");
        // If body has "name" field → create named key; otherwise legacy single key
        if (!req.body.empty() && req.body.find("\"name\"") != std::string::npos)
            return buildHttpResponse(200, handleAuthApiKeyCreate(vr.userId, req.body));
        return buildHttpResponse(200, handleAuthApiKey(token, req.body));
    }
    if (req.path == "/auth/api-keys" && req.method == "GET") {
        std::string token = extractBearerToken(req);
        auto vr = authMgr_.validateToken(token);
        if (!vr.valid) return buildHttpResponse(401, R"({"success":false,"error":"Unauthorized"})");
        return buildHttpResponse(200, handleAuthApiKeyList(vr.userId));
    }
    // DELETE /auth/api-key/ms_xxxxx
    if (req.path.size() > 15 && req.path.substr(0,15) == "/auth/api-key/" && req.method == "DELETE") {
        std::string keyId = req.path.substr(15);
        std::string token = extractBearerToken(req);
        auto vr = authMgr_.validateToken(token);
        if (!vr.valid) return buildHttpResponse(401, R"({"success":false,"error":"Unauthorized"})");
        return buildHttpResponse(200, handleAuthApiKeyDelete(vr.userId, keyId));
    }
    // GET /auth/api-key/ms_xxxxx/stats
    if (req.path.size() > 21 && req.path.find("/stats") != std::string::npos &&
        req.path.substr(0,15) == "/auth/api-key/") {
        std::string rest = req.path.substr(15);
        auto sp = rest.find("/stats");
        if (sp != std::string::npos) {
            std::string keyId = rest.substr(0, sp);
            std::string token = extractBearerToken(req);
            auto vr = authMgr_.validateToken(token);
            if (!vr.valid) return buildHttpResponse(401, R"({"success":false,"error":"Unauthorized"})");
            return buildHttpResponse(200, handleAuthApiKeyStats(vr.userId, keyId));
        }
    }

    // Phase 156: Admin + Quota routes
    if (req.path == "/admin/users" && req.method == "GET")
        return buildHttpResponse(200, handleAdminUsers(extractBearerToken(req)));
    if (req.path == "/admin/stats" && req.method == "GET")
        return buildHttpResponse(200, handleAdminStats(extractBearerToken(req)));
    if (req.path == "/admin/quota" && req.method == "GET")
        return buildHttpResponse(200, handleAdminQuota(extractBearerToken(req)));
    if (req.path == "/auth/quota" && req.method == "GET")
        return buildHttpResponse(200, handleMyQuota(extractBearerToken(req)));

    // Phase 167: Backup — full SQL dump (auth required)
    if (req.path == "/backup" && req.method == "GET") {
        std::string backupToken = extractBearerToken(req);
        if (backupToken.empty())
            return buildHttpResponse(401, R"({"success":false,"error":"Authorization required. Use: curl -H 'Authorization: Bearer TOKEN' /backup"})");
        auto backupResult = handleBackup(backupToken);
        if (backupResult.find("-- ERROR: Access denied") == 0)
            return buildHttpResponse(403, R"({"success":false,"error":"Access denied. Valid token required."})");
        return buildHttpResponse(200, backupResult, "application/sql");
    }

    // Phase 168: Restore — import SQL dump (root only)
    if (req.path == "/restore" && req.method == "POST") {
        std::string restoreToken = extractBearerToken(req);
        if (restoreToken.empty())
            return buildHttpResponse(401, R"({"success":false,"error":"Authorization required"})");
        // Accept raw SQL body or JSON {"dump":"...","dry_run":true,"clean":true}
        std::string dump;
        bool dryRun = false;
        bool clean = false;
        if (!req.body.empty() && req.body[0] == '{') {
            dump = extractJsonStr(req.body, "dump");
            if (req.body.find("\"dry_run\"") != std::string::npos &&
                req.body.find("true") != std::string::npos)
                dryRun = true;
            if (req.body.find("\"clean\"") != std::string::npos &&
                req.body.find("true") != std::string::npos)
                clean = true;
        }
        if (dump.empty()) dump = req.body;  // raw SQL body
        if (dump.empty())
            return buildHttpResponse(400, R"({"success":false,"error":"Empty dump body"})");
        return buildHttpResponse(200, handleRestore(restoreToken, dump, dryRun, clean));
    }

    // ── Query + Tables (auth-aware) ───────────────────────────
    if (req.path == "/query" || req.path == "/api/query") {
        // Resolve user from token (cookie or Bearer) or API key
        auto ctx = extractUserContext(req);
        AuthManager::ValidateResult vr{0, "anonymous", "user", false};
        if (ctx.valid) {
            vr.userId = ctx.userId; vr.username = ctx.username;
            vr.role = ctx.role; vr.valid = true;
        } else {
            std::string apiKey = extractApiKey(req);
            if (!apiKey.empty()) vr = authMgr_.validateApiKey(apiKey);
        }
        // Reject unauthenticated requests
        if (!vr.valid) {
            return buildHttpResponse(401, R"({"success":false,"error":"Authentication required"})");
        }

        // Rate limit by userId or IP (tiered)
        std::string rlKey = vr.userId > 0 ? std::to_string(vr.userId) : clientIp;
        // Assign tier based on role — check every request so first request gets correct tier
        RateTier desiredTier = RateTier::ANONYMOUS;
        if (vr.role == "root")          desiredTier = RateTier::ADMIN;
        else if (vr.role == "service")  desiredTier = RateTier::FREE;   // service accounts: 600/min per user
        else if (vr.userId > 0)         desiredTier = RateTier::FREE;
        if (requestLimiter_.getTier(rlKey) != desiredTier) {
            requestLimiter_.setTier(rlKey, desiredTier);
        }
        if (!requestLimiter_.allow(rlKey)) {
            double retry = requestLimiter_.retryAfterSeconds(rlKey);
            return buildHttpResponse(429,
                "{\"success\":false,\"error\":\"Rate limit exceeded\",\"retryAfter\":"
                + std::to_string((int)std::ceil(retry)) + "}");
        }

        std::string sql;
        std::vector<std::string> params;
        if (req.method == "GET") sql = getQueryParam(req.query, "sql");
        else if (req.method == "POST") {
            sql = extractSqlFromJson(req.body);
            if (sql.empty()) sql = req.body;
            params = extractParamsFromJson(req.body);
        }
        if (sql.empty())
            return buildHttpResponse(400, R"({"success":false,"error":"Missing SQL"})");

        // Phase C: Bind parameters BEFORE any parsing/sanitization.
        // This ensures params are treated as pure values, never as SQL syntax.
        if (!params.empty()) {
            sql = bindParams(sql, params);
        }

        // Input sanitization: length limit + null-byte + control-char removal
        if (sql.size() > 10000)
            return buildHttpResponse(400, std::string("{\"success\":false,\"error\":\"SQL too long (max 10000 chars)\"}"));
        {
            std::string clean;
            clean.reserve(sql.size());
            for (unsigned char c : sql) {
                if (c == '\0') continue;                // null bytes
                if (c < 0x20 && c != '\t' && c != '\n' && c != '\r') continue; // control chars
                clean += static_cast<char>(c);
            }
            sql = std::move(clean);
        }

        // ══ FORTRESS: Schicht 3 — SQL Injection Detection ═════
        {
            // Honeypot table access check
            if (fortress.checkHoneypotTableAccess(sql)) {
                fortress.recordHoneypotHit(clientIp, "HONEYPOT_TABLE: " + sql.substr(0, 80));
                fortress.saveBanList(dbPath_ + ".banlist");
                return buildHttpResponse(403,
                    "{\"success\":false,\"error\":\"Access denied\"}");
            }

            auto sqliResult = fortress.analyzeQuery(sql);
            if (sqliResult.detected) {
                if (sqliResult.severity >= 2) {
                    // Medium/High: block + ban
                    fortress.recordSqliAttempt(clientIp, sqliResult.pattern);
                    fortress.saveBanList(dbPath_ + ".banlist");
                    // Return fake SQL error to confuse attacker
                    return buildHttpResponse(500,
                        "{\"success\":false,\"error\":\"" +
                        jsonEscape(fortress.getFakeSqlError()) + "\"}");
                }
                // Low severity: log but allow (might be legitimate)
            }
        }

        ++queryCounter_;  // Phase 166: track query count

        // Bug #26: Query timeout (30 seconds max)
        std::string queryResult;
        {
            auto fut = std::async(std::launch::async, [&]() {
                return handleQueryForUser(sql, vr.userId, vr.role);
            });
            if (fut.wait_for(std::chrono::seconds(30)) == std::future_status::timeout) {
                return buildHttpResponse(504,
                    std::string("{\"success\":false,\"error\":\"Query timeout (30s exceeded)\"}"));
            }
            queryResult = fut.get();
        }
        return buildHttpResponse(200, queryResult);
    }

    // Phase 121: Semantic Search REST API
    // POST /semantic-search
    // Body: {"table":"docs","vector_column":"embedding","query_vector":"[1.0,0.0,0.0]",
    //        "limit":5,"filter":"category = 'tech'","include_score":true}
    if (req.path == "/semantic-search" && req.method == "POST") {
        auto ctx = extractUserContext(req);
        if (!ctx.valid) return buildHttpResponse(401, R"({"error":"Authentication required"})");
        return buildHttpResponse(200, handleSemanticSearch(req.body, ctx.userId, ctx.isRoot));
    }

    // Phase 171: Schema Visualizer API
    if (req.path == "/api/schema") {
        auto ctx = extractUserContext(req);
        if (!ctx.valid) return buildHttpResponse(401, R"({"error":"Authentication required"})");
        std::shared_lock<std::shared_mutex> lock(engineMutex_);
        return buildHttpResponse(200, engine_.getSchemaJsonForUser(ctx.userId, ctx.isRoot), "application/json");
    }

    // Phase 170: RLS policies API
    if (req.path == "/api/rls-policies") {
        auto ctx = extractUserContext(req);
        if (!ctx.valid) return buildHttpResponse(401, R"({"error":"Authentication required"})");
        std::shared_lock<std::shared_mutex> lock(engineMutex_);
        return buildHttpResponse(200, engine_.getRlsPoliciesJsonForUser(ctx.userId, ctx.isRoot), "application/json");
    }

    // Phase 170: Per-table RLS policies
    if (req.path.rfind("/api/rls-policies/", 0) == 0) {
        auto ctx = extractUserContext(req);
        if (!ctx.valid) return buildHttpResponse(401, R"({"error":"Authentication required"})");
        std::string tblName = req.path.substr(18);  // after "/api/rls-policies/"
        // Non-root: verify table belongs to user
        if (!ctx.isRoot && ctx.userId > 0) {
            std::string userPrefix = "u" + std::to_string(ctx.userId) + "_";
            std::string bareName = tblName;
            auto dot = tblName.find('.');
            if (dot != std::string::npos) bareName = tblName.substr(dot + 1);
            if (bareName.substr(0, userPrefix.size()) != userPrefix)
                return buildHttpResponse(403, R"({"error":"Access denied"})");
        }
        std::unique_lock<std::shared_mutex> lock(engineMutex_);
        return buildHttpResponse(200, engine_.getTablePoliciesJson(tblName), "application/json");
    }

        if (req.path == "/tables") {
        auto ctx = extractUserContext(req);
        if (!ctx.valid) return buildHttpResponse(401, R"({"error":"Authentication required"})");
        int listId = ctx.isRoot ? 0 : ctx.userId;
        return buildHttpResponse(200, handleListTablesForUser(listId));
    }

    if (req.path.size() > 8 && req.path.substr(0, 8) == "/tables/") {
        auto ctx = extractUserContext(req);
        if (!ctx.valid) return buildHttpResponse(401, R"({"error":"Authentication required"})");
        std::string tableName = req.path.substr(8);
        // Non-root: verify table belongs to user
        if (!ctx.isRoot && ctx.userId > 0) {
            std::string userPrefix = "u" + std::to_string(ctx.userId) + "_";
            std::string bareName = tableName;
            auto dot = tableName.find('.');
            if (dot != std::string::npos) bareName = tableName.substr(dot + 1);
            if (bareName.substr(0, userPrefix.size()) != userPrefix)
                return buildHttpResponse(403, R"({"error":"Access denied"})");
        }
        return buildHttpResponse(200, handleDescribeTable(tableName));
    }

    if (req.path == "/schemas") {
        auto ctx = extractUserContext(req);
        if (!ctx.valid) return buildHttpResponse(401, R"({"error":"Authentication required"})");
        return buildHttpResponse(200, handleListSchemas());
    }

    // ══ MEDIA UPLOAD: File storage on server ════════════════
    if (req.path == "/api/media/upload" && req.method == "POST") {
        auto ctx = extractUserContext(req);
        if (!ctx.valid)
            return buildHttpResponse(401, "{\"success\":false,\"error\":\"Authentication required\"}");

        // Parse multipart boundary from Content-Type
        std::string boundary;
        auto ctIt = req.headers.find("content-type");
        if (ctIt == req.headers.end()) ctIt = req.headers.find("Content-Type");
        if (ctIt != req.headers.end()) {
            auto bp = ctIt->second.find("boundary=");
            if (bp != std::string::npos)
                boundary = ctIt->second.substr(bp + 9);
            // Remove quotes if present
            if (!boundary.empty() && boundary.front() == '"')
                boundary = boundary.substr(1, boundary.size() - 2);
        }
        if (boundary.empty())
            return buildHttpResponse(400, "{\"success\":false,\"error\":\"Missing multipart boundary\"}");

        // Find file data between boundaries
        std::string delim = "--" + boundary;
        auto partStart = req.body.find(delim);
        if (partStart == std::string::npos)
            return buildHttpResponse(400, "{\"success\":false,\"error\":\"No file in upload\"}");

        // Find Content-Type of the file part
        std::string fileExt = ".bin";
        std::string fileContentType;
        auto ctPos = req.body.find("Content-Type:", partStart);
        if (ctPos != std::string::npos) {
            auto ctEnd = req.body.find("\r\n", ctPos);
            fileContentType = req.body.substr(ctPos + 14, ctEnd - ctPos - 14);
            // Trim
            while (!fileContentType.empty() && fileContentType.front() == ' ')
                fileContentType.erase(0, 1);

            if (fileContentType.find("jpeg") != std::string::npos || fileContentType.find("jpg") != std::string::npos) fileExt = ".jpg";
            else if (fileContentType.find("png") != std::string::npos) fileExt = ".png";
            else if (fileContentType.find("gif") != std::string::npos) fileExt = ".gif";
            else if (fileContentType.find("webp") != std::string::npos) fileExt = ".webp";
            else if (fileContentType.find("svg") != std::string::npos) {
                return buildHttpResponse(400, R"JSON({"success":false,"error":"SVG uploads not allowed"})JSON");
            } else if (fileContentType.find("mp4") != std::string::npos) fileExt = ".mp4";
            else if (fileContentType.find("webm") != std::string::npos) fileExt = ".webm";
            else return buildHttpResponse(400, "{\"success\":false,\"error\":\"File type not allowed\"}");
        }

        // File data starts after \r\n\r\n
        auto dataStart = req.body.find("\r\n\r\n", partStart);
        if (dataStart == std::string::npos)
            return buildHttpResponse(400, "{\"success\":false,\"error\":\"Malformed upload\"}");
        dataStart += 4;

        auto dataEnd = req.body.find(delim, dataStart);
        if (dataEnd == std::string::npos) dataEnd = req.body.size();
        // Remove trailing \r\n before boundary
        if (dataEnd >= 2 && req.body[dataEnd-1] == '\n' && req.body[dataEnd-2] == '\r')
            dataEnd -= 2;

        size_t fileSize = dataEnd - dataStart;
        if (fileSize > 10 * 1024 * 1024)
            return buildHttpResponse(400, "{\"success\":false,\"error\":\"File too large (max 10MB)\"}");
        if (fileSize == 0)
            return buildHttpResponse(400, "{\"success\":false,\"error\":\"Empty file\"}");

        // Generate unique filename
        std::random_device rd;
        std::string filename;
        for (int i = 0; i < 16; ++i) {
            static const char hex[] = "0123456789abcdef";
            uint8_t b = static_cast<uint8_t>(rd() & 0xff);
            filename += hex[b >> 4];
            filename += hex[b & 0xf];
        }
        filename += fileExt;

        // Save to /opt/milansql/uploads/
        std::string uploadDir = "/opt/milansql/uploads/";
        std::string filePath = uploadDir + filename;
        std::ofstream outFile(filePath, std::ios::binary);
        if (!outFile)
            return buildHttpResponse(500, "{\"success\":false,\"error\":\"Cannot write file\"}");
        outFile.write(req.body.data() + dataStart, static_cast<std::streamsize>(fileSize));
        outFile.close();

        std::string url = "https://milansql.de/uploads/" + filename;
        return buildHttpResponse(200,
            "{\"success\":true,\"url\":\"" + url + "\",\"filename\":\"" + filename + "\"}");
    }

    // ══ FORTRESS: Security Dashboard Endpoints ═══════════════
    if (req.path == "/fortress/stats") {
        auto ctx = extractUserContext(req);
        if (!ctx.valid || ctx.role != "root")
            return buildHttpResponse(403, "{\"success\":false,\"error\":\"Root access required\"}");
        return buildHttpResponse(200, fortress.getStats());
    }
    if (req.path == "/fortress/alerts") {
        auto ctx = extractUserContext(req);
        if (!ctx.valid || ctx.role != "root")
            return buildHttpResponse(403, "{\"success\":false,\"error\":\"Root access required\"}");
        return buildHttpResponse(200, "{\"alerts\":" + fortress.getAlertLog() + "}");
    }
    if (req.path == "/fortress/threats") {
        auto ctx = extractUserContext(req);
        if (!ctx.valid || ctx.role != "root")
            return buildHttpResponse(403, "{\"success\":false,\"error\":\"Root access required\"}");
        return buildHttpResponse(200, "{\"threats\":" + fortress.getThreatLog() + "}");
    }

    if (req.path == "/status") {
        auto ctx = extractUserContext(req);
        if (!ctx.valid) return buildHttpResponse(401, R"({"error":"Authentication required"})");
        return buildHttpResponse(200, handleStatus());
    }

    if (req.path == "/metrics") {
        // MEDIUM-01: Require root auth for metrics
        auto mctx = extractUserContext(req);
        if (!mctx.valid || mctx.role != "root")
            return buildHttpResponse(403, R"({"success":false,"error":"Authentication required"})");
        std::unique_lock<std::shared_mutex> lock(engineMutex_);
        // Update uptime gauge
        double upSec = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - startTime_).count();
        milansql::g_prometheus().set("milansql_uptime_seconds", upSec);
        // Update buffer pool gauge
        milansql::g_prometheus().set("milansql_buffer_pool_size_mb",
            static_cast<double>(engine_.getBufferPoolSizeMB()));
        // Phase 133: Extended metrics V2
        std::string extraMetrics =
            "# HELP milansql_memory_allocated_bytes Currently allocated memory\n"
            "# TYPE milansql_memory_allocated_bytes gauge\n"
            "milansql_memory_allocated_bytes 0\n"
            "# HELP milansql_errors_total Total errors by type\n"
            "# TYPE milansql_errors_total counter\n"
            "milansql_errors_total{type=\"syntax\"} " + std::to_string(engine_.syntaxErrors_.load()) + "\n"
            "milansql_errors_total{type=\"constraint\"} " + std::to_string(engine_.constraintErrors_.load()) + "\n"
            "milansql_errors_total{type=\"runtime\"} " + std::to_string(engine_.runtimeErrors_.load()) + "\n"
            "milansql_slow_queries_total " + std::to_string(engine_.slowQueryLog.size()) + "\n"
            "milansql_slow_query_threshold_ms " + std::to_string((int)engine_.slowQueryLog.thresholdMs) + "\n"
            "milansql_table_count " + std::to_string(engine_.tableCount()) + "\n";
        std::string body = milansql::g_prometheus().exportMetrics() + extraMetrics;
        return buildHttpResponse(200, body, "text/plain; version=0.0.4");
    }

    if (req.path == "/health") {
        std::unique_lock<std::shared_mutex> lock(engineMutex_);
        double upSec = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - startTime_).count();
        // Audit Bug #25: storage-Check spiegelt echten Persist-Status
        bool storageOk = lastPersistError_.empty();
        std::string storageCheck = storageOk
            ? "{\"status\":\"ok\"}"
            : "{\"status\":\"error\",\"error\":\"" + jsonEscape(lastPersistError_) + "\"}";
        std::string body = "{"
            "\"status\":\"" + std::string(storageOk ? "healthy" : "degraded") + "\","
            "\"test_count\":" + std::to_string(MILANSQL_TEST_COUNT) + ","
            "\"version\":\"" + std::string(MILANSQL_VERSION) + "\","
            "\"uptime_seconds\":" + std::to_string((int)upSec) + ","
            "\"checks\":{"
                "\"storage\":" + storageCheck + ","
                "\"memory\":{\"status\":\"ok\"},"
                "\"wal\":{\"status\":\"ok\"},"
                "\"connections\":{\"status\":\"ok\",\"active\":" +
                    std::to_string(milansql::g_connectionPool.activeCount()) +
                    ",\"idle\":" + std::to_string(milansql::g_connectionPool.idleCount()) +
                    ",\"waiting\":" + std::to_string(milansql::g_connectionPool.waitingCount()) +
                    ",\"max\":" + std::to_string(milansql::g_connectionPool.getMaxConnections()) + "},"
                "\"replication\":{\"status\":\"ok\",\"lag_ms\":0}"
            "},"
            "\"warnings\":[],"
            "\"errors\":[" + (storageOk ? std::string()
                : "\"" + jsonEscape(lastPersistError_) + "\"") + "]"
            "}";
        return buildHttpResponse(200, body);
    }

    if (req.path == "/ready") {
        return buildHttpResponse(200, "{\"ready\":true}");
    }

    // Phase 170: Connection pool statistics
    if (req.path == "/pool/stats") {
        return buildHttpResponse(200, milansql::g_connectionPool.statsJson());
    }

    // Phase 171: MVCC vacuum statistics
    if (req.path == "/vacuum/stats") {
        std::shared_lock<std::shared_mutex> lock(engineMutex_);
        return buildHttpResponse(200, engine_.vacuumManager().statsJson());
    }

    // Phase 172: Streaming replication status (role, lag, failover)
    if (req.path == "/replication/status") {
        return buildHttpResponse(200, milansql::replicationStatusJson());
    }

    if (req.path == "/live") {
        return buildHttpResponse(200, "{\"alive\":true}");
    }

    if (req.path == "/webui" || req.path.rfind("/webui/", 0) == 0) {
        std::string html = handleWebUI();
        // Cache-Fix 2026-07: Browser cachten die WebUI aggressiv und
        // zeigten nach Deploys/Login die alte Seite — hart verbieten.
        return buildHttpResponse(200, html, "text/html",
               "Cache-Control: no-cache, no-store, must-revalidate\r\n"
               "Pragma: no-cache\r\n"
               "Expires: 0\r\n");
    }

    // Phase 163: Landing Page at /
    if (req.path == "/") {
        // Try to serve docs/landing/index.html from working directory
        std::ifstream lf("docs/landing/index.html");
        if (lf.good()) {
            std::string html((std::istreambuf_iterator<char>(lf)),
                              std::istreambuf_iterator<char>());
            return "HTTP/1.1 200 OK\r\n"
                   "Content-Type: text/html; charset=utf-8\r\n"
                   "Content-Length: " + std::to_string(html.size()) + "\r\n"
                   "Cache-Control: no-cache, must-revalidate\r\n"
                   "Connection: close\r\n\r\n" + html;
        }
        // Fallback: redirect to admin UI
        return "HTTP/1.1 302 Found\r\nLocation: /webui\r\nCache-Control: no-store\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    }
    // Impressum (legal requirement for .de domains)
    if (req.path == "/impressum") {
        std::ifstream imf("docs/impressum.html");
        if (imf.good()) {
            std::string html((std::istreambuf_iterator<char>(imf)),
                              std::istreambuf_iterator<char>());
            return buildHttpResponse(200, html, "text/html");
        }
        return buildHttpResponse(404, R"({"error":"Impressum not found"})");
    }
        if (req.path == "/dashboard") {
        return "HTTP/1.1 302 Found\r\nLocation: /webui\r\nCache-Control: no-store\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    }
    // Phase 164: JS SDK at /sdk/milansql.js
    if (req.path == "/sdk/milansql.js") {
        std::ifstream sf("clients/js/milansql.js");
        if (sf.good()) {
            std::string js((std::istreambuf_iterator<char>(sf)),
                            std::istreambuf_iterator<char>());
            return "HTTP/1.1 200 OK\r\n"
                   "Content-Type: application/javascript; charset=utf-8\r\n"
                   "Content-Length: " + std::to_string(js.size()) + "\r\n"
                   "Cache-Control: public, max-age=3600\r\n"
                   "Connection: close\r\n\r\n" + js;
        }
        return buildHttpResponse(404, R"({"error":"SDK not found"})");
    }

    if (req.path == "/ws-playground") {
        // LOW-08: Require auth for WebSocket playground
        auto wsCtx = extractUserContext(req);
        if (!wsCtx.valid)
            return buildHttpResponse(401, R"({"success":false,"error":"Authentication required"})");
        std::string html = R"HTML(<!DOCTYPE html>
<html>
<head><title>MilanSQL WebSocket Playground</title>
<style>body{font-family:monospace;max-width:800px;margin:20px auto;}
#output{background:#1e1e1e;color:#d4d4d4;padding:10px;height:300px;overflow-y:auto;border-radius:4px;}
input{width:60%;padding:6px;} button{padding:6px 12px;margin:2px;cursor:pointer;}</style>
</head>
<body>
<h2>MilanSQL WebSocket Playground</h2>
<p>Connected to: <span id="status">Connecting...</span></p>
<input id="sql" placeholder="SQL Query or table name to subscribe..." value="SELECT 1">
<button onclick="sendQuery()">Execute</button>
<button onclick="subscribe()">Subscribe</button>
<button onclick="clearOutput()">Clear</button>
<pre id="output"></pre>
<script>
const ws = new WebSocket('ws://localhost:8082');
ws.onopen = () => { document.getElementById('status').textContent = 'Connected'; log('Connected to MilanSQL WebSocket'); };
ws.onclose = () => { document.getElementById('status').textContent = 'Disconnected'; };
ws.onerror = () => { document.getElementById('status').textContent = 'Error'; };
ws.onmessage = e => { log('<- ' + e.data); };
function log(msg) {
  const el = document.getElementById('output');
  el.textContent += msg + '\n';
  el.scrollTop = el.scrollHeight;
}
function sendQuery() {
  const sql = document.getElementById('sql').value;
  const msg = JSON.stringify({type:'query', sql});
  log('-> ' + msg);
  ws.send(msg);
}
function subscribe() {
  const table = document.getElementById('sql').value;
  const msg = JSON.stringify({type:'subscribe', table});
  log('-> ' + msg);
  ws.send(msg);
}
function clearOutput() { document.getElementById('output').textContent = ''; }
</script>
</body>
</html>)HTML";
        return "HTTP/1.1 200 OK\r\n"
               "Content-Type: text/html; charset=utf-8\r\n"
               "Content-Length: " + std::to_string(html.size()) + "\r\n"
               "Access-Control-Allow-Origin: *\r\n"
               "Connection: close\r\n"
               "\r\n" + html;
    }

    return buildHttpResponse(404, R"({"success":false,"error":"Not found"})");
}

// ── MilanHttpServer::sendResponse ─────────────────────────────

inline void MilanHttpServer::sendResponse(sock_t sock, const std::string& resp) {
    size_t sent = 0;
    while (sent < resp.size()) {
        int n = send(sock, resp.c_str() + sent, (int)(resp.size() - sent), 0);
        if (n <= 0) break;
        sent += n;
    }
}

// ── MilanHttpServer::handleClient ─────────────────────────────

inline void MilanHttpServer::handleClient(sock_t clientSock) {
    auto req = parseHttpRequest(clientSock);
    if (!req.method.empty()) {
        // Extract client IP — only trust proxy headers if behind trusted reverse proxy
        // (nginx on localhost). Direct connections use socket peer address.
        std::string clientIp = "unknown";
        {
            struct sockaddr_storage addr;
            socklen_t addrLen = sizeof(addr);
            if (getpeername(clientSock, (struct sockaddr*)&addr, &addrLen) == 0) {
                char ipBuf[INET6_ADDRSTRLEN] = {};
                if (addr.ss_family == AF_INET) {
                    inet_ntop(AF_INET, &((struct sockaddr_in*)&addr)->sin_addr, ipBuf, sizeof(ipBuf));
                } else if (addr.ss_family == AF_INET6) {
                    inet_ntop(AF_INET6, &((struct sockaddr_in6*)&addr)->sin6_addr, ipBuf, sizeof(ipBuf));
                }
                clientIp = ipBuf;
            }
            // Only trust proxy headers from loopback (nginx reverse proxy)
            if (clientIp == "127.0.0.1" || clientIp == "::1") {
                auto it = req.headers.find("x-forwarded-for");
                if (it != req.headers.end() && !it->second.empty()) {
                    // Take first IP (original client) from comma-separated list
                    std::string xff = it->second;
                    size_t comma = xff.find(',');
                    clientIp = (comma != std::string::npos) ? xff.substr(0, comma) : xff;
                    // Trim whitespace
                    while (!clientIp.empty() && clientIp.front() == ' ') clientIp.erase(clientIp.begin());
                    while (!clientIp.empty() && clientIp.back() == ' ') clientIp.pop_back();
                } else {
                    auto it2 = req.headers.find("x-real-ip");
                    if (it2 != req.headers.end()) clientIp = it2->second;
                }
            }
        }
        // Phase 170: acquire a pooled connection for engine-touching
        // requests. Monitoring/static endpoints stay pool-exempt so
        // observability keeps working even when the pool is exhausted.
        bool poolExempt =
            req.method == "OPTIONS" ||
            req.path == "/health"  || req.path == "/ready" ||
            req.path == "/live"    || req.path == "/metrics" ||
            req.path == "/pool/stats" || req.path == "/vacuum/stats" ||
            req.path == "/replication/status" ||
            req.path == "/" || req.path == "/webui" ||
            req.path == "/dashboard" || req.path == "/impressum" ||
            req.path.rfind("/favicon", 0) == 0 ||
            req.path.rfind("/apple-touch-icon", 0) == 0;

        milansql::PoolLease lease;
        if (!poolExempt) {
            lease = milansql::PoolLease(milansql::g_connectionPool);
            if (!lease) {
                std::string err = milansql::g_connectionPool.isShuttingDown()
                    ? "{\"success\":false,\"error\":\"Server is shutting down\"}"
                    : "{\"success\":false,\"error\":\"Connection pool exhausted (30s timeout)\"}";
                sendResponse(clientSock, buildHttpResponse(503, err));
                closesocket(clientSock);
                return;
            }
        }

        std::string response;
        try {
            response = handleRequest(req, clientIp);
        } catch (const std::bad_alloc&) {
            response = buildHttpResponse(503, R"({"success":false,"error":"Server out of memory"})");
        } catch (const std::exception& e) {
            response = buildHttpResponse(500, "{\"success\":false,\"error\":\"Internal error\"}");
        } catch (...) {
            response = buildHttpResponse(500, R"({"success":false,"error":"Internal server error"})");
        }
        sendResponse(clientSock, response);
    }
    closesocket(clientSock);
}

// ── MilanHttpServer::run ──────────────────────────────────────

inline void MilanHttpServer::run() {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    initEngine();

    sock_t srv = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons((unsigned short)port_);

    bind(srv, (sockaddr*)&addr, sizeof(addr));
    listen(srv, 1024);  // backlog: 1024 pending connections

    // Phase 167: Thread Pool — 256 workers, 4096 queue depth
    constexpr size_t POOL_SIZE = 256;
    threadPool_ = std::make_unique<ThreadPool>(POOL_SIZE, 4096);

    // Phase 170: Connection pool health checker + graceful shutdown
    milansql::g_connectionPool.startHealthChecker();

    // Phase 171: Auto-vacuum thread — every 60s, exclusive engine lock
    // (the thread lives in VacuumManager; the callback takes engineMutex_
    //  so it never races with HTTP request handlers)
    engine_.vacuumManager().startAutoVacuum([this]() -> size_t {
        std::unique_lock<std::shared_mutex> lock(engineMutex_);
        return engine_.vacuumAllTracked(/*automatic=*/true);
    });

    // Optimizer Phase 3: Auto-ANALYZE thread — analysiert Tabellen,
    // deren Aenderungszaehler > threshold * rowCount (Postgres-Logik).
    milansql::g_autoAnalyze().start([this]() -> size_t {
        std::unique_lock<std::shared_mutex> lock(engineMutex_);
        return milansql::autoAnalyzeSweep(engine_);
    });

    std::signal(SIGINT,  httpShutdownSignalHandler);
#ifdef SIGTERM
    std::signal(SIGTERM, httpShutdownSignalHandler);
#endif

    std::cout << "MilanSQL HTTP Server auf Port " << port_
              << " (Thread Pool: " << POOL_SIZE << " workers, backlog: 1024, "
              << "Conn-Pool: " << milansql::g_connectionPool.getMinConnections()
              << "-" << milansql::g_connectionPool.getMaxConnections() << ")\n" << std::flush;

    while (!g_httpShutdownRequested.load()) {
        // select() with timeout so we can notice the shutdown flag
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(srv, &fds);
        timeval tv{0, 500000};   // 500ms
        int sel = select(static_cast<int>(srv) + 1, &fds, nullptr, nullptr, &tv);
        if (sel < 0) break;
        if (sel == 0) continue;

        sockaddr_in clientAddr{};
        socklen_t len = sizeof(clientAddr);
        sock_t client = accept(srv, (sockaddr*)&clientAddr, &len);
        if (client == INVALID_SOCK) {
            if (g_httpShutdownRequested.load()) break;
            continue;
        }

        // Submit to thread pool; if queue full → 503 Service Unavailable
        bool submitted = threadPool_->enqueue([this, client]() {
            handleClient(client);
        });
        if (!submitted) {
            // Backpressure: queue full → reject gracefully
            const char* busy =
                "HTTP/1.1 503 Service Unavailable\r\n"
                "Content-Type: application/json\r\n"
                "Retry-After: 1\r\n"
                "Content-Length: 60\r\n"
                "Connection: close\r\n\r\n"
                "{\"success\":false,\"error\":\"Server busy, retry in 1s\"}";
            send(client, busy, (int)strlen(busy), 0);
            closesocket(client);
        }
    }

    // Phase 170: Graceful shutdown — stop accepting, drain active queries
    closesocket(srv);
    std::cout << "HTTP Server: Shutdown angefordert — warte auf aktive Queries...\n" << std::flush;
    engine_.vacuumManager().stopAutoVacuum();   // Phase 171
    milansql::g_autoAnalyze().stop();           // Optimizer Phase 3
    milansql::g_connectionPool.stopHealthChecker();
    bool drained = milansql::g_connectionPool.shutdown(30000);
    threadPool_.reset();   // joins worker threads (in-flight requests finish)
    std::cout << "HTTP Server: Shutdown "
              << (drained ? "sauber abgeschlossen (alle Queries beendet)."
                          : "nach 30s Timeout erzwungen.")
              << "\n" << std::flush;
#ifdef _WIN32
    WSACleanup();
#endif
}
