#pragma once
// ============================================================
// master_repl.hpp — Master Replication Server for MilanSQL
// Phase 59: Master/Slave Replication
// Bug #12: TLS encryption + auth token verification
//
// Listens on the replication port (e.g. 4407).
// Slave connects → sends AUTH_TOKEN:xxx\nREPL_SYNC\nFROM_POS:N\nEND\n
// Master verifies token, then replies with REPL_DATA or REPL_UPTODATE.
// All traffic encrypted via TLS when SSL is enabled.
// ============================================================

#include <thread>
#include <atomic>
#include <mutex>
#include <string>
#include <fstream>
#include "binlog.hpp"
#include "repl_state.hpp"
#include "../ssl/tls_context.hpp"

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  using sock_t = SOCKET;
  static inline void repl_closesock(sock_t s) { closesocket(s); }
  // Ensure Winsock is initialized before any socket call
  static inline void repl_ensureWinsock() {
      static bool done = false;
      if (!done) { WSADATA w; WSAStartup(MAKEWORD(2,2), &w); done = true; }
  }
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  using sock_t = int;
  // Guard: both master_repl.hpp and slave_repl.hpp define this;
  // #define (not constexpr) so #ifndef works across translation units.
  #ifndef INVALID_SOCKET
  #define INVALID_SOCKET (-1)
  #endif
  static inline void repl_closesock(sock_t s) { ::close(s); }
  static inline void repl_ensureWinsock() {} // no-op on Unix
#endif

namespace milansql {

// Shared replication auth token — loaded from <dbPath>.repl_token
inline std::string& g_replAuthToken() {
    static std::string token;
    return token;
}

// Load or generate replication token
inline void loadOrCreateReplToken(const std::string& dbPath) {
    std::string path = dbPath + ".repl_token";
    // Try load existing
    std::ifstream in(path);
    if (in) {
        std::getline(in, g_replAuthToken());
        in.close();
        if (!g_replAuthToken().empty()) return;
    }
    // Generate 32-byte hex token
    std::random_device rd;
    std::string tok;
    tok.reserve(64);
    for (int i = 0; i < 32; ++i) {
        uint8_t b = static_cast<uint8_t>(rd() & 0xff);
        static const char hex[] = "0123456789abcdef";
        tok += hex[b >> 4];
        tok += hex[b & 0xf];
    }
    g_replAuthToken() = tok;
    std::ofstream out(path);
    if (out) { out << tok << "\n"; out.flush(); }
    std::cout << "  [Repl] Generated new replication token: " << path << "\n";
}

class MasterReplication {
public:
    // Phase 172: bindAny=true listens on 0.0.0.0 (remote replicas);
    // default remains loopback-only (Bug #12 hardening).
    explicit MasterReplication(int replPort, BinlogWriter& binlog,
                               bool bindAny = false)
        : replPort_(replPort), binlog_(binlog)
        , bindAny_(bindAny)
        , running_(false), slaveCount_(0)
    {}

    ~MasterReplication() {
        running_ = false;
        replNotifyNewData();   // Phase 172: wake long-poll waiters
        if (acceptThread_.joinable()) acceptThread_.join();
    }

    void start() {
        running_ = true;
        acceptThread_ = std::thread(&MasterReplication::listenLoop, this);
    }

private:
    int           replPort_;
    BinlogWriter& binlog_;
    bool          bindAny_;
    std::atomic<bool> running_;
    std::thread   acceptThread_;
    std::atomic<int> slaveCount_;

    // Constant-time string comparison to prevent timing attacks
    static bool secureCompare(const std::string& a, const std::string& b) {
        if (a.size() != b.size()) return false;
        volatile unsigned char result = 0;
        for (size_t i = 0; i < a.size(); ++i)
            result |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
        return result == 0;
    }

    void listenLoop() {
        repl_ensureWinsock();
        sock_t srv = socket(AF_INET, SOCK_STREAM, 0);
        if (srv == INVALID_SOCKET) {
            std::cerr << "MasterRepl: socket() failed\n";
            return;
        }

        int opt = 1;
        setsockopt(srv, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&opt), sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_port        = htons(static_cast<uint16_t>(replPort_));
        // Bug #12: loopback by default; Phase 172: 0.0.0.0 with --repl-bind-any
        addr.sin_addr.s_addr = htonl(bindAny_ ? INADDR_ANY : INADDR_LOOPBACK);

        if (bind(srv, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            std::cerr << "MasterRepl: bind() failed on port " << replPort_ << "\n";
            repl_closesock(srv);
            return;
        }
        if (listen(srv, 10) != 0) {
            std::cerr << "MasterRepl: listen() failed\n";
            repl_closesock(srv);
            return;
        }

        std::cout << "  Replikations-Port " << replPort_ << " bereit (TLS+Auth).\n";

        while (running_) {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(srv, &fds);
            timeval tv{1, 0};
            int sel = select(static_cast<int>(srv) + 1, &fds, nullptr, nullptr, &tv);
            if (sel <= 0) continue;

            sockaddr_in clientAddr{};
            socklen_t   clientLen = sizeof(clientAddr);
            sock_t client = accept(srv,
                reinterpret_cast<sockaddr*>(&clientAddr), &clientLen);
            if (client == INVALID_SOCKET) continue;

            ++slaveCount_;
            g_replState.connectedSlaves.store(slaveCount_.load());

            // Serve each slave in a detached thread
            std::thread([this, client, clientAddr]() {
                // Get client IP for logging
                char ipStr[INET_ADDRSTRLEN] = {};
                inet_ntop(AF_INET, &clientAddr.sin_addr, ipStr, sizeof(ipStr));

                // TLS handshake if SSL enabled
                if (g_sslConfig().enabled.load() && g_tlsContext().isReady()) {
                    TlsSocket ts = g_tlsContext().wrapAccepted(client);
                    if (!ts.tlsActive) {
                        std::cerr << "  [Repl] TLS handshake failed from " << ipStr << "\n";
                        ts.close();
                        --slaveCount_;
                        g_replState.connectedSlaves.store(slaveCount_.load());
                        return;
                    }
                    handleSlaveTls(ts, ipStr);
                    ts.close();
                } else {
                    handleSlave(client, ipStr);
                }
                --slaveCount_;
                g_replState.connectedSlaves.store(slaveCount_.load());
            }).detach();
        }
        repl_closesock(srv);
    }

    // Verify AUTH_TOKEN in request, return false if auth fails
    bool verifyAuth(const std::string& req, const char* ipStr) {
        auto pos = req.find("AUTH_TOKEN:");
        if (pos == std::string::npos) {
            std::cerr << "  [Repl] REJECTED " << ipStr << " — no auth token\n";
            return false;
        }
        auto endPos = req.find('\n', pos);
        std::string token = req.substr(pos + 11,
            endPos != std::string::npos ? endPos - pos - 11 : std::string::npos);
        // Trim whitespace
        while (!token.empty() && (token.back() == '\r' || token.back() == ' '))
            token.pop_back();

        if (!secureCompare(token, g_replAuthToken())) {
            std::cerr << "  [Repl] REJECTED " << ipStr << " — invalid auth token\n";
            return false;
        }
        return true;
    }

    // Process replication request and build response.
    // Phase 172 streaming: if the request contains "WAIT", long-poll —
    // block up to STREAM_WAIT_MS for new binlog entries instead of
    // immediately answering REPL_UPTODATE. Combined with the slave's
    // tight re-poll loop this delivers statements in realtime.
    static constexpr int STREAM_WAIT_MS = 3000;

    std::string processRequest(const std::string& req, const char* ipStr) {
        long long fromPos = 0;
        auto it = req.find("FROM_POS:");
        if (it != std::string::npos) {
            try { fromPos = std::stoll(req.substr(it + 9)); }
            catch (...) { fromPos = 0; }
        }

        // Phase 172: FROM_POS doubles as the slave's ack position
        // (slave has durably applied everything up to fromPos)
        replRecordAck(fromPos);
        // Phase 173: per-replica registry for /replication/status
        replRecordSlaveInfo(ipStr ? ipStr : "?", fromPos);

        auto entries = binlog_.readFrom(fromPos);

        // Phase 172: streaming long-poll — wait for new data
        if (entries.empty() && req.find("WAIT") != std::string::npos) {
            replWaitForNewData(STREAM_WAIT_MS, [this, fromPos]() {
                return binlog_.getCurrentPos() > fromPos || !running_;
            });
            entries = binlog_.readFrom(fromPos);
        }

        std::string ts = "TS:" + std::to_string(replNowMs()) + "\n";
        std::string response;
        if (entries.empty()) {
            response = "REPL_UPTODATE\nPOS:" +
                       std::to_string(binlog_.getCurrentPos()) + "\n" + ts + "END\n";
        } else {
            response = "REPL_DATA\n";
            for (const auto& e : entries) {
                response += "STMT:" + e.sql + "\n";
                response += "POS:"  + std::to_string(e.pos) + "\n";
            }
            response += ts + "END\n";
        }
        return response;
    }

    // Handle slave over TLS connection
    void handleSlaveTls(TlsSocket& ts, const char* ipStr) {
        std::string req;
        char buf[2048];
        while (req.find("END\n") == std::string::npos) {
            int n = ts.read(buf, static_cast<int>(sizeof(buf) - 1));
            if (n <= 0) return;
            buf[n] = '\0';
            req.append(buf, static_cast<size_t>(n));
            if (req.size() > 65536) return; // prevent memory exhaustion
        }

        if (!verifyAuth(req, ipStr)) {
            std::string reject = "REPL_AUTH_FAILED\nEND\n";
            ts.write(reject.c_str(), static_cast<int>(reject.size()));
            return;
        }

        std::string response = processRequest(req, ipStr);
        ts.write(response.c_str(), static_cast<int>(response.size()));
    }

    // Handle slave over plain TCP (fallback when TLS not available)
    void handleSlave(sock_t client, const char* ipStr) {
        std::string req;
        char buf[2048];
        while (req.find("END\n") == std::string::npos) {
            int n = recv(client, buf, static_cast<int>(sizeof(buf) - 1), 0);
            if (n <= 0) { repl_closesock(client); return; }
            buf[n] = '\0';
            req.append(buf, static_cast<size_t>(n));
            if (req.size() > 65536) { repl_closesock(client); return; }
        }

        if (!verifyAuth(req, ipStr)) {
            std::string reject = "REPL_AUTH_FAILED\nEND\n";
            send(client, reject.c_str(), static_cast<int>(reject.size()), 0);
            repl_closesock(client);
            return;
        }

        std::string response = processRequest(req, ipStr);
        send(client, response.c_str(),
             static_cast<int>(response.size()), 0);
        repl_closesock(client);
    }
};

} // namespace milansql
