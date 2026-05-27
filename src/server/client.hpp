#pragma once
// ============================================================
// client.hpp — MilanSQL TCP Client (Phase 47)
// ============================================================

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
  // Windows.h defines DELETE as a macro — undefine it
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

#include <string>
#include <iostream>

class MilanClient {
public:
    MilanClient(const std::string& host, int port)
        : host_(host), port_(port) {}

    bool connect();
    void runREPL();
    void disconnect();

private:
    std::string host_;
    int port_;
    sock_t sock_ = INVALID_SOCK;

    std::string sendQuery(const std::string& sql);
    static std::string recvResponse(sock_t sock);
};

// ── MilanClient::connect ──────────────────────────────────────
inline bool MilanClient::connect() {
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "WSAStartup fehlgeschlagen.\n";
        return false;
    }
#endif

    sock_ = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_ == INVALID_SOCK) {
        std::cerr << "Fehler: socket() fehlgeschlagen.\n";
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((unsigned short)port_);

    // Convert host string to address
    if (inet_pton(AF_INET, host_.c_str(), &addr.sin_addr) <= 0) {
        std::cerr << "Fehler: Ungueltige Adresse '" << host_ << "'\n";
        closesocket(sock_);
        sock_ = INVALID_SOCK;
        return false;
    }

    if (::connect(sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCK_ERR) {
        std::cerr << "Fehler: Verbindung zu " << host_ << ":" << port_
                  << " fehlgeschlagen.\n";
        closesocket(sock_);
        sock_ = INVALID_SOCK;
        return false;
    }

    return true;
}

// ── MilanClient::disconnect ───────────────────────────────────
inline void MilanClient::disconnect() {
    if (sock_ != INVALID_SOCK) {
        closesocket(sock_);
        sock_ = INVALID_SOCK;
    }
#ifdef _WIN32
    WSACleanup();
#endif
}

// ── MilanClient::recvResponse ─────────────────────────────────
inline std::string MilanClient::recvResponse(sock_t sock) {
    std::string data;
    char buf[4096];
    while (true) {
        int n = recv(sock, buf, (int)(sizeof(buf) - 1), 0);
        if (n <= 0) break;
        buf[n] = '\0';
        data += buf;
        // Check if response ends with "END\n"
        if (data.size() >= 4 && data.compare(data.size() - 4, 4, "END\n") == 0) break;
    }
    return data;
}

// ── MilanClient::sendQuery ────────────────────────────────────
inline std::string MilanClient::sendQuery(const std::string& sql) {
    std::string message = "SQL_QUERY\n" + sql + "\nEND\n";

    // Send all bytes
    size_t sent = 0;
    while (sent < message.size()) {
        int n = send(sock_, message.c_str() + sent,
                     (int)(message.size() - sent), 0);
        if (n <= 0) return "ERROR\nVerbindung unterbrochen.\nEND\n";
        sent += (size_t)n;
    }

    return recvResponse(sock_);
}

// ── MilanClient::runREPL ──────────────────────────────────────
inline void MilanClient::runREPL() {
    std::cout << "MilanSQL Client verbunden mit " << host_ << ":" << port_ << "\n";
    std::cout << "Tippe SQL-Befehle. 'exit' zum Beenden.\n\n";

    std::string line;
    while (true) {
        std::cout << "milansql[net]> " << std::flush;
        if (!std::getline(std::cin, line)) {
            std::cout << "\nVerbindung getrennt.\n";
            break;
        }
        if (line.empty()) continue;

        // Send query and receive response
        std::string response = sendQuery(line);

        // Parse response: starts with "OK\n" or "ERROR\n", ends with "END\n"
        const std::string okPrefix  = "OK\n";
        const std::string errPrefix = "ERROR\n";
        const std::string endSuffix = "END\n";

        if (response.size() >= okPrefix.size() &&
            response.substr(0, okPrefix.size()) == okPrefix) {
            // Strip OK\n prefix and END\n suffix
            std::string body = response.substr(okPrefix.size());
            if (body.size() >= endSuffix.size() &&
                body.compare(body.size() - endSuffix.size(),
                              endSuffix.size(), endSuffix) == 0) {
                body = body.substr(0, body.size() - endSuffix.size());
            }
            if (!body.empty()) std::cout << body;
        } else if (response.size() >= errPrefix.size() &&
                   response.substr(0, errPrefix.size()) == errPrefix) {
            // Strip ERROR\n prefix and END\n suffix
            std::string body = response.substr(errPrefix.size());
            if (body.size() >= endSuffix.size() &&
                body.compare(body.size() - endSuffix.size(),
                              endSuffix.size(), endSuffix) == 0) {
                body = body.substr(0, body.size() - endSuffix.size());
            }
            std::cout << "  FEHLER: " << body << "\n";
        } else {
            std::cout << "  Unbekannte Antwort: " << response << "\n";
        }

        // Check if the server confirmed EXIT
        if (line == "exit" || line == "EXIT") break;
    }

    disconnect();
}
