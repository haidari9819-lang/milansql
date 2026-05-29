#pragma once
// ============================================================
// mysql_server.hpp — Phase 74: MySQL Wire Protocol Server
//
// Implements a subset of the MySQL Client/Server Protocol v10
// so that standard MySQL clients can connect to MilanSQL.
//
//   mysql -h 127.0.0.1 -P 4407 -u root --skip-ssl
//
// Supported:
//   COM_QUERY  (0x03) — execute SQL, return resultset / OK / ERR
//   COM_QUIT   (0x01) — close connection
//   COM_PING   (0x0e) — return OK
//   COM_INIT_DB(0x02) — USE database (ignored, always OK)
//   COM_QUERY for: SHOW DATABASES, SET NAMES, SET @...
//
// Packet format (MySQL wire protocol):
//   [3 bytes payload_length LE][1 byte sequence_id][payload...]
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
  #define SOCK_ERR SOCKET_ERROR
  #define MILAN_RECV(s, b, l, f) recv(s, reinterpret_cast<char*>(b), l, f)
  #define MILAN_SEND(s, b, l, f) send(s, reinterpret_cast<const char*>(b), l, f)
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  typedef int sock_t;
  #define INVALID_SOCK (-1)
  #define SOCK_ERR (-1)
  #define closesocket close
  #define MILAN_RECV(s, b, l, f) recv(s, b, l, f)
  #define MILAN_SEND(s, b, l, f) send(s, b, l, f)
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

#include "../engine/engine.hpp"
#include "../parser/parser.hpp"
#include "../storage/storage.hpp"
#include "../dispatch.hpp"

namespace milansql {

// ── MySQL capabilities ────────────────────────────────────────
static constexpr uint32_t MY_CLIENT_LONG_PASSWORD   = 0x00000001;
static constexpr uint32_t MY_CLIENT_FOUND_ROWS       = 0x00000002;
static constexpr uint32_t MY_CLIENT_LONG_FLAG        = 0x00000004;
static constexpr uint32_t MY_CLIENT_CONNECT_WITH_DB  = 0x00000008;
static constexpr uint32_t MY_CLIENT_PROTOCOL_41      = 0x00000200;
static constexpr uint32_t MY_CLIENT_TRANSACTIONS     = 0x00002000;
static constexpr uint32_t MY_CLIENT_SECURE_CONNECTION= 0x00008000;
static constexpr uint32_t MY_CLIENT_MULTI_RESULTS    = 0x00020000;

// ── Field types ───────────────────────────────────────────────
static constexpr uint8_t  MYSQL_TYPE_VAR_STRING = 0xfd;
static constexpr uint8_t  MYSQL_TYPE_NULL       = 0x06;

// ── Server status flags ───────────────────────────────────────
static constexpr uint16_t MY_SERVER_STATUS_AUTOCOMMIT = 0x0002;

// ============================================================
// Helper: length-encoded integer / string
// ============================================================

static inline void appendU8(std::vector<uint8_t>& buf, uint8_t v) {
    buf.push_back(v);
}
static inline void appendU16LE(std::vector<uint8_t>& buf, uint16_t v) {
    buf.push_back(static_cast<uint8_t>(v & 0xff));
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xff));
}
static inline void appendU32LE(std::vector<uint8_t>& buf, uint32_t v) {
    buf.push_back(static_cast<uint8_t>(v & 0xff));
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xff));
    buf.push_back(static_cast<uint8_t>((v >> 16) & 0xff));
    buf.push_back(static_cast<uint8_t>((v >> 24) & 0xff));
}
static inline void appendStr(std::vector<uint8_t>& buf, const std::string& s) {
    for (char c : s) buf.push_back(static_cast<uint8_t>(c));
}
static inline void appendNullStr(std::vector<uint8_t>& buf, const std::string& s) {
    appendStr(buf, s);
    buf.push_back(0x00);
}
// Length-encoded integer
static inline void appendLEI(std::vector<uint8_t>& buf, uint64_t v) {
    if (v < 251) {
        buf.push_back(static_cast<uint8_t>(v));
    } else if (v < 65536) {
        buf.push_back(0xfc);
        appendU16LE(buf, static_cast<uint16_t>(v));
    } else {
        buf.push_back(0xfd);
        buf.push_back(static_cast<uint8_t>(v & 0xff));
        buf.push_back(static_cast<uint8_t>((v >> 8) & 0xff));
        buf.push_back(static_cast<uint8_t>((v >> 16) & 0xff));
    }
}
// Length-encoded string
static inline void appendLES(std::vector<uint8_t>& buf, const std::string& s) {
    appendLEI(buf, s.size());
    appendStr(buf, s);
}

// ============================================================
// MysqlServer
// ============================================================
class MysqlServer {
public:
    MysqlServer(int port, const std::string& dbPath)
        : port_(port), dbPath_(dbPath) {}

    void run() {
#if defined(_WIN32)
        WSADATA wsaData;
        WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif
        sock_t listenSock = socket(AF_INET, SOCK_STREAM, 0);
        if (listenSock == INVALID_SOCK) {
            std::cerr << "[MySQL] socket() failed\n";
            return;
        }
        int opt = 1;
        setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&opt), sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_port        = htons(static_cast<uint16_t>(port_));
        addr.sin_addr.s_addr = INADDR_ANY;

        if (bind(listenSock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCK_ERR) {
            std::cerr << "[MySQL] bind() failed on port " << port_ << "\n";
            closesocket(listenSock);
            return;
        }
        listen(listenSock, 64);
        std::cout << "[MySQL] Server ready on port " << port_
                  << " — connect with: mysql -h 127.0.0.1 -P " << port_
                  << " -u root --skip-ssl\n";

        // Load database
        {
            std::lock_guard<std::mutex> lk(engineMutex_);
            try { storage_.loadWithCount(engine_); } catch (...) {}
        }

        std::atomic<uint32_t> connId{1};

        while (true) {
            sockaddr_in clientAddr{};
            socklen_t clientLen = sizeof(clientAddr);
            sock_t clientSock = accept(listenSock,
                reinterpret_cast<sockaddr*>(&clientAddr), &clientLen);
            if (clientSock == INVALID_SOCK) continue;

            uint32_t cid = connId.fetch_add(1);
            std::thread([this, clientSock, cid]() {
                handleClient(clientSock, cid);
                closesocket(clientSock);
            }).detach();
        }
        closesocket(listenSock);
    }

private:
    int port_;
    std::string dbPath_;
    Engine    engine_;
    milansql::MilanBinaryStorage storage_{dbPath_};
    std::mutex engineMutex_;
    Parser    parser_;

    // ── Packet I/O ─────────────────────────────────────────────

    // Read exactly n bytes from socket into buf
    bool recvAll(sock_t sock, uint8_t* buf, int n) {
        int received = 0;
        while (received < n) {
            int r = MILAN_RECV(sock, buf + received, n - received, 0);
            if (r <= 0) return false;
            received += r;
        }
        return true;
    }

    // Read one MySQL packet: returns {seq, payload} or {255, empty} on error
    std::pair<uint8_t, std::vector<uint8_t>> readPacket(sock_t sock) {
        uint8_t header[4];
        if (!recvAll(sock, header, 4))
            return {255, {}};
        uint32_t len = static_cast<uint32_t>(header[0])
                     | (static_cast<uint32_t>(header[1]) << 8)
                     | (static_cast<uint32_t>(header[2]) << 16);
        uint8_t seq = header[3];
        std::vector<uint8_t> payload(len);
        if (len > 0 && !recvAll(sock, payload.data(), static_cast<int>(len)))
            return {255, {}};
        return {seq, std::move(payload)};
    }

    // Send a MySQL packet — header + payload in one send() to avoid TCP segmentation
    bool sendPacket(sock_t sock, uint8_t seq, const std::vector<uint8_t>& payload) {
        uint32_t len = static_cast<uint32_t>(payload.size());
        std::vector<uint8_t> frame;
        frame.reserve(4 + len);
        frame.push_back(static_cast<uint8_t>(len & 0xff));
        frame.push_back(static_cast<uint8_t>((len >> 8) & 0xff));
        frame.push_back(static_cast<uint8_t>((len >> 16) & 0xff));
        frame.push_back(seq);
        frame.insert(frame.end(), payload.begin(), payload.end());
        int total = static_cast<int>(frame.size());
        int sent = MILAN_SEND(sock, frame.data(), total, 0);
        return sent == total;
    }

    // ── OK / ERR / EOF packets ─────────────────────────────────

    void sendOK(sock_t sock, uint8_t seq,
                uint64_t affectedRows = 0, uint64_t lastInsertId = 0) {
        std::vector<uint8_t> pkt;
        appendU8(pkt, 0x00);                           // OK header
        appendLEI(pkt, affectedRows);
        appendLEI(pkt, lastInsertId);
        appendU16LE(pkt, MY_SERVER_STATUS_AUTOCOMMIT); // status flags
        appendU16LE(pkt, 0);                           // warnings
        sendPacket(sock, seq, pkt);
    }

    void sendERR(sock_t sock, uint8_t seq, uint16_t errCode, const std::string& msg) {
        std::vector<uint8_t> pkt;
        appendU8(pkt, 0xff);                // ERR header
        appendU16LE(pkt, errCode);
        appendU8(pkt, '#');                 // sql state marker
        // 5-byte SQL state
        for (char c : std::string("42000")) appendU8(pkt, static_cast<uint8_t>(c));
        appendStr(pkt, msg);
        sendPacket(sock, seq, pkt);
    }

    void sendEOF(sock_t sock, uint8_t seq) {
        std::vector<uint8_t> pkt;
        appendU8(pkt, 0xfe);   // EOF header
        appendU16LE(pkt, 0);   // warnings
        appendU16LE(pkt, MY_SERVER_STATUS_AUTOCOMMIT);
        sendPacket(sock, seq, pkt);
    }

    // ── Handshake ─────────────────────────────────────────────

    void sendHandshake(sock_t sock, uint8_t seq, uint32_t connId) {
        std::vector<uint8_t> pkt;

        // Protocol version 10
        appendU8(pkt, 10);

        // Server version string (null-terminated)
        appendNullStr(pkt, "MilanSQL 2.4.0");

        // Connection id (4 bytes LE)
        appendU32LE(pkt, connId);

        // Auth-plugin-data part 1 (8 bytes — fake, we accept any password)
        for (int i = 0; i < 8; ++i) appendU8(pkt, static_cast<uint8_t>(0x41 + i));

        // Filler
        appendU8(pkt, 0x00);

        // Capability flags (lower 2 bytes)
        uint32_t caps = MY_CLIENT_LONG_PASSWORD
                      | MY_CLIENT_FOUND_ROWS
                      | MY_CLIENT_LONG_FLAG
                      | MY_CLIENT_PROTOCOL_41
                      | MY_CLIENT_TRANSACTIONS
                      | MY_CLIENT_SECURE_CONNECTION
                      | MY_CLIENT_MULTI_RESULTS;
        appendU16LE(pkt, static_cast<uint16_t>(caps & 0xffff));

        // Character set (utf8 = 33)
        appendU8(pkt, 33);

        // Status flags
        appendU16LE(pkt, MY_SERVER_STATUS_AUTOCOMMIT);

        // Capability flags (upper 2 bytes)
        appendU16LE(pkt, static_cast<uint16_t>((caps >> 16) & 0xffff));

        // Length of auth plugin data (21 = 8+13)
        appendU8(pkt, 21);

        // Reserved (10 bytes of zero)
        for (int i = 0; i < 10; ++i) appendU8(pkt, 0x00);

        // Auth-plugin-data part 2 (13 bytes)
        for (int i = 0; i < 13; ++i) appendU8(pkt, static_cast<uint8_t>(0x61 + i));

        // Auth plugin name (null-terminated)
        appendNullStr(pkt, "mysql_native_password");

        sendPacket(sock, seq, pkt);
    }

    // Read and discard client login packet (we always accept)
    bool readClientHandshake(sock_t sock) {
        auto [seq, payload] = readPacket(sock);
        return !payload.empty();
    }

    // ── ResultSet ─────────────────────────────────────────────

    void sendResultSet(sock_t sock, uint8_t& seq,
                       const Table& result, const std::string& tblName) {
        const auto& cols = result.columns();
        const auto& rows = result.rows();
        size_t numCols = cols.size();

        // 1. Column count packet
        {
            std::vector<uint8_t> pkt;
            appendLEI(pkt, numCols);
            sendPacket(sock, seq++, pkt);
        }

        // 2. Column definition packets
        for (const auto& col : cols) {
            std::vector<uint8_t> pkt;
            appendLES(pkt, "def");          // catalog
            appendLES(pkt, "");             // schema
            appendLES(pkt, tblName);        // table
            appendLES(pkt, tblName);        // org_table
            appendLES(pkt, col.name);       // name
            appendLES(pkt, col.name);       // org_name
            appendU8(pkt, 0x0c);            // length of fixed fields
            appendU16LE(pkt, 33);           // character set (utf8)
            appendU32LE(pkt, 500);          // column length
            appendU8(pkt, MYSQL_TYPE_VAR_STRING); // column type
            appendU16LE(pkt, 0);            // flags
            appendU8(pkt, 0);               // decimals
            appendU16LE(pkt, 0);            // filler
            sendPacket(sock, seq++, pkt);
        }

        // 3. EOF after column definitions
        sendEOF(sock, seq++);

        // 4. Row data packets
        for (const auto& row : rows) {
            if (row.xmax != 0) continue;  // skip MVCC dead rows
            std::vector<uint8_t> pkt;
            for (size_t ci = 0; ci < numCols; ++ci) {
                if (ci < row.values.size() && row.values[ci] != "NULL") {
                    appendLES(pkt, row.values[ci]);
                } else {
                    // NULL column value
                    appendU8(pkt, 0xfb);
                }
            }
            sendPacket(sock, seq++, pkt);
        }

        // 5. EOF after rows
        sendEOF(sock, seq++);
    }

    // ── Query execution ────────────────────────────────────────

    // Execute SQL and send response to client
    void executeAndReply(sock_t sock, uint8_t& seq, const std::string& sql) {
        // Handle MySQL client management queries silently
        std::string upper = sql;
        for (char& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

        // Trim leading whitespace
        size_t start = 0;
        while (start < upper.size() && (upper[start] == ' ' || upper[start] == '\t' || upper[start] == '\n')) ++start;
        upper = upper.substr(start);

        // SET NAMES, SET CHARACTER SET, SET @var — always OK
        if (upper.substr(0, 3) == "SET") {
            sendOK(sock, seq++);
            return;
        }
        // SELECT @@version_comment, @@version, etc.
        if (upper.find("@@VERSION") != std::string::npos ||
            upper.find("@@VERSION_COMMENT") != std::string::npos ||
            upper.find("@@GLOBAL.") != std::string::npos ||
            upper.find("@@SESSION.") != std::string::npos ||
            upper.find("@@") != std::string::npos) {
            // Return a single-row result with the version string
            Table result("vars", {Column("Value", "TEXT")});
            result.insert(Row({"MilanSQL 2.4.0"}));
            sendResultSet(sock, seq, result, "vars");
            return;
        }
        // SHOW DATABASES
        if (upper.substr(0, 14) == "SHOW DATABASES") {
            Table result("Databases", {Column("Database", "TEXT")});
            result.insert(Row({"milansql"}));
            sendResultSet(sock, seq, result, "Databases");
            return;
        }
        // USE database
        if (upper.substr(0, 3) == "USE") {
            sendOK(sock, seq++);
            return;
        }
        // SELECT 1 (keepalive)
        if (upper == "SELECT 1" || upper == "SELECT 1;") {
            Table result("dual", {Column("1", "INT")});
            result.insert(Row({"1"}));
            sendResultSet(sock, seq, result, "dual");
            return;
        }
        // SELECT DATABASE()
        if (upper.find("DATABASE()") != std::string::npos) {
            Table result("DATABASE()", {Column("DATABASE()", "TEXT")});
            result.insert(Row({"milansql"}));
            sendResultSet(sock, seq, result, "DATABASE()");
            return;
        }
        // SELECT USER(), CURRENT_USER()
        if (upper.find("USER()") != std::string::npos ||
            upper.find("CURRENT_USER") != std::string::npos) {
            Table result("USER()", {Column("USER()", "TEXT")});
            result.insert(Row({"root@localhost"}));
            sendResultSet(sock, seq, result, "USER()");
            return;
        }

        // ── Execute via engine ──────────────────────────────────
        // Redirect stdout to a buffer for non-SELECT ops
        std::ostringstream captureOut;
        std::streambuf* oldBuf = std::cout.rdbuf(captureOut.rdbuf());

        bool hasResult = false;
        Table lastResult("", {});
        std::string lastTableName;
        uint64_t affectedRows = 0;

        try {
            std::lock_guard<std::mutex> lk(engineMutex_);

            // Persist helper (no-op for reads)
            auto persistFn = [this]() {
                try { storage_.save(engine_); } catch (...) {}
            };
            auto noopFn = []() {};

            milansql::ParsedCommand cmd = parser_.parse(sql);

            // For SELECT-like commands, capture result
            if (cmd.type == CommandType::SELECT) {
                try {
                    milansql::Table result = dispatch_executeSelectToTable(
                        engine_, parser_, cmd);
                    lastResult    = std::move(result);
                    lastTableName = cmd.tableName;
                    hasResult     = true;
                } catch (const std::exception& ex) {
                    std::cout.rdbuf(oldBuf);
                    sendERR(sock, seq++, 1064, ex.what());
                    return;
                }
            } else {
                // Non-select: dispatch normally, capture affected row count from output
                dispatchCommand(cmd, engine_, parser_, sql,
                                persistFn, noopFn, noopFn);
            }
        } catch (const std::exception& ex) {
            std::cout.rdbuf(oldBuf);
            sendERR(sock, seq++, 1064, ex.what());
            return;
        }

        std::cout.rdbuf(oldBuf);

        if (hasResult) {
            sendResultSet(sock, seq, lastResult, lastTableName);
        } else {
            // Try to extract affected rows from captured output
            std::string captured = captureOut.str();
            // Look for "N Zeile" pattern
            for (size_t p = 0; p < captured.size(); ++p) {
                if (std::isdigit(static_cast<unsigned char>(captured[p]))) {
                    try {
                        affectedRows = std::stoull(captured.substr(p));
                    } catch (...) {}
                    break;
                }
            }
            sendOK(sock, seq++, affectedRows);
        }
    }

    // ── Client handler ─────────────────────────────────────────

    void handleClient(sock_t sock, uint32_t connId) {
        uint8_t seq = 0;

        // 1. Send server greeting
        sendHandshake(sock, seq++, connId);

        // 2. Read client handshake (auth)
        if (!readClientHandshake(sock)) return;

        // 3. Send OK to complete auth
        sendOK(sock, seq++);
        seq = 0;  // reset sequence for query phase

        // 4. Query loop
        while (true) {
            auto [pktSeq, payload] = readPacket(sock);
            if (payload.empty()) break;  // client disconnected

            seq = pktSeq + 1;

            if (payload.empty()) continue;
            uint8_t cmd = payload[0];

            if (cmd == 0x01) {
                // COM_QUIT
                break;
            } else if (cmd == 0x0e) {
                // COM_PING
                sendOK(sock, seq++);
            } else if (cmd == 0x02) {
                // COM_INIT_DB (USE database)
                sendOK(sock, seq++);
            } else if (cmd == 0x03) {
                // COM_QUERY
                std::string sql(payload.begin() + 1, payload.end());
                // Remove trailing \0 if present
                while (!sql.empty() && sql.back() == '\0') sql.pop_back();
                if (!sql.empty()) {
                    executeAndReply(sock, seq, sql);
                } else {
                    sendOK(sock, seq++);
                }
                seq = 0;  // reset sequence for next command
            } else {
                // Unknown command — return OK
                sendOK(sock, seq++);
            }
        }
    }
};

} // namespace milansql
