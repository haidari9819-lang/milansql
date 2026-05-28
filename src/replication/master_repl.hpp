#pragma once
// ============================================================
// master_repl.hpp — Master Replication Server for MilanSQL
// Phase 59: Master/Slave Replication
//
// Listens on the replication port (e.g. 4407).
// Slave connects → sends REPL_SYNC\nFROM_POS:N\nEND\n
// Master replies with REPL_DATA or REPL_UPTODATE.
// ============================================================

#include <thread>
#include <atomic>
#include <mutex>
#include <string>
#include "binlog.hpp"
#include "repl_state.hpp"

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  using sock_t = SOCKET;
  static inline void repl_closesock(sock_t s) { closesocket(s); }
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  using sock_t = int;
  #ifndef INVALID_SOCKET
  constexpr sock_t INVALID_SOCKET = -1;
  #endif
  static inline void repl_closesock(sock_t s) { ::close(s); }
#endif

namespace milansql {

class MasterReplication {
public:
    explicit MasterReplication(int replPort, BinlogWriter& binlog)
        : replPort_(replPort), binlog_(binlog)
        , running_(false), slaveCount_(0)
    {}

    ~MasterReplication() {
        running_ = false;
        if (acceptThread_.joinable()) acceptThread_.join();
    }

    void start() {
        running_ = true;
        acceptThread_ = std::thread(&MasterReplication::listenLoop, this);
    }

private:
    int           replPort_;
    BinlogWriter& binlog_;
    std::atomic<bool> running_;
    std::thread   acceptThread_;
    std::atomic<int> slaveCount_;

    void listenLoop() {
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
        addr.sin_addr.s_addr = INADDR_ANY;

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

        std::cout << "  Replikations-Port " << replPort_ << " bereit.\n";

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
            std::thread([this, client]() {
                handleSlave(client);
                --slaveCount_;
                g_replState.connectedSlaves.store(slaveCount_.load());
            }).detach();
        }
        repl_closesock(srv);
    }

    void handleSlave(sock_t client) {
        // Read complete request (terminated by "END\n")
        std::string req;
        char buf[2048];
        while (req.find("END\n") == std::string::npos) {
            int n = recv(client, buf, static_cast<int>(sizeof(buf) - 1), 0);
            if (n <= 0) { repl_closesock(client); return; }
            buf[n] = '\0';
            req.append(buf, static_cast<size_t>(n));
        }

        // Parse FROM_POS
        long long fromPos = 0;
        auto it = req.find("FROM_POS:");
        if (it != std::string::npos) {
            try { fromPos = std::stoll(req.substr(it + 9)); }
            catch (...) { fromPos = 0; }
        }

        // Build response
        auto entries = binlog_.readFrom(fromPos);
        std::string response;

        if (entries.empty()) {
            response = "REPL_UPTODATE\nPOS:" +
                       std::to_string(binlog_.getCurrentPos()) + "\nEND\n";
        } else {
            response = "REPL_DATA\n";
            for (const auto& e : entries) {
                response += "STMT:" + e.sql + "\n";
                response += "POS:"  + std::to_string(e.pos) + "\n";
            }
            response += "END\n";
        }

        send(client, response.c_str(),
             static_cast<int>(response.size()), 0);
        repl_closesock(client);
    }
};

} // namespace milansql
