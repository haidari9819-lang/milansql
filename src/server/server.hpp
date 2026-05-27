#pragma once
// ============================================================
// server.hpp — MilanSQL TCP Server (Phase 47)
// ============================================================

#if defined(_WIN32)
  // Windows (MSVC and MSYS2/MinGW): use Winsock2
  // Note: #pragma comment not used — ws2_32 is linked via CMakeLists.txt
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
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  typedef int sock_t;
  #define INVALID_SOCK (-1)
  #define SOCK_ERR (-1)
  #define closesocket close
#endif

#include <thread>
#include <mutex>
#include <functional>
#include <string>
#include <sstream>
#include <iostream>
#include <fstream>
#include <map>
#include <chrono>
#include <atomic>

#include "../engine/engine.hpp"
#include "../parser/parser.hpp"
#include "../storage/storage.hpp"
#include "../dispatch.hpp"

// ── Phase 54D: Process tracking ──────────────────────────────
struct ProcessInfo {
    int         id;
    std::string user;
    std::string host;
    std::string db;
    std::string currentQuery;
    std::chrono::steady_clock::time_point connectedAt;
};

class MilanServer {
public:
    MilanServer(int port, const std::string& dbPath)
        : port_(port), dbPath_(dbPath), storage_(dbPath_) {}

    void run();

private:
    int port_;
    std::string dbPath_;
    milansql::Engine engine_;
    milansql::MilanBinaryStorage storage_;
    std::mutex engineMutex_;

    // Phase 54D: Process list
    std::map<int, ProcessInfo> processList_;
    std::mutex processListMutex_;
    std::atomic<int> nextPid_{1};

    void handleClient(sock_t clientSock);
    std::string executeQuery(const std::string& sql, int pid);

    std::string getProcessListStr();

    static std::string recvAll(sock_t sock);
    static void sendAll(sock_t sock, const std::string& data);

    void initEngine();
};

// ── MilanServer::initEngine ───────────────────────────────────
inline void MilanServer::initEngine() {
    // Remove WAL on startup (crash recovery)
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
                        bodyLine[bi] == '\\' && bodyLine[bi+1] == 'n') {
                        decoded += ' '; ++bi;
                    } else decoded += bodyLine[bi];
                }
                def.body = decoded;
                engine_.createProcedure(def);
            }
        }
    }
}

// ── MilanServer::recvAll ──────────────────────────────────────
inline std::string MilanServer::recvAll(sock_t sock) {
    std::string data;
    char buf[4096];
    while (true) {
        int n = recv(sock, buf, (int)(sizeof(buf) - 1), 0);
        if (n <= 0) break;
        buf[n] = '\0';
        data += buf;
        // Check if message ends with "END\n"
        if (data.size() >= 4 && data.compare(data.size() - 4, 4, "END\n") == 0) break;
    }
    return data;
}

// ── MilanServer::sendAll ──────────────────────────────────────
inline void MilanServer::sendAll(sock_t sock, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        int n = send(sock, data.c_str() + (int)sent, (int)(data.size() - sent), 0);
        if (n <= 0) break;
        sent += (size_t)n;
    }
}

// ── MilanServer::getProcessListStr ───────────────────────────
inline std::string MilanServer::getProcessListStr() {
    std::lock_guard<std::mutex> lk(processListMutex_);
    if (processList_.empty()) {
        return "\n  (Keine aktiven Verbindungen)\n\n";
    }
    // Column widths
    size_t wId = 2, wUser = 4, wHost = 9, wDb = 6, wQuery = 25;
    for (const auto& [id, p] : processList_) {
        wId    = std::max(wId,    std::to_string(p.id).size());
        wUser  = std::max(wUser,  p.user.size());
        wHost  = std::max(wHost,  p.host.size());
        wDb    = std::max(wDb,    p.db.size());
        wQuery = std::max(wQuery, p.currentQuery.size());
    }
    wQuery = std::min(wQuery, size_t(50));

    auto cell = [](const std::string& s, size_t w) {
        std::string r = " " + (s.size() > w ? s.substr(0, w-3) + "..." : s);
        while (r.size() < w + 2) r += ' ';
        r += "\u2502";
        return r;
    };
    std::vector<size_t> ws = {wId, wUser, wHost, wDb, wQuery};
    auto hline = [&](const std::string& l, const std::string& m, const std::string& r) {
        std::string s = "  " + l;
        for (size_t i = 0; i < ws.size(); ++i) {
            for (size_t j = 0; j < ws[i] + 2; ++j) s += "\u2500";
            if (i + 1 < ws.size()) s += m;
        }
        return s + r + "\n";
    };
    std::string out = "\n";
    out += hline("\u250c", "\u252c", "\u2510");
    out += "  \u2502" + cell("ID", wId) + cell("User", wUser)
                      + cell("Host", wHost) + cell("DB", wDb) + cell("Query", wQuery) + "\n";
    out += hline("\u251c", "\u253c", "\u2524");
    for (const auto& [id, p] : processList_) {
        out += "  \u2502" + cell(std::to_string(p.id), wId)
                          + cell(p.user, wUser) + cell(p.host, wHost)
                          + cell(p.db, wDb) + cell(p.currentQuery, wQuery) + "\n";
    }
    out += hline("\u2514", "\u2534", "\u2518");
    out += "  " + std::to_string(processList_.size()) + " aktive Verbindung(en)\n\n";
    return out;
}

// ── MilanServer::executeQuery ─────────────────────────────────
inline std::string MilanServer::executeQuery(const std::string& sql, int /*pid*/) {
    std::lock_guard<std::mutex> lock(engineMutex_);

    // Redirect cout to capture output
    std::ostringstream captured;
    std::streambuf* oldBuf = std::cout.rdbuf(captured.rdbuf());

    auto persistFn = [this]() {
        if (engine_.isInTransaction()) return;
        try { storage_.save(engine_); }
        catch (const std::exception& ex) {
            std::cout << "  WARNUNG: Speichern fehlgeschlagen: " << ex.what() << "\n";
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

    try {
        milansql::Parser parser;

        // Parse subqueries
        milansql::ParsedCommand cmd = parser.parse(sql);
        for (const auto& sq : cmd.subqueries) {
            if (sq.condIdx < cmd.whereConds.size()) {
                cmd.whereConds[sq.condIdx].inList =
                    engine_.subqueryValues(sq.subTable, sq.subCol,
                                           sq.subWhere, sq.subWhereLogic);
            }
        }

        auto getProcessListFn = [this]() -> std::string {
            return this->getProcessListStr();
        };

        milansql::dispatchCommand(cmd, engine_, parser, sql,
                                  persistFn, saveProceduresFn, saveTriggFn,
                                  getProcessListFn);

        std::cout.rdbuf(oldBuf);
        return "OK\n" + captured.str() + "END\n";
    } catch (const std::exception& e) {
        std::cout.rdbuf(oldBuf);
        return std::string("ERROR\n") + e.what() + "\nEND\n";
    } catch (...) {
        std::cout.rdbuf(oldBuf);
        return std::string("ERROR\nUnbekannter Fehler\nEND\n");
    }
}

// ── MilanServer::handleClient ─────────────────────────────────
inline void MilanServer::handleClient(sock_t clientSock) {
    // Phase 54D: Register process
    int pid = nextPid_++;
    {
        std::lock_guard<std::mutex> lk(processListMutex_);
        ProcessInfo pi;
        pi.id           = pid;
        pi.user         = engine_.getCurrentUser();
        pi.host         = "localhost";
        pi.db           = engine_.getCurrentSchema();
        pi.currentQuery = "(idle)";
        pi.connectedAt  = std::chrono::steady_clock::now();
        processList_[pid] = pi;
    }

    while (true) {
        std::string message = recvAll(clientSock);
        if (message.empty()) break;  // client disconnected

        // Parse protocol: SQL_QUERY\n<sql>\nEND\n
        const std::string prefix = "SQL_QUERY\n";
        const std::string suffix = "\nEND\n";

        if (message.size() < prefix.size() + suffix.size() ||
            message.substr(0, prefix.size()) != prefix) {
            std::string errResp = "ERROR\nInvalid protocol: expected SQL_QUERY\\n<sql>\\nEND\\n\nEND\n";
            sendAll(clientSock, errResp);
            continue;
        }

        size_t sqlStart = prefix.size();
        size_t sqlEnd   = message.size() - suffix.size();
        std::string sql = message.substr(sqlStart, sqlEnd - sqlStart);

        // Handle EXIT command specially
        {
            std::string up = sql;
            for (char& c : up) c = (char)std::toupper((unsigned char)c);
            size_t s = 0;
            while (s < up.size() && (up[s] == ' ' || up[s] == '\t')) ++s;
            if (up.substr(s) == "EXIT" || up.substr(s) == "EXIT;") {
                sendAll(clientSock, "OK\nAuf Wiedersehen!\nEND\n");
                break;
            }
        }

        // Phase 54D: Update current query
        {
            std::lock_guard<std::mutex> lk(processListMutex_);
            auto it = processList_.find(pid);
            if (it != processList_.end()) it->second.currentQuery = sql;
        }

        std::string response = executeQuery(sql, pid);

        // Phase 54D: Mark as idle after query
        {
            std::lock_guard<std::mutex> lk(processListMutex_);
            auto it = processList_.find(pid);
            if (it != processList_.end()) it->second.currentQuery = "(idle)";
        }

        sendAll(clientSock, response);
    }

    // Phase 54D: Remove process on disconnect
    {
        std::lock_guard<std::mutex> lk(processListMutex_);
        processList_.erase(pid);
    }
    closesocket(clientSock);
}

// ── MilanServer::run ──────────────────────────────────────────
inline void MilanServer::run() {
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "WSAStartup fehlgeschlagen.\n";
        return;
    }
#endif

    initEngine();

    sock_t serverSock = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSock == INVALID_SOCK) {
        std::cerr << "Fehler: socket() fehlgeschlagen.\n";
#ifdef _WIN32
        WSACleanup();
#endif
        return;
    }

    // Allow address reuse
    int opt = 1;
    setsockopt(serverSock, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&opt), sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons((unsigned short)port_);

    if (bind(serverSock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCK_ERR) {
        std::cerr << "Fehler: bind() fehlgeschlagen auf Port " << port_ << "\n";
        closesocket(serverSock);
#ifdef _WIN32
        WSACleanup();
#endif
        return;
    }

    if (listen(serverSock, SOMAXCONN) == SOCK_ERR) {
        std::cerr << "Fehler: listen() fehlgeschlagen.\n";
        closesocket(serverSock);
#ifdef _WIN32
        WSACleanup();
#endif
        return;
    }

    std::cout << "MilanSQL Server lauscht auf Port " << port_ << " ...\n";
    std::cout << "Warte auf Verbindungen (Ctrl+C zum Beenden)\n" << std::flush;

    while (true) {
        sockaddr_in clientAddr{};
        socklen_t clientLen = sizeof(clientAddr);
        sock_t clientSock = accept(serverSock,
                                   reinterpret_cast<sockaddr*>(&clientAddr),
                                   &clientLen);
        if (clientSock == INVALID_SOCK) {
            // Server may have been stopped
            break;
        }

        std::thread(&MilanServer::handleClient, this, clientSock).detach();
    }

    closesocket(serverSock);
#ifdef _WIN32
    WSACleanup();
#endif
}
