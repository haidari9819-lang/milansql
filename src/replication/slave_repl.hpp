#pragma once
// ============================================================
// slave_repl.hpp — Slave Replication Client for MilanSQL
// Phase 59: Master/Slave Replication
//
// Polls master every 500ms for new binlog entries.
// Replays received SQL on the local engine.
// Auto-reconnects every 5s on connection failure.
// ============================================================

#include <thread>
#include <atomic>
#include <string>
#include <functional>
#include <sstream>
#include <chrono>
#include <iostream>
#include "repl_state.hpp"

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  using slave_sock_t = SOCKET;
  static inline void slave_closesock(slave_sock_t s) { closesocket(s); }
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <netdb.h>
  #include <unistd.h>
  using slave_sock_t = int;
  #ifndef INVALID_SOCKET
  #define INVALID_SOCKET (-1)
  #endif
  static inline void slave_closesock(slave_sock_t s) { ::close(s); }
#endif

namespace milansql {

class SlaveReplication {
public:
    using ExecFn = std::function<void(const std::string&)>;

    SlaveReplication(const std::string& masterHost, int masterPort,
                     ExecFn fn)
        : masterHost_(masterHost), masterPort_(masterPort)
        , execFn_(std::move(fn))
        , running_(false), paused_(false)
    {}

    ~SlaveReplication() {
        running_ = false;
        if (thread_.joinable()) thread_.join();
    }

    void start() {
        running_ = true;
        paused_  = false;
        thread_  = std::thread(&SlaveReplication::syncLoop, this);
    }

    // STOP SLAVE — pauses sync (thread keeps running)
    void stop() {
        paused_ = true;
        setStatus("Stopped");
        g_replState.slaveRunning.store(false);
    }

    // START SLAVE — resumes sync
    void resume() {
        paused_ = false;
    }

private:
    std::string       masterHost_;
    int               masterPort_;
    ExecFn            execFn_;
    std::atomic<bool> running_;
    std::atomic<bool> paused_;
    std::thread       thread_;

    void setStatus(const std::string& s) {
        std::lock_guard<std::mutex> lk(g_replState.statusMu);
        g_replState.slaveStatus = s;
    }

    void syncLoop() {
        g_replState.isSlave.store(true);

        while (running_) {
            if (paused_) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }

            if (!trySync()) {
                setStatus("Reconnecting...");
                g_replState.slaveRunning.store(false);
                // Wait up to 5s before retry, checking running_ / paused_ periodically
                for (int i = 0; i < 50 && running_ && !paused_; ++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
        }

        setStatus("Stopped");
        g_replState.slaveRunning.store(false);
    }

    // Connect to master, request sync, replay received statements.
    // Returns true on success (even if up-to-date), false on error.
    bool trySync() {
        slave_sock_t sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock == INVALID_SOCKET) return false;

        // Resolve hostname (handles "localhost" correctly)
        struct addrinfo hints{}, *res = nullptr;
        hints.ai_family   = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(masterHost_.c_str(), nullptr, &hints, &res) != 0 || !res) {
            slave_closesock(sock);
            return false;
        }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(static_cast<uint16_t>(masterPort_));
        addr.sin_addr   = reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr;
        freeaddrinfo(res);

        if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            slave_closesock(sock);
            return false;
        }

        // Send sync request
        long long myPos = g_replState.slavePos.load();
        std::string req = "REPL_SYNC\nFROM_POS:" + std::to_string(myPos) + "\nEND\n";
        if (send(sock, req.c_str(), static_cast<int>(req.size()), 0) <= 0) {
            slave_closesock(sock);
            return false;
        }

        // Read response (5s timeout, terminated by "END\n")
        auto t0 = std::chrono::high_resolution_clock::now();
        std::string resp;
        char buf[8192];
        while (resp.find("END\n") == std::string::npos) {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(sock, &fds);
            timeval tv{5, 0};
            int sel = select(static_cast<int>(sock) + 1, &fds, nullptr, nullptr, &tv);
            if (sel <= 0) { slave_closesock(sock); return false; }
            int n = recv(sock, buf, static_cast<int>(sizeof(buf) - 1), 0);
            if (n <= 0) break;
            buf[n] = '\0';
            resp.append(buf, static_cast<size_t>(n));
        }
        slave_closesock(sock);

        auto t1  = std::chrono::high_resolution_clock::now();
        long long lagMs = std::chrono::duration_cast<
            std::chrono::milliseconds>(t1 - t0).count();
        g_replState.slaveLagMs.store(lagMs);

        // Handle REPL_UPTODATE
        if (resp.find("REPL_UPTODATE") != std::string::npos) {
            auto it = resp.find("POS:");
            if (it != std::string::npos) {
                try {
                    g_replState.slavePos.store(std::stoll(resp.substr(it + 4)));
                } catch (...) {}
            }
            setStatus("Connected");
            g_replState.slaveRunning.store(true);
            return true;
        }

        // Handle REPL_DATA — replay each statement
        if (resp.find("REPL_DATA") != std::string::npos) {
            std::istringstream ss(resp);
            std::string line;
            while (std::getline(ss, line)) {
                if (line.rfind("STMT:", 0) == 0) {
                    std::string sql = line.substr(5);
                    try { execFn_(sql); }
                    catch (const std::exception& ex) {
                        std::cerr << "  [Slave] Replay-Fehler: "
                                  << ex.what() << "\n";
                    }
                    catch (...) {}
                } else if (line.rfind("POS:", 0) == 0) {
                    try {
                        g_replState.slavePos.store(
                            std::stoll(line.substr(4)));
                    } catch (...) {}
                }
            }
            setStatus("Connected");
            g_replState.slaveRunning.store(true);
            return true;
        }

        return false;
    }
};

} // namespace milansql
