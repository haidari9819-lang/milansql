#pragma once
// ============================================================
// pg_server.hpp — Phase 91: PostgreSQL Wire Protocol v3 Server
//
// Allows psql, libpq, psycopg2, etc. to connect to MilanSQL.
//
//   build/milansql.exe --pg --pg-port 5433
//   psql -h localhost -p 5433 -U root -d public
//
// Implements:
//   - SSL rejection (single byte 'N')
//   - Startup sequence (AuthenticationOk + ParameterStatus + ReadyForQuery)
//   - Simple Query Protocol ('Q')
//   - Extended Query Protocol ('P','B','D','E','S') — minimal
//   - Terminate ('X')
//
// All wire protocol integers are BIG-ENDIAN.
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
  typedef SOCKET pg_sock_t;
  #define PG_INVALID_SOCK INVALID_SOCKET
  #define PG_SOCK_ERR SOCKET_ERROR
  #define PG_RECV(s, b, l, f) recv(s, reinterpret_cast<char*>(b), l, f)
  #define PG_SEND(s, b, l, f) send(s, reinterpret_cast<const char*>(b), l, f)
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  typedef int pg_sock_t;
  #define PG_INVALID_SOCK (-1)
  #define PG_SOCK_ERR (-1)
  #define PG_RECV(s, b, l, f) recv(s, b, l, f)
  #define PG_SEND(s, b, l, f) send(s, b, l, f)
  #define closesocket close
#endif

#include <string>
#include <vector>
#include <sstream>
#include <iostream>
#include <thread>
#include <atomic>
#include <mutex>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <cstdint>

#include "../engine/engine.hpp"
#include "../parser/parser.hpp"
#include "../storage/storage.hpp"
#include "../dispatch.hpp"

namespace milansql {

// ============================================================
// PgServer — PostgreSQL Wire Protocol v3
// ============================================================
class PgServer {
public:
    PgServer(int port, const std::string& dbPath)
        : port_(port), dbPath_(dbPath), running_(false) {}

    // Start background listener thread (non-blocking)
    void start() {
#if defined(_WIN32)
        WSADATA wsaData;
        WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif
        running_.store(true);
        std::thread([this]() { listenerLoop(); }).detach();
    }

    void stop() { running_.store(false); }
    bool isRunning() const { return running_.load(); }

private:
    int port_;
    std::string dbPath_;
    std::atomic<bool> running_;
    Engine    engine_;
    milansql::MilanBinaryStorage storage_{dbPath_};
    std::mutex engineMutex_;
    Parser    parser_;

    // ── Listener loop ─────────────────────────────────────────

    void listenerLoop() {
        pg_sock_t listenSock = socket(AF_INET, SOCK_STREAM, 0);
        if (listenSock == PG_INVALID_SOCK) {
            std::cerr << "[PG] socket() failed\n";
            running_.store(false);
            return;
        }

        int opt = 1;
        setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&opt), sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_port        = htons(static_cast<uint16_t>(port_));
        addr.sin_addr.s_addr = INADDR_ANY;

        if (bind(listenSock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == PG_SOCK_ERR) {
            std::cerr << "[PG] bind() failed on port " << port_ << "\n";
            closesocket(listenSock);
            running_.store(false);
            return;
        }

        listen(listenSock, 64);
        std::cout << "[PG] PostgreSQL Wire Protocol server ready on port " << port_
                  << " — connect with: psql -h localhost -p " << port_
                  << " -U root -d public\n" << std::flush;

        // Load database
        {
            std::lock_guard<std::mutex> lk(engineMutex_);
            try { storage_.loadWithCount(engine_); } catch (...) {}
        }

        while (running_.load()) {
            sockaddr_in clientAddr{};
            socklen_t clientLen = sizeof(clientAddr);
            pg_sock_t clientSock = accept(listenSock,
                reinterpret_cast<sockaddr*>(&clientAddr), &clientLen);
            if (clientSock == PG_INVALID_SOCK) continue;

            std::thread([this, clientSock]() {
                handleClient(clientSock);
                closesocket(clientSock);
            }).detach();
        }

        closesocket(listenSock);
    }

    // ── Low-level I/O ─────────────────────────────────────────

    // Read exactly n bytes
    bool recvAll(pg_sock_t sock, uint8_t* buf, int n) {
        int received = 0;
        while (received < n) {
            int r = PG_RECV(sock, buf + received, n - received, 0);
            if (r <= 0) return false;
            received += r;
        }
        return true;
    }

    // Send all bytes in buf
    bool sendAll(pg_sock_t sock, const std::vector<uint8_t>& buf) {
        int total = static_cast<int>(buf.size());
        int sent = PG_SEND(sock, buf.data(), total, 0);
        return sent == total;
    }

    // ── Big-endian helpers ────────────────────────────────────

    static void writeInt32BE(std::vector<uint8_t>& buf, int32_t v) {
        uint32_t u = static_cast<uint32_t>(v);
        buf.push_back(static_cast<uint8_t>((u >> 24) & 0xff));
        buf.push_back(static_cast<uint8_t>((u >> 16) & 0xff));
        buf.push_back(static_cast<uint8_t>((u >>  8) & 0xff));
        buf.push_back(static_cast<uint8_t>((u      ) & 0xff));
    }

    static void writeInt16BE(std::vector<uint8_t>& buf, int16_t v) {
        uint16_t u = static_cast<uint16_t>(v);
        buf.push_back(static_cast<uint8_t>((u >> 8) & 0xff));
        buf.push_back(static_cast<uint8_t>((u     ) & 0xff));
    }

    static int32_t readInt32BE(const uint8_t* p) {
        return static_cast<int32_t>(
            (static_cast<uint32_t>(p[0]) << 24) |
            (static_cast<uint32_t>(p[1]) << 16) |
            (static_cast<uint32_t>(p[2]) <<  8) |
            (static_cast<uint32_t>(p[3])      ));
    }

    static int16_t readInt16BE(const uint8_t* p) {
        return static_cast<int16_t>(
            (static_cast<uint16_t>(p[0]) << 8) |
            (static_cast<uint16_t>(p[1])     ));
    }

    static void appendStr(std::vector<uint8_t>& buf, const std::string& s) {
        for (char c : s) buf.push_back(static_cast<uint8_t>(c));
    }

    static void appendCStr(std::vector<uint8_t>& buf, const std::string& s) {
        appendStr(buf, s);
        buf.push_back(0x00);
    }

    // ── Message builders ──────────────────────────────────────
    // All messages: type byte + int32 length (includes itself) + payload

    // AuthenticationOk: 'R' + int32(8) + int32(0)
    static std::vector<uint8_t> makeAuthOk() {
        std::vector<uint8_t> msg;
        msg.push_back('R');
        writeInt32BE(msg, 8);   // length = 4 (len field) + 4 (auth type)
        writeInt32BE(msg, 0);   // auth type = 0 = OK
        return msg;
    }

    // ParameterStatus: 'S' + int32(len) + key\0 + value\0
    static std::vector<uint8_t> makeParameterStatus(const std::string& key, const std::string& val) {
        std::vector<uint8_t> payload;
        appendCStr(payload, key);
        appendCStr(payload, val);

        std::vector<uint8_t> msg;
        msg.push_back('S');
        writeInt32BE(msg, static_cast<int32_t>(4 + payload.size()));
        msg.insert(msg.end(), payload.begin(), payload.end());
        return msg;
    }

    // BackendKeyData: 'K' + int32(12) + int32(pid) + int32(key)
    static std::vector<uint8_t> makeBackendKeyData(int32_t pid, int32_t key) {
        std::vector<uint8_t> msg;
        msg.push_back('K');
        writeInt32BE(msg, 12);
        writeInt32BE(msg, pid);
        writeInt32BE(msg, key);
        return msg;
    }

    // ReadyForQuery: 'Z' + int32(5) + status char
    static std::vector<uint8_t> makeReadyForQuery(char status = 'I') {
        std::vector<uint8_t> msg;
        msg.push_back('Z');
        writeInt32BE(msg, 5);
        msg.push_back(static_cast<uint8_t>(status));
        return msg;
    }

    // RowDescription: 'T' + int32(len) + int16(ncols) + col descriptors
    // For each col: name\0 + int32(tableOID=0) + int16(attrNum=0) + int32(typeOID=25=TEXT)
    //             + int16(typelen=-1) + int32(typemod=-1) + int16(format=0)
    static std::vector<uint8_t> makeRowDescription(const std::vector<std::string>& colNames) {
        std::vector<uint8_t> payload;
        writeInt16BE(payload, static_cast<int16_t>(colNames.size()));
        for (const auto& name : colNames) {
            appendCStr(payload, name);       // column name
            writeInt32BE(payload, 0);        // tableOID
            writeInt16BE(payload, 0);        // attrNum
            writeInt32BE(payload, 25);       // typeOID = 25 = TEXT
            writeInt16BE(payload, -1);       // typelen = -1 (variable)
            writeInt32BE(payload, -1);       // typemod = -1
            writeInt16BE(payload, 0);        // format = 0 = text
        }

        std::vector<uint8_t> msg;
        msg.push_back('T');
        writeInt32BE(msg, static_cast<int32_t>(4 + payload.size()));
        msg.insert(msg.end(), payload.begin(), payload.end());
        return msg;
    }

    // DataRow: 'D' + int32(len) + int16(ncols) + for each col: int32(vallen) + val
    // NULL values: int32(-1) with no following bytes
    static std::vector<uint8_t> makeDataRow(const std::vector<std::string>& values) {
        std::vector<uint8_t> payload;
        writeInt16BE(payload, static_cast<int16_t>(values.size()));
        for (const auto& val : values) {
            if (val == "NULL" || val.empty()) {
                // Send NULL as int32(-1)
                // Actually empty string should be sent as empty, not NULL
                // Let's only send NULL for the string "NULL"
                if (val == "NULL") {
                    writeInt32BE(payload, -1);
                } else {
                    // empty string: int32(0) + no bytes
                    writeInt32BE(payload, 0);
                }
            } else {
                writeInt32BE(payload, static_cast<int32_t>(val.size()));
                appendStr(payload, val);
            }
        }

        std::vector<uint8_t> msg;
        msg.push_back('D');
        writeInt32BE(msg, static_cast<int32_t>(4 + payload.size()));
        msg.insert(msg.end(), payload.begin(), payload.end());
        return msg;
    }

    // CommandComplete: 'C' + int32(len) + tag\0
    static std::vector<uint8_t> makeCommandComplete(const std::string& tag) {
        std::vector<uint8_t> msg;
        msg.push_back('C');
        writeInt32BE(msg, static_cast<int32_t>(4 + tag.size() + 1));
        appendCStr(msg, tag);
        return msg;
    }

    // ErrorResponse: 'E' + int32(len) + 'S' + "ERROR\0" + 'M' + msg\0 + '\0'
    static std::vector<uint8_t> makeErrorResponse(const std::string& errMsg) {
        std::vector<uint8_t> payload;
        payload.push_back('S');
        appendCStr(payload, "ERROR");
        payload.push_back('C');
        appendCStr(payload, "42000");  // SQL state: syntax error
        payload.push_back('M');
        appendCStr(payload, errMsg);
        payload.push_back(0x00);  // terminator

        std::vector<uint8_t> msg;
        msg.push_back('E');
        writeInt32BE(msg, static_cast<int32_t>(4 + payload.size()));
        msg.insert(msg.end(), payload.begin(), payload.end());
        return msg;
    }

    // EmptyQueryResponse: 'I' + int32(4)
    static std::vector<uint8_t> makeEmptyQueryResponse() {
        std::vector<uint8_t> msg;
        msg.push_back('I');
        writeInt32BE(msg, 4);
        return msg;
    }

    // ParseComplete: '1' + int32(4)
    static std::vector<uint8_t> makeParseComplete() {
        std::vector<uint8_t> msg;
        msg.push_back('1');
        writeInt32BE(msg, 4);
        return msg;
    }

    // BindComplete: '2' + int32(4)
    static std::vector<uint8_t> makeBindComplete() {
        std::vector<uint8_t> msg;
        msg.push_back('2');
        writeInt32BE(msg, 4);
        return msg;
    }

    // NoData: 'n' + int32(4)
    static std::vector<uint8_t> makeNoData() {
        std::vector<uint8_t> msg;
        msg.push_back('n');
        writeInt32BE(msg, 4);
        return msg;
    }

    // ── Query execution ───────────────────────────────────────

    struct QueryResult {
        bool isSelect = false;
        std::vector<std::string> colNames;
        std::vector<std::vector<std::string>> rows;
        std::string commandTag;  // e.g. "SELECT 3", "CREATE TABLE", "INSERT 0 1"
        std::string error;
    };

    QueryResult executeQuery(const std::string& sqlIn) {
        QueryResult r;

        // Trim whitespace
        std::string sql = sqlIn;
        size_t start = 0;
        while (start < sql.size() && (sql[start] == ' ' || sql[start] == '\t' ||
               sql[start] == '\n' || sql[start] == '\r')) ++start;
        sql = sql.substr(start);
        // Remove trailing semicolons and whitespace
        while (!sql.empty() && (sql.back() == ';' || sql.back() == ' ' ||
               sql.back() == '\t' || sql.back() == '\n' || sql.back() == '\r'))
            sql.pop_back();

        if (sql.empty()) {
            r.commandTag = "EMPTY";
            return r;
        }

        // Build uppercase version for classification
        std::string upper = sql;
        for (char& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

        // ── Handle psql/client management queries ──────────────

        // SET client_encoding, SET ..., SET search_path, etc.
        if (upper.substr(0, 3) == "SET") {
            r.commandTag = "SET";
            return r;
        }

        // SELECT version()
        if (upper.find("VERSION()") != std::string::npos) {
            r.isSelect = true;
            r.colNames = {"version"};
            r.rows.push_back({"PostgreSQL 14.0 (MilanSQL compatible)"});
            r.commandTag = "SELECT 1";
            return r;
        }

        // SELECT current_schema(), current_schemas(), pg_catalog.*
        if (upper.find("CURRENT_SCHEMA") != std::string::npos ||
            upper.find("CURRENT_USER") != std::string::npos ||
            upper.find("SESSION_USER") != std::string::npos) {
            r.isSelect = true;
            r.colNames = {"current_schema"};
            r.rows.push_back({"public"});
            r.commandTag = "SELECT 1";
            return r;
        }

        // SELECT 1
        if (upper == "SELECT 1") {
            r.isSelect = true;
            r.colNames = {"?column?"};
            r.rows.push_back({"1"});
            r.commandTag = "SELECT 1";
            return r;
        }

        // SHOW search_path, SHOW ...
        if (upper.substr(0, 4) == "SHOW") {
            r.isSelect = true;
            r.colNames = {"search_path"};
            r.rows.push_back({"\"$user\", public"});
            r.commandTag = "SHOW";
            return r;
        }

        // pg_catalog system table queries (from psql's \d, \l etc.)
        // Return empty result to avoid crashing
        if (upper.find("PG_CATALOG") != std::string::npos ||
            upper.find("INFORMATION_SCHEMA") != std::string::npos ||
            upper.find("PG_CLASS") != std::string::npos ||
            upper.find("PG_NAMESPACE") != std::string::npos ||
            upper.find("PG_TYPE") != std::string::npos ||
            upper.find("PG_ATTRIBUTE") != std::string::npos) {
            r.isSelect = true;
            r.colNames = {"result"};
            r.commandTag = "SELECT 0";
            return r;
        }

        // ── Execute via MilanSQL engine ─────────────────────────

        std::ostringstream captureOut;
        std::streambuf* oldBuf = std::cout.rdbuf(captureOut.rdbuf());

        bool ok = true;
        std::string errMsg;
        bool isSelect = false;
        Table selectResult("", {});
        std::string selectTableName;

        try {
            std::lock_guard<std::mutex> lk(engineMutex_);

            auto persistFn = [this]() {
                try { storage_.save(engine_); } catch (...) {}
            };
            auto noopFn = []() {};

            milansql::ParsedCommand cmd = parser_.parse(sql);

            if (cmd.type == CommandType::SELECT) {
                isSelect = true;
                selectTableName = cmd.tableName;
                selectResult = dispatch_executeSelectToTable(engine_, parser_, cmd);
            } else {
                dispatchCommand(cmd, engine_, parser_, sql, persistFn, noopFn, noopFn);
            }
        } catch (const std::exception& ex) {
            ok = false;
            errMsg = ex.what();
        } catch (...) {
            ok = false;
            errMsg = "Unknown error";
        }

        std::cout.rdbuf(oldBuf);

        if (!ok) {
            r.error = errMsg;
            return r;
        }

        if (isSelect) {
            r.isSelect = true;
            const auto& cols = selectResult.columns();
            const auto& rows = selectResult.rows();

            for (const auto& col : cols)
                r.colNames.push_back(col.name);

            for (const auto& row : rows) {
                if (row.xmax != 0) continue;  // skip MVCC dead rows
                std::vector<std::string> rowVals;
                for (size_t ci = 0; ci < cols.size(); ++ci) {
                    if (ci < row.values.size())
                        rowVals.push_back(row.values[ci]);
                    else
                        rowVals.push_back("NULL");
                }
                r.rows.push_back(std::move(rowVals));
            }

            r.commandTag = "SELECT " + std::to_string(r.rows.size());
        } else {
            // Determine command tag from original SQL
            if (upper.substr(0, 6) == "INSERT") {
                r.commandTag = "INSERT 0 1";
            } else if (upper.substr(0, 6) == "UPDATE") {
                r.commandTag = "UPDATE 1";
            } else if (upper.substr(0, 6) == "DELETE") {
                r.commandTag = "DELETE 1";
            } else if (upper.substr(0, 12) == "CREATE TABLE") {
                r.commandTag = "CREATE TABLE";
            } else if (upper.substr(0, 11) == "DROP TABLE") {
                r.commandTag = "DROP TABLE";
            } else if (upper.substr(0, 13) == "CREATE SCHEMA") {
                r.commandTag = "CREATE SCHEMA";
            } else if (upper.substr(0, 11) == "DROP SCHEMA") {
                r.commandTag = "DROP SCHEMA";
            } else if (upper.substr(0, 13) == "CREATE INDEX") {
                r.commandTag = "CREATE INDEX";
            } else if (upper.substr(0, 10) == "DROP INDEX") {
                r.commandTag = "DROP INDEX";
            } else if (upper.substr(0, 5) == "BEGIN") {
                r.commandTag = "BEGIN";
            } else if (upper.substr(0, 6) == "COMMIT") {
                r.commandTag = "COMMIT";
            } else if (upper.substr(0, 8) == "ROLLBACK") {
                r.commandTag = "ROLLBACK";
            } else {
                // Extract first word as tag
                size_t sp = upper.find(' ');
                r.commandTag = (sp != std::string::npos) ? upper.substr(0, sp) : upper;
            }
        }

        return r;
    }

    // Send a complete query result (SELECT or DML/DDL)
    void sendQueryResult(pg_sock_t sock, const QueryResult& result) {
        if (!result.error.empty()) {
            sendAll(sock, makeErrorResponse(result.error));
            sendAll(sock, makeReadyForQuery());
            return;
        }

        if (result.commandTag == "EMPTY") {
            sendAll(sock, makeEmptyQueryResponse());
            sendAll(sock, makeReadyForQuery());
            return;
        }

        if (result.isSelect) {
            // Send RowDescription
            sendAll(sock, makeRowDescription(result.colNames));
            // Send DataRows
            for (const auto& row : result.rows) {
                sendAll(sock, makeDataRow(row));
            }
            // CommandComplete
            sendAll(sock, makeCommandComplete(result.commandTag));
        } else {
            // DML/DDL — just CommandComplete
            sendAll(sock, makeCommandComplete(result.commandTag));
        }

        sendAll(sock, makeReadyForQuery());
    }

    // ── Client handler ────────────────────────────────────────

    void handleClient(pg_sock_t sock) {
        // ── Step 1: Read startup message ───────────────────────
        // The first message may be an SSL request (protocol = 0x04D2162F)
        // or a real StartupMessage.
        // Both start with a 4-byte length, then 4-byte protocol version.

        while (true) {
            // Read 4-byte length
            uint8_t lenBuf[4];
            if (!recvAll(sock, lenBuf, 4)) return;
            int32_t msgLen = readInt32BE(lenBuf);

            if (msgLen < 8) {
                // Invalid
                return;
            }

            // Read remaining bytes (msgLen - 4, since we already read 4)
            int32_t remainLen = msgLen - 4;
            std::vector<uint8_t> payload(static_cast<size_t>(remainLen));
            if (remainLen > 0 && !recvAll(sock, payload.data(), remainLen)) return;

            // Read protocol version from first 4 bytes of payload
            if (payload.size() < 4) return;
            int32_t protoVer = readInt32BE(payload.data());

            if (protoVer == 80877103) {
                // SSL request (0x04D2162F) — reject with single byte 'N'
                uint8_t reject = 'N';
                PG_SEND(sock, &reject, 1, 0);
                // Continue to next message (real startup)
                continue;
            }

            if (protoVer == 80877102) {
                // CancelRequest — just close
                return;
            }

            // Real StartupMessage (protocol 196608 = 0x00030000)
            // Parse key=value pairs from payload[4..]
            // We just accept all users/databases without authentication
            break;
        }

        // ── Step 2: Send authentication OK and startup messages ──
        {
            std::vector<uint8_t> resp;

            // AuthenticationOk
            auto authOk = makeAuthOk();
            resp.insert(resp.end(), authOk.begin(), authOk.end());

            // ParameterStatus messages
            auto addParam = [&](const std::string& k, const std::string& v) {
                auto ps = makeParameterStatus(k, v);
                resp.insert(resp.end(), ps.begin(), ps.end());
            };

            addParam("server_version",    "14.0");
            addParam("client_encoding",   "UTF8");
            addParam("server_encoding",   "UTF8");
            addParam("integer_datetimes", "on");
            addParam("DateStyle",         "ISO, MDY");
            addParam("TimeZone",          "UTC");
            addParam("is_superuser",      "on");
            addParam("session_authorization", "root");
            addParam("standard_conforming_strings", "on");

            // BackendKeyData (fake pid=1, key=0)
            auto bkd = makeBackendKeyData(1, 0);
            resp.insert(resp.end(), bkd.begin(), bkd.end());

            // ReadyForQuery
            auto rfq = makeReadyForQuery();
            resp.insert(resp.end(), rfq.begin(), rfq.end());

            sendAll(sock, resp);
        }

        // ── Step 3: Query loop ────────────────────────────────

        // State for extended query protocol
        std::string pendingSql;  // from Parse message

        while (true) {
            // Read 1-byte message type
            uint8_t msgType = 0;
            {
                int r = PG_RECV(sock, reinterpret_cast<char*>(&msgType), 1, 0);
                if (r <= 0) break;  // client disconnected
            }

            // Read 4-byte length
            uint8_t lenBuf[4];
            if (!recvAll(sock, lenBuf, 4)) break;
            int32_t msgLen = readInt32BE(lenBuf);

            // Read message body (msgLen - 4 bytes, since length includes itself)
            int32_t bodyLen = msgLen - 4;
            std::vector<uint8_t> body;
            if (bodyLen > 0) {
                body.resize(static_cast<size_t>(bodyLen));
                if (!recvAll(sock, body.data(), bodyLen)) break;
            }

            switch (msgType) {
                case 'Q': {
                    // Simple Query
                    // body is: sql_string\0
                    std::string sql(body.begin(), body.end());
                    // Remove trailing null bytes
                    while (!sql.empty() && sql.back() == '\0') sql.pop_back();

                    if (sql.empty() || sql == ";" || isOnlySemicolons(sql)) {
                        sendAll(sock, makeEmptyQueryResponse());
                        sendAll(sock, makeReadyForQuery());
                        break;
                    }

                    // Handle multiple statements (split on ';')
                    auto stmts = milansql::splitStatements(sql);
                    if (stmts.empty()) {
                        sendAll(sock, makeEmptyQueryResponse());
                        sendAll(sock, makeReadyForQuery());
                        break;
                    }

                    for (size_t si = 0; si < stmts.size(); ++si) {
                        const auto& stmt = stmts[si];
                        if (stmt.empty()) continue;

                        QueryResult qr = executeQuery(stmt);

                        if (!qr.error.empty()) {
                            sendAll(sock, makeErrorResponse(qr.error));
                            sendAll(sock, makeReadyForQuery());
                            // Stop processing further statements on error
                            goto nextMessage;
                        }

                        if (qr.commandTag == "EMPTY") {
                            sendAll(sock, makeEmptyQueryResponse());
                        } else if (qr.isSelect) {
                            sendAll(sock, makeRowDescription(qr.colNames));
                            for (const auto& row : qr.rows) {
                                sendAll(sock, makeDataRow(row));
                            }
                            sendAll(sock, makeCommandComplete(qr.commandTag));
                        } else {
                            sendAll(sock, makeCommandComplete(qr.commandTag));
                        }
                    }

                    sendAll(sock, makeReadyForQuery());
                    nextMessage:;
                    break;
                }

                case 'P': {
                    // Parse (extended query)
                    // Format: statement_name\0 query\0 int16(numParams) ...
                    pendingSql = "";
                    // Find the query string (after statement name)
                    size_t pos = 0;
                    // Skip statement name (null-terminated)
                    while (pos < body.size() && body[pos] != 0) ++pos;
                    ++pos;  // skip null
                    // Read query string
                    size_t qstart = pos;
                    while (pos < body.size() && body[pos] != 0) ++pos;
                    if (pos > qstart) {
                        pendingSql = std::string(body.begin() + static_cast<int>(qstart),
                                                 body.begin() + static_cast<int>(pos));
                    }
                    sendAll(sock, makeParseComplete());
                    break;
                }

                case 'B': {
                    // Bind (extended query) — we ignore parameters
                    sendAll(sock, makeBindComplete());
                    break;
                }

                case 'D': {
                    // Describe
                    // We send NoData (simplified) — psql can handle this
                    sendAll(sock, makeNoData());
                    break;
                }

                case 'E': {
                    // Execute — run pendingSql
                    if (pendingSql.empty()) {
                        sendAll(sock, makeCommandComplete("SELECT 0"));
                        break;
                    }

                    QueryResult qr = executeQuery(pendingSql);
                    pendingSql = "";

                    if (!qr.error.empty()) {
                        sendAll(sock, makeErrorResponse(qr.error));
                    } else if (qr.commandTag == "EMPTY") {
                        sendAll(sock, makeEmptyQueryResponse());
                    } else if (qr.isSelect) {
                        // Note: RowDescription was supposed to be sent at Describe stage
                        // but since we sent NoData, send it here
                        sendAll(sock, makeRowDescription(qr.colNames));
                        for (const auto& row : qr.rows) {
                            sendAll(sock, makeDataRow(row));
                        }
                        sendAll(sock, makeCommandComplete(qr.commandTag));
                    } else {
                        sendAll(sock, makeCommandComplete(qr.commandTag));
                    }
                    break;
                }

                case 'S': {
                    // Sync — send ReadyForQuery
                    sendAll(sock, makeReadyForQuery());
                    break;
                }

                case 'X': {
                    // Terminate
                    return;
                }

                case 'H': {
                    // Flush — no-op for us
                    break;
                }

                default: {
                    // Unknown message — log and continue
                    std::cerr << "[PG] Unknown message type: '"
                              << static_cast<char>(msgType) << "' (0x"
                              << std::hex << static_cast<int>(msgType) << std::dec
                              << ")\n";
                    // Send ReadyForQuery to keep the client happy
                    sendAll(sock, makeReadyForQuery());
                    break;
                }
            }
        }
    }

    // Check if a string is only semicolons and whitespace
    static bool isOnlySemicolons(const std::string& s) {
        for (char c : s) {
            if (c != ';' && c != ' ' && c != '\t' && c != '\n' && c != '\r')
                return false;
        }
        return true;
    }
};

} // namespace milansql
