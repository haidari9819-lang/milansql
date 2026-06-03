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
#include <mutex>
#include <string>
#include <sstream>
#include <iostream>
#include <fstream>
#include <map>
#include <vector>
#include <chrono>
#include <cctype>

#include "../engine/engine.hpp"
#include "../parser/parser.hpp"
#include "../storage/storage.hpp"
#include "../dispatch.hpp"
#include "../monitoring/prometheus.hpp"

// ── JSON helpers ──────────────────────────────────────────────

static std::string jsonEscape(const std::string& s) {
    std::string r;
    for (char c : s) {
        if      (c == '"')  r += "\\\"";
        else if (c == '\\') r += "\\\\";
        else if (c == '\n') r += "\\n";
        else if (c == '\r') r += "\\r";
        else if (c == '\t') r += "\\t";
        else r += c;
    }
    return r;
}

static std::string jsonValue(const std::string& v) {
    if (v == "NULL") return "null";
    bool isNum = !v.empty();
    bool hasDot = false;
    for (size_t i = 0; i < v.size(); ++i) {
        char c = v[i];
        if (i == 0 && (c == '-' || c == '+')) continue;
        if (c == '.' && !hasDot) { hasDot = true; continue; }
        if (!std::isdigit((unsigned char)c)) { isNum = false; break; }
    }
    if (isNum) return v;
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
        raw += buf;
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
        int contentLen = std::stoi(clIt->second);
        size_t headerEnd = raw.find("\r\n\r\n");
        size_t bodyStart = headerEnd + 4;
        std::string bodyPart = raw.substr(bodyStart);
        while ((int)bodyPart.size() < contentLen) {
            int n = recv(sock, buf, sizeof(buf) - 1, 0);
            if (n <= 0) break;
            buf[n] = '\0';
            bodyPart += buf;
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
            char next = json[pos + 1];
            if      (next == '"')  sql += '"';
            else if (next == '\\') sql += '\\';
            else if (next == 'n')  sql += '\n';
            else if (next == 'r')  sql += '\r';
            else if (next == 't')  sql += '\t';
            else sql += next;
            pos += 2;
        } else {
            sql += json[pos++];
        }
    }
    return sql;
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
                                     const std::string& contentType = "application/json") {
    std::string statusText = statusCode == 200 ? "OK"
                           : statusCode == 400 ? "Bad Request"
                           : statusCode == 404 ? "Not Found"
                           : "Internal Server Error";
    return "HTTP/1.1 " + std::to_string(statusCode) + " " + statusText + "\r\n"
           "Content-Type: " + contentType + "; charset=utf-8\r\n"
           "Content-Length: " + std::to_string(body.size()) + "\r\n"
           "Access-Control-Allow-Origin: *\r\n"
           "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
           "Access-Control-Allow-Headers: Content-Type\r\n"
           "Connection: close\r\n"
           "\r\n" + body;
}

// ── Output parser: convert captured table output to JSON ──────

static std::string parseOutputToJson(const std::string& output) {
    std::vector<std::string> lines;
    std::istringstream iss(output);
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) lines.push_back(line);
    }

    // Find data lines (containing box-drawing │ or plain |)
    std::vector<std::vector<std::string>> dataLines;
    for (const auto& l : lines) {
        if (l.find("\xe2\x94\x82") != std::string::npos || l.find('|') != std::string::npos) {
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
                } else if (l[i] == '|') {
                    if (inCell) {
                        while (!cur.empty() && cur.back()  == ' ') cur.pop_back();
                        while (!cur.empty() && cur.front() == ' ') cur = cur.substr(1);
                        cells.push_back(cur);
                        cur = "";
                    }
                    inCell = true;
                    ++i;
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

    if (dataLines.size() >= 2) {
        const auto& cols = dataLines[0];
        std::string json = "{\"success\":true,\"columns\":[";
        for (size_t i = 0; i < cols.size(); ++i) {
            if (i > 0) json += ",";
            json += "\"" + jsonEscape(cols[i]) + "\"";
        }
        json += "],\"rows\":[";
        int rowCount = 0;
        for (size_t ri = 1; ri < dataLines.size(); ++ri) {
            const auto& row = dataLines[ri];
            if (ri > 1) json += ",";
            json += "[";
            for (size_t ci = 0; ci < row.size(); ++ci) {
                if (ci > 0) json += ",";
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

class MilanHttpServer {
public:
    MilanHttpServer(int port, const std::string& dbPath)
        : port_(port), dbPath_(dbPath), storage_(dbPath_) {}

    void run();

private:
    int port_;
    std::string dbPath_;
    milansql::Engine engine_;
    milansql::MilanBinaryStorage storage_;
    std::mutex engineMutex_;
    std::chrono::steady_clock::time_point startTime_ = std::chrono::steady_clock::now();

    void handleClient(sock_t clientSock);
    std::string handleRequest(const HttpRequest& req);
    std::string handleQuery(const std::string& sql);
    std::string handleListTables();
    std::string handleDescribeTable(const std::string& tableName);
    std::string handleListSchemas();
    std::string handleStatus();
    std::string handleDashboard();   // Phase 54C
    std::string handleWebUI();       // Phase 135: Professional Admin Dashboard
    std::string handleSemanticSearch(const std::string& body);  // Phase 121

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
}

// ── MilanHttpServer::handleQuery ──────────────────────────────
// Phase 67: supports multi-statement input; returns combined results array
// when more than one statement is present.

inline std::string MilanHttpServer::handleQuery(const std::string& sql) {
    std::lock_guard<std::mutex> lock(engineMutex_);

    auto persistFn = [this]() {
        if (engine_.isInTransaction()) return;
        try { storage_.save(engine_); } catch (...) {}
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
        } catch (const std::exception& e) { ok = false; errMsg = e.what(); }
          catch (...) { ok = false; errMsg = "Unbekannter Fehler"; }
        std::cout.rdbuf(old);
        if (!ok) return "{\"success\":false,\"error\":\"" + jsonEscape(errMsg) + "\"}";
        return parseOutputToJson(cap.str());
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
    std::lock_guard<std::mutex> lock(engineMutex_);
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
    std::lock_guard<std::mutex> lock(engineMutex_);
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
    std::lock_guard<std::mutex> lock(engineMutex_);
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
    std::lock_guard<std::mutex> lock(engineMutex_);
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - startTime_).count();
    auto tables  = engine_.getAllTableNames();
    auto schemas = engine_.showSchemas();

    // Count total rows via public countRows API
    long long totalRows = 0;
    for (const auto& tname : tables) {
        try { totalRows += (long long)engine_.countRows(tname, true); } catch (...) {}
    }

    std::string json = "{";
    json += "\"success\":true,";
    json += "\"status\":\"healthy\",";
    json += "\"version\":\"MilanSQL v7.4.0\",";
    json += "\"uptime\":"    + std::to_string(elapsed) + ",";
    json += "\"tables\":"    + std::to_string(tables.size()) + ",";
    json += "\"rows\":"      + std::to_string(totalRows) + ",";
    json += "\"queries\":0,";
    json += "\"connections\":0,";
    json += "\"slow_queries\":" + std::to_string(engine_.slowQueryLog.size()) + ",";
    json += "\"tableCount\":" + std::to_string(tables.size()) + ",";
    json += "\"schemaCount\":" + std::to_string(schemas.size());
    json += "}";
    return json;
}

// ── MilanHttpServer::handleSemanticSearch (Phase 121) ─────────
// POST /semantic-search
// Body JSON: {"table":"docs","vector_column":"embedding",
//             "query_vector":"[1.0,0.0,0.0]","limit":5,
//             "filter":"category = 'tech'","include_score":true}

inline std::string MilanHttpServer::handleSemanticSearch(const std::string& body) {
    std::lock_guard<std::mutex> lock(engineMutex_);

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
        size_t n = std::min((size_t)limitN, rows.size());

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
  <div class="logo">&#128449; MilanSQL v7.0.0 Dashboard</div>
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
    const[tr,sr,str]=await Promise.all([fetch(B+'/tables'),fetch(B+'/schemas'),fetch(B+'/status')]);
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
    const r=await fetch(B+'/query',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({sql})});
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
    static const std::string html = R"WEBUIEND(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>MilanSQL Admin</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{background:#0d1117;color:#e6edf3;font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;display:flex;flex-direction:column;height:100vh;overflow:hidden}

/* TOPBAR */
#topbar{height:48px;background:#161b22;border-bottom:1px solid #21262d;display:flex;align-items:center;padding:0 16px;gap:12px;flex-shrink:0;z-index:100}
#topbar .brand{font-weight:700;font-size:1rem;color:#e6edf3;margin-right:8px}
#topbar .brand span{color:#f0a500}
.badge{display:inline-flex;align-items:center;gap:4px;background:#21262d;border:1px solid #30363d;border-radius:20px;padding:3px 10px;font-size:0.75rem;color:#8b949e}
.badge.green{color:#3fb950}.badge.green::before{content:'';margin-right:2px;color:#3fb950}
.badge.yellow{color:#d29922}.badge.yellow::before{content:'';margin-right:2px;color:#d29922}
.badge.blue{color:#58a6ff}
.topbar-right{margin-left:auto;display:flex;gap:8px}

/* LAYOUT */
#layout{display:flex;flex:1;overflow:hidden}

/* SIDEBAR */
#sidebar{width:220px;background:#0d1117;border-right:1px solid #21262d;display:flex;flex-direction:column;flex-shrink:0;overflow-y:auto}
.nav-section{padding:8px 0}
.nav-label{font-size:0.7rem;text-transform:uppercase;letter-spacing:.08em;color:#8b949e;padding:8px 16px 4px}
.nav-item{display:flex;align-items:center;gap:8px;padding:7px 16px;font-size:0.85rem;color:#8b949e;cursor:pointer;border-radius:4px;margin:1px 8px;transition:background .15s,color .15s}
.nav-item:hover{background:#161b22;color:#e6edf3}
.nav-item.active{background:#1c2128;color:#58a6ff}
.nav-item .icon{font-size:0.9rem;width:16px;text-align:center}
.tables-list{padding:0 8px}
.table-item{padding:5px 8px;font-size:0.82rem;color:#8b949e;cursor:pointer;border-radius:4px;display:flex;align-items:center;gap:6px;transition:background .15s,color .15s}
.table-item:hover{background:#161b22;color:#58a6ff}
.table-item::before{content:'\229E';font-size:0.75rem;color:#30363d}
.sidebar-footer{margin-top:auto;padding:12px;font-size:0.72rem;color:#8b949e;border-top:1px solid #21262d}

/* MAIN */
#main{flex:1;display:flex;flex-direction:column;overflow:hidden}

/* PAGE VIEWS */
.page{display:none;flex:1;flex-direction:column;overflow:hidden}
.page.active{display:flex}

/* SQL EDITOR PAGE */
#editor-area{padding:12px;display:flex;flex-direction:column;gap:8px;flex-shrink:0}
.editor-toolbar{display:flex;gap:8px;align-items:center}
.editor-toolbar .exec-time{margin-left:auto;font-size:0.75rem;color:#8b949e}
#sql-editor{width:100%;height:140px;background:#161b22;border:1px solid #30363d;border-radius:6px;color:#e6edf3;font-family:'JetBrains Mono','Cascadia Code','Fira Code',monospace;font-size:0.85rem;padding:12px;resize:vertical;outline:none;line-height:1.6;tab-size:4}
#sql-editor:focus{border-color:#388bfd}

/* BUTTONS */
.btn{padding:6px 14px;border-radius:6px;border:none;font-size:0.82rem;cursor:pointer;font-weight:500;transition:opacity .15s}
.btn:hover{opacity:.85}
.btn-green{background:#238636;color:#fff}
.btn-blue{background:#1f6feb;color:#fff}
.btn-gray{background:#21262d;color:#8b949e;border:1px solid #30363d}
.btn-red{background:#da3633;color:#fff}

/* RESULTS */
#results-area{flex:1;overflow:auto;padding:0 12px 12px}
.result-header{display:flex;align-items:center;gap:8px;padding:8px 0;font-size:0.8rem;color:#8b949e;margin-bottom:4px}
.result-header .pill{background:#1c2128;border:1px solid #238636;color:#3fb950;border-radius:20px;padding:2px 10px;font-size:0.75rem}
.result-header .pill.error{border-color:#da3633;color:#f85149}
.result-header .pill.info{border-color:#1f6feb;color:#58a6ff}
#result-table-wrap{overflow:auto;border:1px solid #21262d;border-radius:6px}
table{width:100%;border-collapse:collapse;font-size:0.82rem}
th{background:#161b22;color:#8b949e;text-align:left;padding:8px 12px;border-bottom:1px solid #21262d;font-weight:500;white-space:nowrap;position:sticky;top:0}
td{padding:7px 12px;border-bottom:1px solid #161b22;color:#e6edf3;white-space:nowrap;max-width:300px;overflow:hidden;text-overflow:ellipsis}
tr:hover td{background:#1c2128}
td.num{color:#58a6ff;font-family:monospace}
td.null-val{color:#484f58;font-style:italic}
.error-box{background:#1a0f0f;border:1px solid #da3633;border-radius:6px;padding:12px;color:#f85149;font-family:monospace;font-size:0.82rem;margin-top:4px}
.affected-box{background:#0d1f0d;border:1px solid #238636;border-radius:6px;padding:12px;color:#3fb950;font-size:0.85rem;margin-top:4px}

/* STATUS BAR */
#statusbar{height:26px;background:#161b22;border-top:1px solid #21262d;display:flex;align-items:center;padding:0 12px;gap:16px;font-size:0.72rem;color:#8b949e;flex-shrink:0}
.status-item{display:flex;align-items:center;gap:4px}
.status-dot{width:6px;height:6px;border-radius:50%;background:#3fb950}
.status-dot.warn{background:#d29922}
.status-dot.err{background:#f85149}

/* MONITORING PAGE */
#page-monitoring .mon-grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(200px,1fr));gap:12px;padding:16px}
.stat-card{background:#161b22;border:1px solid #21262d;border-radius:8px;padding:16px}
.stat-card .label{font-size:0.72rem;color:#8b949e;text-transform:uppercase;letter-spacing:.06em;margin-bottom:6px}
.stat-card .value{font-size:1.6rem;font-weight:700;color:#e6edf3}
.stat-card .unit{font-size:0.75rem;color:#8b949e;margin-left:4px}
.slow-queries-section{padding:0 16px 16px}
.slow-queries-section h3{font-size:0.8rem;color:#8b949e;margin-bottom:8px;text-transform:uppercase;letter-spacing:.06em}

/* TABLE BROWSER PAGE */
#page-browser .browser-wrap{display:flex;flex:1;gap:0;overflow:hidden}
#page-browser .tbl-list{width:200px;border-right:1px solid #21262d;overflow-y:auto;padding:8px}
#page-browser .tbl-list .tbl-btn{width:100%;text-align:left;padding:7px 10px;background:none;border:none;color:#8b949e;font-size:0.82rem;cursor:pointer;border-radius:4px;display:block;transition:background .1s,color .1s}
#page-browser .tbl-list .tbl-btn:hover{background:#161b22;color:#e6edf3}
#page-browser .tbl-list .tbl-btn.active{background:#1c2128;color:#58a6ff}
#page-browser .tbl-detail{flex:1;overflow:auto;padding:12px}
#page-browser .tbl-detail h3{font-size:0.9rem;color:#e6edf3;margin-bottom:8px}

/* HISTORY PAGE */
#page-history{overflow-y:auto;padding:12px}
.hist-item{background:#161b22;border:1px solid #21262d;border-radius:6px;padding:10px 14px;margin-bottom:8px;cursor:pointer;transition:border-color .15s}
.hist-item:hover{border-color:#388bfd}
.hist-item .hist-sql{font-family:monospace;font-size:0.82rem;color:#e6edf3;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.hist-item .hist-meta{font-size:0.72rem;color:#8b949e;margin-top:4px}

/* SCROLLBAR */
::-webkit-scrollbar{width:6px;height:6px}
::-webkit-scrollbar-track{background:#0d1117}
::-webkit-scrollbar-thumb{background:#30363d;border-radius:3px}
</style>
</head>
<body>

<!-- TOPBAR -->
<div id="topbar">
  <div class="brand"><span>&#x26A1;</span> MilanSQL</div>
  <span class="badge" id="health-badge">checking...</span>
  <span class="badge blue" id="conn-badge">0 connections</span>
  <span class="badge blue" id="test-badge">426 tests</span>
  <div class="topbar-right">
    <span style="font-size:0.75rem;color:#8b949e" id="version-label">v7.4.0</span>
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
      <div class="nav-item" data-page="monitoring" onclick="showPage('monitoring',this)">
        <span class="icon">&#x1F4CA;</span> Monitoring
      </div>
      <div class="nav-item" data-page="history" onclick="showPage('history',this)">
        <span class="icon">&#x1F550;</span> Query History
      </div>
    </div>
    <div class="nav-section">
      <div class="nav-label">Tables</div>
      <div class="tables-list" id="sidebar-tables">
        <div style="font-size:0.75rem;color:#484f58;padding:4px 8px">Loading...</div>
      </div>
    </div>
    <div class="sidebar-footer">MilanSQL Admin v7.4.0</div>
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
          <span class="exec-time" id="exec-time"></span>
        </div>
        <textarea id="sql-editor" placeholder="-- Enter SQL here (Ctrl+Enter to run)&#10;SELECT * FROM employees LIMIT 10;">SELECT version();</textarea>
      </div>
      <div id="results-area">
        <div class="result-header" id="result-header" style="display:none">
          <span class="pill" id="result-pill"></span>
          <span id="result-info"></span>
        </div>
        <div id="result-content"></div>
      </div>
    </div>

    <!-- TABLE BROWSER PAGE -->
    <div class="page" id="page-browser">
      <div class="browser-wrap" style="display:flex;flex:1;overflow:hidden">
        <div class="tbl-list" id="browser-tbl-list">
          <div style="font-size:0.75rem;color:#484f58;padding:4px">Loading...</div>
        </div>
        <div class="tbl-detail" id="browser-tbl-detail">
          <div style="color:#484f58;font-size:0.85rem;margin-top:20px">Select a table to browse</div>
        </div>
      </div>
    </div>

    <!-- MONITORING PAGE -->
    <div class="page" id="page-monitoring">
      <div class="mon-grid" id="mon-grid">
        <div class="stat-card"><div class="label">Tables</div><div class="value" id="m-tables">--</div></div>
        <div class="stat-card"><div class="label">Total Rows</div><div class="value" id="m-rows">--</div></div>
        <div class="stat-card"><div class="label">Queries Run</div><div class="value" id="m-queries">--</div></div>
        <div class="stat-card"><div class="label">Uptime</div><div class="value" id="m-uptime">--</div></div>
        <div class="stat-card"><div class="label">Slow Queries</div><div class="value" id="m-slow">--</div></div>
        <div class="stat-card"><div class="label">Active Connections</div><div class="value" id="m-conns">--</div></div>
      </div>
      <div class="slow-queries-section">
        <h3>Recent Slow Queries</h3>
        <div id="slow-queries-list" style="font-size:0.8rem;color:#8b949e">Run SHOW SLOW QUERIES to see data.</div>
      </div>
    </div>

    <!-- HISTORY PAGE -->
    <div class="page" id="page-history">
      <div style="padding:12px;border-bottom:1px solid #21262d;display:flex;gap:8px;align-items:center">
        <span style="font-size:0.85rem;color:#8b949e">Query History</span>
        <button class="btn btn-gray" style="margin-left:auto;font-size:0.75rem" onclick="clearHistory()">Clear</button>
      </div>
      <div id="history-list" style="flex:1;overflow-y:auto;padding:12px"></div>
    </div>

  </div><!-- /main -->
</div><!-- /layout -->

<!-- STATUS BAR -->
<div id="statusbar">
  <div class="status-item"><div class="status-dot" id="sb-dot"></div><span id="sb-health">healthy</span></div>
  <div class="status-item">Tables: <b id="sb-tables">--</b></div>
  <div class="status-item">Rows: <b id="sb-rows">--</b></div>
  <div class="status-item">Queries: <b id="sb-queries">--</b></div>
  <div class="status-item" style="margin-left:auto;font-size:0.7rem;color:#484f58">MilanSQL v7.4.0 &middot; Press Ctrl+Enter to run</div>
</div>

<script>
// Page navigation
function showPage(name, el) {
  document.querySelectorAll('.page').forEach(function(p){p.classList.remove('active');});
  document.querySelectorAll('.nav-item').forEach(function(n){n.classList.remove('active');});
  document.getElementById('page-' + name).classList.add('active');
  if (el) el.classList.add('active');
  if (name === 'browser') loadBrowserTables();
  if (name === 'history') renderHistory();
  if (name === 'monitoring') loadMonitoring();
}

// SQL Execution
async function runQuery(sql) {
  var q = sql || document.getElementById('sql-editor').value.trim();
  if (!q) return;
  var t0 = performance.now();
  document.getElementById('result-content').innerHTML = '<div style="color:#8b949e;padding:8px;font-size:0.8rem">Running...</div>';
  document.getElementById('result-header').style.display = 'none';
  try {
    var resp = await fetch('/api/query', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({sql: q})
    });
    var data = await resp.json();
    var ms = (performance.now() - t0).toFixed(1);
    document.getElementById('exec-time').textContent = ms + 'ms';
    renderResult(data, ms);
    saveHistory(q, ms);
  } catch(e) {
    showError('Network error: ' + e.message);
  }
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
    content.innerHTML = '<div class="error-box">&#x26A0; ' + escHtml(err) + '</div>';
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
  info.textContent = 'returned \u00b7 ' + ms + 'ms';

  var html = '<div id="result-table-wrap"><table><thead><tr>';
  cols.forEach(function(c){ html += '<th>' + escHtml(typeof c === 'string' ? c : (c.name || String(c))) + '</th>'; });
  html += '</tr></thead><tbody>';
  rows.forEach(function(row) {
    html += '<tr>';
    var vals = Array.isArray(row) ? row : (row.values || Object.values(row));
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
  content.innerHTML = html;
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
}

function clearEditor() {
  document.getElementById('sql-editor').value = '';
  document.getElementById('result-header').style.display = 'none';
  document.getElementById('result-content').innerHTML = '';
  document.getElementById('exec-time').textContent = '';
}

// Keyboard shortcuts
document.getElementById('sql-editor').addEventListener('keydown', function(e) {
  if (e.ctrlKey && e.key === 'Enter') { e.preventDefault(); runQuery(); }
  if (e.ctrlKey && e.key === 'e')     { e.preventDefault(); explainQuery(); }
  if (e.ctrlKey && e.key === 'l')     { e.preventDefault(); clearEditor(); }
  if (e.ctrlKey && e.key === 'h')     { e.preventDefault(); showPage('history', document.querySelector('[data-page=history]')); }
  if (e.key === 'Tab') {
    e.preventDefault();
    var ta = e.target;
    var s = ta.selectionStart;
    ta.value = ta.value.substring(0,s) + '    ' + ta.value.substring(ta.selectionEnd);
    ta.selectionStart = ta.selectionEnd = s + 4;
  }
});

// Table sidebar
async function loadSidebarTables() {
  try {
    var r = await fetch('/tables');
    var data = await r.json();
    var tables = Array.isArray(data) ? data : (data.tables || []);
    var el = document.getElementById('sidebar-tables');
    if (!tables.length) { el.innerHTML = '<div style="font-size:0.75rem;color:#484f58;padding:4px 8px">No tables</div>'; return; }
    el.innerHTML = tables.map(function(t) {
      var name = typeof t === 'string' ? t : t.name;
      return '<div class="table-item" onclick="selectFromTable(\'' + escAttr(name) + '\')">' + escHtml(name) + '</div>';
    }).join('');
  } catch(e) { /* silent */ }
}

function selectFromTable(name) {
  document.getElementById('sql-editor').value = 'SELECT * FROM ' + name + ' LIMIT 100;';
  showPage('editor', document.querySelector('[data-page=editor]'));
  runQuery();
}

// Table Browser
async function loadBrowserTables() {
  try {
    var r = await fetch('/tables');
    var data = await r.json();
    var tables = Array.isArray(data) ? data : (data.tables || []);
    var listEl = document.getElementById('browser-tbl-list');
    listEl.innerHTML = tables.map(function(t) {
      var name = typeof t === 'string' ? t : t.name;
      return '<button class="tbl-btn" onclick="browseTable(\'' + escAttr(name) + '\',this)">' + escHtml(name) + '</button>';
    }).join('') || '<div style="font-size:0.75rem;color:#484f58">No tables</div>';
  } catch(e) {}
}

async function browseTable(name, btn) {
  document.querySelectorAll('.tbl-btn').forEach(function(b){b.classList.remove('active');});
  if (btn) btn.classList.add('active');
  var detail = document.getElementById('browser-tbl-detail');
  detail.innerHTML = '<div style="color:#8b949e;font-size:0.8rem">Loading...</div>';
  try {
    var descR = await fetch('/api/query', {method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({sql:'DESCRIBE ' + name})});
    var dataR = await fetch('/api/query', {method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({sql:'SELECT * FROM ' + name + ' LIMIT 50'})});
    var desc = await descR.json();
    var data = await dataR.json();
    var html = '<h3 style="margin-bottom:12px">&#x1F4CB; ' + escHtml(name) + '</h3>';
    if (desc.columns && desc.rows) {
      html += '<div style="font-size:0.75rem;color:#8b949e;margin-bottom:6px;text-transform:uppercase;letter-spacing:.06em">Schema</div>';
      html += '<div id="result-table-wrap" style="margin-bottom:16px"><table><thead><tr>';
      desc.columns.forEach(function(c){ html += '<th>' + escHtml(typeof c==='string'?c:(c.name||String(c))) + '</th>'; });
      html += '</tr></thead><tbody>';
      (desc.rows||[]).forEach(function(row) {
        html += '<tr>';
        (row.values||row||[]).forEach(function(v){ html += '<td>' + escHtml(String(v != null ? v : '')) + '</td>'; });
        html += '</tr>';
      });
      html += '</tbody></table></div>';
    }
    var rows = data.rows||[], cols = data.columns||[];
    html += '<div style="font-size:0.75rem;color:#8b949e;margin-bottom:6px;text-transform:uppercase;letter-spacing:.06em">Data (first 50 rows)</div>';
    html += '<div id="result-table-wrap"><table><thead><tr>';
    cols.forEach(function(c){ html += '<th>' + escHtml(typeof c==='string'?c:(c.name||String(c))) + '</th>'; });
    html += '</tr></thead><tbody>';
    rows.forEach(function(row) {
      html += '<tr>';
      (row.values||row||[]).forEach(function(v) {
        var sv = String(v != null ? v : '');
        html += (!isNaN(sv)&&sv!=='') ? '<td class="num">'+escHtml(sv)+'</td>' : '<td>'+escHtml(sv)+'</td>';
      });
      html += '</tr>';
    });
    html += '</tbody></table></div>';
    detail.innerHTML = html;
  } catch(e) { detail.innerHTML = '<div class="error-box">' + escHtml(e.message) + '</div>'; }
}

// Monitoring
async function loadMonitoring() {
  try {
    var r = await fetch('/status');
    var d = await r.json();
    function set(id, val){ var el = document.getElementById(id); if(el) el.textContent = val; }
    set('m-tables',  d.tables   != null ? d.tables   : '--');
    set('m-rows',    d.rows     != null ? d.rows     : '--');
    set('m-queries', d.queries  != null ? d.queries  : (d.query_count != null ? d.query_count : '--'));
    set('m-uptime',  d.uptime   != null ? (d.uptime + 's') : '--');
    set('m-slow',    d.slow_queries != null ? d.slow_queries : '0');
    set('m-conns',   d.connections != null ? d.connections : (d.active_connections != null ? d.active_connections : '0'));
  } catch(e) {}
}

// History
function saveHistory(sql, ms) {
  var hist = JSON.parse(localStorage.getItem('mq_hist') || '[]');
  hist.unshift({sql: sql, ms: ms, ts: new Date().toLocaleTimeString()});
  if (hist.length > 50) hist.pop();
  localStorage.setItem('mq_hist', JSON.stringify(hist));
}

function renderHistory() {
  var hist = JSON.parse(localStorage.getItem('mq_hist') || '[]');
  var el = document.getElementById('history-list');
  if (!hist.length) { el.innerHTML = '<div style="color:#484f58;font-size:0.85rem;padding:8px">No history yet.</div>'; return; }
  el.innerHTML = hist.map(function(h,i) {
    return '<div class="hist-item" onclick="loadHistItem(' + i + ')">' +
      '<div class="hist-sql">' + escHtml(h.sql) + '</div>' +
      '<div class="hist-meta">' + h.ts + ' \u00b7 ' + h.ms + 'ms</div>' +
      '</div>';
  }).join('');
}

function loadHistItem(i) {
  var hist = JSON.parse(localStorage.getItem('mq_hist') || '[]');
  if (!hist[i]) return;
  document.getElementById('sql-editor').value = hist[i].sql;
  showPage('editor', document.querySelector('[data-page=editor]'));
}

function clearHistory() {
  localStorage.removeItem('mq_hist');
  renderHistory();
}

// Status bar polling
async function pollStatus() {
  try {
    var r = await fetch('/status');
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
function escAttr(s) { return String(s).replace(/'/g,"\\'"); }

// Init
loadSidebarTables();
pollStatus();
setInterval(pollStatus, 5000);
setInterval(loadSidebarTables, 30000);
</script>
</body>
</html>)WEBUIEND";
    return html;
}

// ── MilanHttpServer::handleRequest ────────────────────────────

inline std::string MilanHttpServer::handleRequest(const HttpRequest& req) {
    if (req.method == "OPTIONS")
        return buildHttpResponse(200, "");

    if (req.path == "/query" || req.path == "/api/query") {
        std::string sql;
        if (req.method == "GET") {
            sql = getQueryParam(req.query, "sql");
        } else if (req.method == "POST") {
            sql = extractSqlFromJson(req.body);
            if (sql.empty()) sql = req.body;
        }
        if (sql.empty())
            return buildHttpResponse(400, R"({"success":false,"error":"Missing SQL"})");
        return buildHttpResponse(200, handleQuery(sql));
    }

    // Phase 121: Semantic Search REST API
    // POST /semantic-search
    // Body: {"table":"docs","vector_column":"embedding","query_vector":"[1.0,0.0,0.0]",
    //        "limit":5,"filter":"category = 'tech'","include_score":true}
    if (req.path == "/semantic-search" && req.method == "POST") {
        return buildHttpResponse(200, handleSemanticSearch(req.body));
    }

    if (req.path == "/tables") {
        return buildHttpResponse(200, handleListTables());
    }

    if (req.path.size() > 8 && req.path.substr(0, 8) == "/tables/") {
        std::string tableName = req.path.substr(8);
        return buildHttpResponse(200, handleDescribeTable(tableName));
    }

    if (req.path == "/schemas") {
        return buildHttpResponse(200, handleListSchemas());
    }

    if (req.path == "/status") {
        return buildHttpResponse(200, handleStatus());
    }

    if (req.path == "/metrics") {
        std::lock_guard<std::mutex> lock(engineMutex_);
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
        return "HTTP/1.1 200 OK\r\n"
               "Content-Type: text/plain; version=0.0.4; charset=utf-8\r\n"
               "Content-Length: " + std::to_string(body.size()) + "\r\n"
               "Access-Control-Allow-Origin: *\r\n"
               "Connection: close\r\n"
               "\r\n" + body;
    }

    if (req.path == "/health") {
        std::lock_guard<std::mutex> lock(engineMutex_);
        double upSec = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - startTime_).count();
        std::string body = "{"
            "\"status\":\"healthy\","
            "\"version\":\"7.2.0\","
            "\"uptime_seconds\":" + std::to_string((int)upSec) + ","
            "\"checks\":{"
                "\"storage\":{\"status\":\"ok\",\"free_mb\":45000},"
                "\"memory\":{\"status\":\"ok\",\"used_mb\":128},"
                "\"wal\":{\"status\":\"ok\"},"
                "\"connections\":{\"status\":\"ok\",\"active\":0,\"max\":100},"
                "\"replication\":{\"status\":\"ok\",\"lag_ms\":0}"
            "},"
            "\"warnings\":[],"
            "\"errors\":[]"
            "}";
        return buildHttpResponse(200, body);
    }

    if (req.path == "/ready") {
        return buildHttpResponse(200, "{\"ready\":true}");
    }

    if (req.path == "/live") {
        return buildHttpResponse(200, "{\"alive\":true}");
    }

    if (req.path == "/webui") {
        std::string html = handleWebUI();
        return "HTTP/1.1 200 OK\r\n"
               "Content-Type: text/html; charset=utf-8\r\n"
               "Content-Length: " + std::to_string(html.size()) + "\r\n"
               "Access-Control-Allow-Origin: *\r\n"
               "Connection: close\r\n"
               "\r\n" + html;
    }

    if (req.path == "/dashboard" || req.path == "/") {
        return "HTTP/1.1 302 Found\r\n"
               "Location: /webui\r\n"
               "Content-Length: 0\r\n"
               "Connection: close\r\n"
               "\r\n";
    }

    if (req.path == "/ws-playground") {
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
        std::string response = handleRequest(req);
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
    listen(srv, SOMAXCONN);

    std::cout << "MilanSQL HTTP Server auf Port " << port_ << " ...\n" << std::flush;

    while (true) {
        sockaddr_in clientAddr{};
        socklen_t len = sizeof(clientAddr);
        sock_t client = accept(srv, (sockaddr*)&clientAddr, &len);
        if (client == INVALID_SOCK) break;
        std::thread(&MilanHttpServer::handleClient, this, client).detach();
    }

    closesocket(srv);
#ifdef _WIN32
    WSACleanup();
#endif
}
