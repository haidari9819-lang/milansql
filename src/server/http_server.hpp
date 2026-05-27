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

inline std::string MilanHttpServer::handleQuery(const std::string& sql) {
    std::lock_guard<std::mutex> lock(engineMutex_);

    std::ostringstream captured;
    std::streambuf* old = std::cout.rdbuf(captured.rdbuf());

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

    bool ok = true;
    std::string errMsg;

    try {
        milansql::Parser parser;
        auto cmd = parser.parse(sql);
        for (const auto& sq : cmd.subqueries) {
            if (sq.condIdx < cmd.whereConds.size())
                cmd.whereConds[sq.condIdx].inList =
                    engine_.subqueryValues(sq.subTable, sq.subCol,
                                           sq.subWhere, sq.subWhereLogic);
        }
        milansql::dispatchCommand(cmd, engine_, parser, sql,
                                  persistFn, saveProceduresFn, saveTriggFn);
    } catch (const std::exception& e) {
        ok = false;
        errMsg = e.what();
    } catch (...) {
        ok = false;
        errMsg = "Unbekannter Fehler";
    }

    std::cout.rdbuf(old);

    if (!ok) {
        return "{\"success\":false,\"error\":\"" + jsonEscape(errMsg) + "\"}";
    }

    return parseOutputToJson(captured.str());
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

    std::string json = "{\"success\":true,\"status\":{";
    json += "\"version\":\"MilanSQL v1.0.0\",";
    json += "\"uptime\":"    + std::to_string(elapsed) + ",";
    json += "\"tableCount\":" + std::to_string(tables.size()) + ",";
    json += "\"schemaCount\":" + std::to_string(schemas.size());
    json += "}}";
    return json;
}

// ── MilanHttpServer::handleRequest ────────────────────────────

inline std::string MilanHttpServer::handleRequest(const HttpRequest& req) {
    if (req.method == "OPTIONS")
        return buildHttpResponse(200, "");

    if (req.path == "/query") {
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
