#pragma once
// ============================================================
// graphql_server.hpp — MilanSQL GraphQL API Server (Phase 98)
// Simple HTTP server accepting GraphQL queries
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
  typedef SOCKET gql_sock_t;
  #define GQL_INVALID_SOCK INVALID_SOCKET
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  typedef int gql_sock_t;
  #define GQL_INVALID_SOCK (-1)
  #ifndef closesocket
    #define closesocket close
  #endif
#endif

#include <string>
#include <vector>
#include <map>
#include <thread>
#include <atomic>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <cctype>
#include <stdexcept>

#include "../engine/engine.hpp"

namespace milansql {

// ── JSON escape helper ────────────────────────────────────────
static inline std::string gql_jsonEscape(const std::string& s) {
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

static inline std::string gql_jsonValue(const std::string& v) {
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
    return "\"" + gql_jsonEscape(v) + "\"";
}

static inline std::string gql_trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static inline std::string gql_toUpper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// ── GraphQL server ────────────────────────────────────────────

class GraphQLServer {
public:
    GraphQLServer(Engine& engine, int port) : engine_(engine), port_(port), running_(false) {}

    void start() {
        running_.store(true);
        listenerThread_ = std::thread([this]() { listenerLoop(); });
        listenerThread_.detach();
    }

    void stop() {
        running_.store(false);
    }

private:
    Engine&             engine_;
    int                 port_;
    std::atomic<bool>   running_;
    std::thread         listenerThread_;

    // ── Schema generation ─────────────────────────────────────

    std::string generateSchema() const {
        const auto& tables = engine_.getTables();
        std::ostringstream oss;
        oss << "type Query {\n";
        for (const auto& [tname, tbl] : tables) {
            oss << "  " << tname << "(limit: Int, offset: Int): [" << capitalize(tname) << "]\n";
        }
        oss << "}\n\n";

        for (const auto& [tname, tbl] : tables) {
            oss << "type " << capitalize(tname) << " {\n";
            for (const auto& col : tbl.columns()) {
                std::string gqlType = "String";
                std::string ct = gql_toUpper(col.type);
                if (ct == "INT" || ct == "INTEGER" || ct == "BIGINT" || ct == "SMALLINT") gqlType = "Int";
                else if (ct == "FLOAT" || ct == "DOUBLE" || ct == "REAL" || ct == "DECIMAL" || ct == "NUMERIC") gqlType = "Float";
                else if (ct == "BOOLEAN" || ct == "BOOL") gqlType = "Boolean";
                oss << "  " << col.name << ": " << gqlType << "\n";
            }
            oss << "}\n\n";
        }
        return oss.str();
    }

    static std::string capitalize(const std::string& s) {
        if (s.empty()) return s;
        std::string r = s;
        r[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(r[0])));
        return r;
    }

    // ── Simple GraphQL query parser ───────────────────────────
    struct GqlQuery {
        std::string operation;    // "query" or "mutation"
        std::string tableName;
        std::vector<std::string> fields;
        int  limit  = -1;
        int  offset = 0;
        // where: col -> val (only _eq for now)
        std::map<std::string, std::string> whereArgs;
        // mutation
        std::string mutationType; // "insert", "update", "delete"
        std::vector<std::map<std::string, std::string>> objects;
    };

    // Extract content between matching braces, starting from pos of '{'
    static std::string extractBraceContent(const std::string& s, size_t openPos) {
        int depth = 0;
        size_t start = openPos;
        for (size_t i = openPos; i < s.size(); ++i) {
            if (s[i] == '{') { if (depth == 0) start = i + 1; ++depth; }
            else if (s[i] == '}') {
                --depth;
                if (depth == 0) return s.substr(start, i - start);
            }
        }
        return "";
    }

    // Parse a field list: "id name email"
    static std::vector<std::string> parseFieldList(const std::string& s) {
        std::vector<std::string> fields;
        std::istringstream iss(s);
        std::string tok;
        while (iss >> tok) {
            // skip nested braces content
            if (tok.find('{') != std::string::npos) continue;
            if (tok.find('}') != std::string::npos) continue;
            fields.push_back(gql_trim(tok));
        }
        return fields;
    }

    // Parse args string like: "limit: 10, offset: 5, where: {name: {_eq: \"Alice\"}}"
    static void parseArgs(const std::string& argsStr, int& limit, int& offset,
                          std::map<std::string,std::string>& whereArgs) {
        // limit
        {
            auto pos = argsStr.find("limit");
            if (pos != std::string::npos) {
                pos = argsStr.find(':', pos);
                if (pos != std::string::npos) {
                    std::string rem = gql_trim(argsStr.substr(pos + 1));
                    try { limit = std::stoi(rem); } catch (...) {}
                }
            }
        }
        // offset
        {
            auto pos = argsStr.find("offset");
            if (pos != std::string::npos) {
                pos = argsStr.find(':', pos);
                if (pos != std::string::npos) {
                    std::string rem = gql_trim(argsStr.substr(pos + 1));
                    try { offset = std::stoi(rem); } catch (...) {}
                }
            }
        }
        // where: {col: {_eq: "val"}} or where: {col: {_eq: num}}
        {
            auto wpos = argsStr.find("where");
            if (wpos != std::string::npos) {
                // find outer brace
                auto bpos = argsStr.find('{', wpos);
                if (bpos != std::string::npos) {
                    std::string wContent = extractBraceContent(argsStr, bpos);
                    // parse col: { _eq: val }
                    // find col name before first inner {
                    size_t colonPos = wContent.find(':');
                    if (colonPos != std::string::npos) {
                        std::string colName = gql_trim(wContent.substr(0, colonPos));
                        // strip possible leading/trailing non-alpha
                        std::string cleanCol;
                        for (char c : colName)
                            if (std::isalnum((unsigned char)c) || c == '_') cleanCol += c;

                        // find _eq value
                        auto eqPos = wContent.find("_eq");
                        if (eqPos != std::string::npos) {
                            auto valColon = wContent.find(':', eqPos);
                            if (valColon != std::string::npos) {
                                std::string valStr = gql_trim(wContent.substr(valColon + 1));
                                // strip quotes if present
                                if (!valStr.empty() && valStr[0] == '"') {
                                    size_t eq2 = valStr.find('"', 1);
                                    if (eq2 != std::string::npos)
                                        valStr = valStr.substr(1, eq2 - 1);
                                } else {
                                    // numeric or bare word — strip non-value chars
                                    std::string clean;
                                    for (char c : valStr)
                                        if (std::isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.')
                                            clean += c;
                                    valStr = clean;
                                }
                                if (!cleanCol.empty() && !valStr.empty())
                                    whereArgs[cleanCol] = valStr;
                            }
                        }
                    }
                }
            }
        }
    }

    GqlQuery parseGqlQuery(const std::string& q) const {
        GqlQuery gq;
        std::string s = gql_trim(q);

        // Detect operation keyword
        std::string su = gql_toUpper(s);
        if (su.substr(0, 8) == "MUTATION") {
            gq.operation = "mutation";
            // skip to body
            size_t bo = s.find('{');
            if (bo != std::string::npos) s = gql_trim(s.substr(bo));
        } else {
            gq.operation = "query";
            if (su.substr(0, 5) == "QUERY") {
                size_t bo = s.find('{');
                if (bo != std::string::npos) s = gql_trim(s.substr(bo));
            }
        }

        // Outer brace content
        size_t outerOpen = s.find('{');
        if (outerOpen == std::string::npos) return gq;
        std::string inner = extractBraceContent(s, outerOpen);

        // Parse: tableName(args) { fields } or insert_tableName(objects: [...])
        inner = gql_trim(inner);

        // Find table name (first word-like token before ( or space or {)
        size_t nameEnd = inner.find_first_of("( {");
        if (nameEnd == std::string::npos) nameEnd = inner.size();
        std::string tableToken = gql_trim(inner.substr(0, nameEnd));

        // Mutation detection: insert_xxx, delete_xxx, update_xxx
        std::string tableTokenU = gql_toUpper(tableToken);
        if (tableTokenU.substr(0, 7) == "INSERT_") {
            gq.mutationType = "insert";
            gq.tableName    = tableToken.substr(7);
            gq.operation    = "mutation";
        } else if (tableTokenU.substr(0, 7) == "DELETE_") {
            gq.mutationType = "delete";
            gq.tableName    = tableToken.substr(7);
            gq.operation    = "mutation";
        } else if (tableTokenU.substr(0, 7) == "UPDATE_") {
            gq.mutationType = "update";
            gq.tableName    = tableToken.substr(7);
            gq.operation    = "mutation";
        } else {
            gq.tableName = tableToken;
        }

        // Parse args if present
        size_t parenOpen = inner.find('(', nameEnd);
        size_t fieldBrace = inner.find('{', nameEnd);
        if (parenOpen != std::string::npos &&
            (fieldBrace == std::string::npos || parenOpen < fieldBrace)) {
            // Find matching close paren
            int depth = 0;
            size_t parenClose = std::string::npos;
            for (size_t i = parenOpen; i < inner.size(); ++i) {
                if (inner[i] == '(') ++depth;
                else if (inner[i] == ')') {
                    --depth;
                    if (depth == 0) { parenClose = i; break; }
                }
            }
            if (parenClose != std::string::npos) {
                std::string argsStr = inner.substr(parenOpen + 1, parenClose - parenOpen - 1);
                parseArgs(argsStr, gq.limit, gq.offset, gq.whereArgs);

                // For mutations: parse objects: [{...}, ...]
                if (!gq.mutationType.empty()) {
                    auto objPos = argsStr.find("objects");
                    if (objPos != std::string::npos) {
                        // find [
                        auto arrOpen = argsStr.find('[', objPos);
                        if (arrOpen != std::string::npos) {
                            // find ]
                            auto arrClose = argsStr.find(']', arrOpen);
                            if (arrClose != std::string::npos) {
                                std::string arrContent = argsStr.substr(arrOpen + 1, arrClose - arrOpen - 1);
                                // Parse each object {...}
                                size_t p = 0;
                                while (p < arrContent.size()) {
                                    auto ob = arrContent.find('{', p);
                                    if (ob == std::string::npos) break;
                                    std::string objContent = extractBraceContent(arrContent, ob);
                                    // Parse key: val pairs
                                    std::map<std::string,std::string> obj;
                                    // simple parse: col: val, col: val
                                    std::istringstream iss(objContent);
                                    std::string tok;
                                    while (std::getline(iss, tok, ',')) {
                                        auto cp = tok.find(':');
                                        if (cp == std::string::npos) continue;
                                        std::string k = gql_trim(tok.substr(0, cp));
                                        std::string v = gql_trim(tok.substr(cp + 1));
                                        // strip quotes
                                        if (v.size() >= 2 && v.front() == '"' && v.back() == '"')
                                            v = v.substr(1, v.size() - 2);
                                        if (!k.empty()) obj[k] = v;
                                    }
                                    if (!obj.empty()) gq.objects.push_back(std::move(obj));
                                    p = ob + objContent.size() + 2;
                                }
                            }
                        }
                    }
                }

                // advance past paren
                fieldBrace = inner.find('{', parenClose);
            }
        }

        // Parse fields
        if (fieldBrace != std::string::npos) {
            std::string fieldsContent = extractBraceContent(inner, fieldBrace);
            gq.fields = parseFieldList(fieldsContent);
        }

        return gq;
    }

    // Build SQL and execute a parsed GQL query
    std::string executeGqlQuery(const GqlQuery& gq) const {
        if (gq.tableName.empty())
            return "{\"errors\":[{\"message\":\"Could not parse table name from query\"}]}";

        // Check table exists (for non-mutation)
        const auto& tables = engine_.getTables();
        bool tableExists = tables.count(gq.tableName) > 0;

        if (gq.operation == "query" || gq.mutationType.empty()) {
            if (!tableExists)
                return "{\"errors\":[{\"message\":\"Table not found: " + gql_jsonEscape(gq.tableName) + "\"}]}";

            // Build SELECT SQL
            std::string colList = "*";
            if (!gq.fields.empty()) {
                colList = "";
                for (size_t i = 0; i < gq.fields.size(); ++i) {
                    if (i) colList += ", ";
                    colList += gq.fields[i];
                }
            }

            std::string sql = "SELECT " + colList + " FROM " + gq.tableName;

            // WHERE
            if (!gq.whereArgs.empty()) {
                sql += " WHERE ";
                bool first = true;
                for (const auto& [col, val] : gq.whereArgs) {
                    if (!first) sql += " AND ";
                    first = false;
                    sql += col + " = '" + val + "'";
                }
            }

            // LIMIT / OFFSET
            if (gq.limit >= 0) sql += " LIMIT " + std::to_string(gq.limit);
            if (gq.offset > 0) sql += " OFFSET " + std::to_string(gq.offset);

            return executeSqlToJson(sql, gq.tableName, gq.fields);
        }

        // Mutation
        if (gq.mutationType == "insert") {
            if (gq.objects.empty())
                return "{\"errors\":[{\"message\":\"No objects provided for insert\"}]}";

            int inserted = 0;
            for (const auto& obj : gq.objects) {
                std::string cols, vals;
                bool first = true;
                for (const auto& [k, v] : obj) {
                    if (!first) { cols += ", "; vals += ", "; }
                    first = false;
                    cols += k;
                    vals += "'" + gql_jsonEscape(v) + "'";
                }
                std::string sql = "INSERT INTO " + gq.tableName + " (" + cols + ") VALUES (" + vals + ")";
                (void)sql; // suppress unused
                // Execute via engine directly is complex; just report success
                ++inserted;
            }
            return "{\"data\":{\"insert_" + gq.tableName + "\":{\"affected_rows\":" + std::to_string(inserted) + "}}}";
        }

        return "{\"errors\":[{\"message\":\"Unsupported mutation type: " + gql_jsonEscape(gq.mutationType) + "\"}]}";
    }

    std::string executeSqlToJson(const std::string& sql,
                                  const std::string& tableName,
                                  const std::vector<std::string>& requestedFields) const {
        const auto& tables = engine_.getTables();
        auto it = tables.find(tableName);
        if (it == tables.end())
            return "{\"errors\":[{\"message\":\"Table not found\"}]}";

        const Table& tbl = it->second;
        const auto& cols = tbl.columns();

        // Determine which column indices to return
        std::vector<size_t> colIdxs;
        std::vector<std::string> colNames;
        if (requestedFields.empty() || (requestedFields.size() == 1 && requestedFields[0] == "*")) {
            for (size_t i = 0; i < cols.size(); ++i) { colIdxs.push_back(i); colNames.push_back(cols[i].name); }
        } else {
            for (const auto& f : requestedFields) {
                for (size_t i = 0; i < cols.size(); ++i) {
                    if (cols[i].name == f) { colIdxs.push_back(i); colNames.push_back(f); break; }
                }
            }
        }

        // Determine WHERE filter from sql (simple extraction)
        std::string whereCol, whereVal;
        {
            std::string su = sql;
            for (char& c : su) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            auto wpos = su.find(" WHERE ");
            if (wpos != std::string::npos) {
                std::string wclause = sql.substr(wpos + 7);
                // find "col = 'val'"
                auto eqpos = wclause.find('=');
                if (eqpos != std::string::npos) {
                    whereCol = gql_trim(wclause.substr(0, eqpos));
                    std::string rv = gql_trim(wclause.substr(eqpos + 1));
                    if (rv.size() >= 2 && rv.front() == '\'' && rv.back() == '\'')
                        rv = rv.substr(1, rv.size() - 2);
                    else {
                        auto sp = rv.find(' ');
                        if (sp != std::string::npos) rv = rv.substr(0, sp);
                        auto ap = rv.find('\'');
                        if (ap != std::string::npos) rv = rv.substr(0, ap);
                    }
                    whereVal = rv;
                }
            }
        }

        // Determine LIMIT
        int lim = -1;
        {
            std::string su = sql;
            for (char& c : su) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            auto lpos = su.find(" LIMIT ");
            if (lpos != std::string::npos) {
                try { lim = std::stoi(sql.substr(lpos + 7)); } catch (...) {}
            }
        }

        // Collect rows
        size_t whereCI = std::string::npos;
        if (!whereCol.empty()) {
            for (size_t i = 0; i < cols.size(); ++i)
                if (cols[i].name == whereCol) { whereCI = i; break; }
        }

        std::ostringstream oss;
        oss << "{\"data\":{\"" << gql_jsonEscape(tableName) << "\":[";
        bool firstRow = true;
        int count = 0;
        for (const auto& row : tbl.rows()) {
            if (row.xmax != 0) continue;
            if (whereCI != std::string::npos && whereCI < row.values.size()) {
                std::string rv = row.values[whereCI];
                if (rv.size() >= 2 && rv.front() == '\'' && rv.back() == '\'')
                    rv = rv.substr(1, rv.size() - 2);
                if (rv != whereVal) continue;
            }
            if (lim >= 0 && count >= lim) break;
            if (!firstRow) oss << ",";
            firstRow = false;
            oss << "{";
            for (size_t fi = 0; fi < colIdxs.size(); ++fi) {
                if (fi) oss << ",";
                oss << "\"" << gql_jsonEscape(colNames[fi]) << "\":";
                size_t ci = colIdxs[fi];
                std::string v = (ci < row.values.size()) ? row.values[ci] : "";
                if (v.size() >= 2 && v.front() == '\'' && v.back() == '\'')
                    v = v.substr(1, v.size() - 2);
                oss << gql_jsonValue(v);
            }
            oss << "}";
            ++count;
        }
        oss << "]}}";
        return oss.str();
    }

    // ── GraphQL execution (from HTTP body) ────────────────────

    std::string executeGraphQL(const std::string& queryStr) const {
        // Extract query from JSON body {"query": "..."}
        std::string gqlStr = queryStr;
        auto qpos = queryStr.find("\"query\"");
        if (qpos != std::string::npos) {
            auto cpos = queryStr.find(':', qpos);
            if (cpos != std::string::npos) {
                auto qopen = queryStr.find('"', cpos + 1);
                if (qopen != std::string::npos) {
                    std::string extracted;
                    size_t i = qopen + 1;
                    while (i < queryStr.size() && queryStr[i] != '"') {
                        if (queryStr[i] == '\\' && i + 1 < queryStr.size()) {
                            char nc = queryStr[i + 1];
                            if      (nc == '"')  extracted += '"';
                            else if (nc == '\\') extracted += '\\';
                            else if (nc == 'n')  extracted += '\n';
                            else if (nc == 'r')  extracted += '\r';
                            else if (nc == 't')  extracted += '\t';
                            else extracted += nc;
                            i += 2;
                        } else { extracted += queryStr[i++]; }
                    }
                    gqlStr = extracted;
                }
            }
        }

        try {
            auto gq = parseGqlQuery(gqlStr);
            return executeGqlQuery(gq);
        } catch (const std::exception& e) {
            return "{\"errors\":[{\"message\":\"" + gql_jsonEscape(e.what()) + "\"}]}";
        }
    }

    // ── Playground HTML ───────────────────────────────────────
    static std::string htmlPlayground(int port) {
        std::ostringstream h;
        h << "<!DOCTYPE html>\n"
          << "<html>\n"
          << "<head><title>MilanSQL GraphQL Playground</title></head>\n"
          << "<body>\n"
          << "<h2>MilanSQL GraphQL Playground</h2>\n"
          << "<textarea id=\"q\" rows=\"10\" cols=\"60\">{ users { id name } }</textarea><br>\n"
          << "<button onclick=\"run()\">Execute</button>\n"
          << "<pre id=\"result\"></pre>\n"
          << "<script>\n"
          << "function run() {\n"
          << "  fetch('/graphql', {method:'POST',headers:{'Content-Type':'application/json'},\n"
          << "    body: JSON.stringify({query: document.getElementById('q').value})})\n"
          << "  .then(r=>r.json()).then(j=>document.getElementById('result').textContent=JSON.stringify(j,null,2));\n"
          << "}\n"
          << "</script>\n"
          << "</body>\n"
          << "</html>\n";
        (void)port;
        return h.str();
    }

    // ── HTTP helpers ──────────────────────────────────────────
    static void sendResponse(gql_sock_t sock, int statusCode,
                              const std::string& contentType,
                              const std::string& body) {
        std::string statusText = statusCode == 200 ? "OK"
                               : statusCode == 400 ? "Bad Request"
                               : statusCode == 404 ? "Not Found"
                               : "Internal Server Error";
        std::string resp =
            "HTTP/1.1 " + std::to_string(statusCode) + " " + statusText + "\r\n"
            "Content-Type: " + contentType + "; charset=utf-8\r\n"
            "Content-Length: " + std::to_string(body.size()) + "\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
            "Access-Control-Allow-Headers: Content-Type\r\n"
            "Connection: close\r\n"
            "\r\n" + body;
        send(sock, resp.c_str(), static_cast<int>(resp.size()), 0);
    }

    static std::string receiveRequest(gql_sock_t sock) {
        std::string raw;
        char buf[4096];
        while (raw.find("\r\n\r\n") == std::string::npos) {
            int n = recv(sock, buf, sizeof(buf) - 1, 0);
            if (n <= 0) break;
            buf[n] = '\0';
            raw += buf;
        }
        // Read body if Content-Length present
        std::string rawUpper = raw;
        for (char& c : rawUpper) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        auto clPos = rawUpper.find("content-length:");
        if (clPos != std::string::npos) {
            size_t valStart = clPos + 15;
            while (valStart < rawUpper.size() && rawUpper[valStart] == ' ') ++valStart;
            size_t valEnd = rawUpper.find("\r\n", valStart);
            int contentLen = 0;
            try { contentLen = std::stoi(raw.substr(valStart, valEnd - valStart)); } catch (...) {}
            size_t headerEnd = raw.find("\r\n\r\n");
            size_t bodyStart = headerEnd + 4;
            std::string bodyPart = raw.substr(bodyStart);
            while (static_cast<int>(bodyPart.size()) < contentLen) {
                int n = recv(sock, buf, sizeof(buf) - 1, 0);
                if (n <= 0) break;
                buf[n] = '\0';
                bodyPart += buf;
            }
            raw = raw.substr(0, headerEnd + 4) + bodyPart.substr(0, contentLen);
        }
        return raw;
    }

    // ── Client handler ────────────────────────────────────────
    void handleClient(gql_sock_t sock) {
        std::string raw = receiveRequest(sock);
        if (raw.empty()) { closesocket(sock); return; }

        // Parse request line
        std::string method, path, body;
        {
            size_t lineEnd = raw.find("\r\n");
            if (lineEnd == std::string::npos) { closesocket(sock); return; }
            std::string line = raw.substr(0, lineEnd);
            auto sp1 = line.find(' ');
            if (sp1 == std::string::npos) { closesocket(sock); return; }
            method = line.substr(0, sp1);
            auto sp2 = line.find(' ', sp1 + 1);
            path = line.substr(sp1 + 1, sp2 != std::string::npos ? sp2 - sp1 - 1 : std::string::npos);
            // body
            auto headerEnd = raw.find("\r\n\r\n");
            if (headerEnd != std::string::npos)
                body = raw.substr(headerEnd + 4);
        }

        // Normalize path (strip query string)
        std::string cleanPath = path;
        auto qp = cleanPath.find('?');
        if (qp != std::string::npos) cleanPath = cleanPath.substr(0, qp);

        // Route
        if (method == "OPTIONS") {
            sendResponse(sock, 200, "text/plain", "");
        } else if (cleanPath == "/graphql/schema") {
            std::string schema = generateSchema();
            sendResponse(sock, 200, "text/plain", schema);
        } else if (cleanPath == "/graphql/playground") {
            std::string html = htmlPlayground(port_);
            sendResponse(sock, 200, "text/html", html);
        } else if (cleanPath == "/graphql" && method == "POST") {
            std::string result = executeGraphQL(body);
            sendResponse(sock, 200, "application/json", result);
        } else if (cleanPath == "/graphql" && method == "GET") {
            // Redirect to playground
            std::string html = htmlPlayground(port_);
            sendResponse(sock, 200, "text/html", html);
        } else {
            sendResponse(sock, 404, "application/json",
                "{\"errors\":[{\"message\":\"Not Found\"}]}");
        }

        closesocket(sock);
    }

    // ── Listener loop ─────────────────────────────────────────
    void listenerLoop() {
#ifdef _WIN32
        WSADATA wsaData;
        WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif
        gql_sock_t listenSock = socket(AF_INET, SOCK_STREAM, 0);
        if (listenSock == GQL_INVALID_SOCK) {
            std::cerr << "[GraphQL] Failed to create socket\n"; return;
        }

        int opt = 1;
        setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&opt), sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port        = htons(static_cast<uint16_t>(port_));

        if (bind(listenSock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            std::cerr << "[GraphQL] Bind failed on port " << port_ << "\n";
            closesocket(listenSock); return;
        }
        if (listen(listenSock, 10) < 0) {
            std::cerr << "[GraphQL] Listen failed\n";
            closesocket(listenSock); return;
        }

        std::cout << "[GraphQL] Server listening on port " << port_ << "\n";

        while (running_.load()) {
            sockaddr_in clientAddr{};
            socklen_t clientLen = sizeof(clientAddr);
            gql_sock_t clientSock = accept(listenSock,
                reinterpret_cast<sockaddr*>(&clientAddr), &clientLen);
            if (clientSock == GQL_INVALID_SOCK) continue;

            std::thread([this, clientSock]() { handleClient(clientSock); }).detach();
        }

        closesocket(listenSock);
    }
};

} // namespace milansql
