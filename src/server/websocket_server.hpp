#pragma once
// ============================================================
// websocket_server.hpp — MilanSQL WebSocket Server (Phase 106)
// Real-time notifications over WebSocket (RFC 6455)
// Zero external dependencies
// ============================================================

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #ifdef DELETE
    #undef DELETE
  #endif
  typedef SOCKET ws_sock_t;
  #define WS_INVALID_SOCK INVALID_SOCKET
  #define ws_closesocket closesocket
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  typedef int ws_sock_t;
  #define WS_INVALID_SOCK (-1)
  #define ws_closesocket close
#endif

#include <string>
#include <vector>
#include <map>
#include <set>
#include <mutex>
#include <thread>
#include <atomic>
#include <functional>
#include <sstream>
#include <cstdint>
#include <algorithm>
#include <iostream>

#include "../engine/engine.hpp"
#include "../parser/parser.hpp"
// Note: dispatch.hpp is already included before this file (this header is
// included at the end of dispatch.hpp to avoid circular dependency)

namespace milansql {

class WebSocketServer {
public:
    WebSocketServer(Engine& engine, int port)
        : engine_(engine), port_(port), running_(false), listenSock_(WS_INVALID_SOCK) {}

    void start();
    void stop();
    bool isRunning() const { return running_.load(); }

    // Called by dispatch hooks when rows change
    void notifyTableChange(const std::string& tableName, const std::string& op,
                           const std::vector<std::string>& colNames,
                           const std::vector<std::string>& values);

private:
    Engine& engine_;
    int port_;
    std::atomic<bool> running_;
    ws_sock_t listenSock_;

    mutable std::mutex mu_;
    std::set<ws_sock_t> clients_;
    std::map<ws_sock_t, std::set<std::string>> subscriptions_;

    void listenerLoop();
    void handleClient(ws_sock_t sock);

    bool performHandshake(ws_sock_t sock);
    std::string readFrame(ws_sock_t sock);
    void sendFrame(ws_sock_t sock, const std::string& payload);
    void sendClose(ws_sock_t sock);

    void handleMessage(ws_sock_t sock, const std::string& payload);
    std::string executeQuery(const std::string& sql) const;

    void removeClient(ws_sock_t sock);

    // ── SHA1 (FIPS 180-4) returns 20 raw bytes ────────────────
    static std::vector<uint8_t> sha1Raw(const std::string& input) {
        uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};

        std::vector<uint8_t> msg(input.begin(), input.end());
        uint64_t origBitLen = static_cast<uint64_t>(msg.size()) * 8;
        msg.push_back(0x80);
        while (msg.size() % 64 != 56) msg.push_back(0x00);
        for (int i = 7; i >= 0; i--)
            msg.push_back(static_cast<uint8_t>((origBitLen >> (8 * i)) & 0xFF));

        for (size_t i = 0; i < msg.size(); i += 64) {
            uint32_t w[80];
            for (int j = 0; j < 16; j++) {
                w[j] = (static_cast<uint32_t>(msg[i + static_cast<size_t>(j) * 4])     << 24) |
                       (static_cast<uint32_t>(msg[i + static_cast<size_t>(j) * 4 + 1]) << 16) |
                       (static_cast<uint32_t>(msg[i + static_cast<size_t>(j) * 4 + 2]) <<  8) |
                        static_cast<uint32_t>(msg[i + static_cast<size_t>(j) * 4 + 3]);
            }
            for (int j = 16; j < 80; j++) {
                uint32_t v = w[j-3] ^ w[j-8] ^ w[j-14] ^ w[j-16];
                w[j] = (v << 1) | (v >> 31);
            }
            uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
            for (int j = 0; j < 80; j++) {
                uint32_t f, k;
                if (j < 20)      { f = (b & c) | ((~b) & d); k = 0x5A827999u; }
                else if (j < 40) { f = b ^ c ^ d;             k = 0x6ED9EBA1u; }
                else if (j < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
                else             { f = b ^ c ^ d;             k = 0xCA62C1D6u; }
                uint32_t tmp = ((a << 5) | (a >> 27)) + f + e + k + w[j];
                e = d; d = c; c = (b << 30) | (b >> 2); b = a; a = tmp;
            }
            h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
        }

        std::vector<uint8_t> result(20);
        for (int i = 0; i < 5; i++) {
            result[static_cast<size_t>(i) * 4]     = static_cast<uint8_t>((h[i] >> 24) & 0xFF);
            result[static_cast<size_t>(i) * 4 + 1] = static_cast<uint8_t>((h[i] >> 16) & 0xFF);
            result[static_cast<size_t>(i) * 4 + 2] = static_cast<uint8_t>((h[i] >>  8) & 0xFF);
            result[static_cast<size_t>(i) * 4 + 3] = static_cast<uint8_t>( h[i]        & 0xFF);
        }
        return result;
    }

    static std::string base64Encode(const std::vector<uint8_t>& data) {
        static const char* B64 =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        int n = static_cast<int>(data.size());
        int i = 0;
        while (i < n) {
            uint32_t b = static_cast<uint32_t>(data[static_cast<size_t>(i++)]) << 16;
            if (i < n) b |= static_cast<uint32_t>(data[static_cast<size_t>(i++)]) << 8;
            if (i < n) b |= static_cast<uint32_t>(data[static_cast<size_t>(i++)]);
            out += B64[(b >> 18) & 63];
            out += B64[(b >> 12) & 63];
            // padding logic: determine how many bytes were consumed this round
            // i advances by 1,2,3 per iteration; rem = original i mod 3
            out += (i >= n && (n % 3) == 1) ? '=' : B64[(b >> 6) & 63];
            out += (i >= n && (n % 3) != 0) ? '=' : B64[b & 63];
        }
        // Fix: simpler padding
        out.clear();
        i = 0;
        while (i < n) {
            int rem = n - i;
            uint32_t b = static_cast<uint32_t>(data[static_cast<size_t>(i++)]) << 16;
            if (i < n) b |= static_cast<uint32_t>(data[static_cast<size_t>(i++)]) << 8;
            if (i < n) b |= static_cast<uint32_t>(data[static_cast<size_t>(i++)]);
            out += B64[(b >> 18) & 63];
            out += B64[(b >> 12) & 63];
            out += (rem > 1) ? B64[(b >> 6) & 63] : '=';
            out += (rem > 2) ? B64[b & 63] : '=';
        }
        return out;
    }

    static std::string computeAcceptKey(const std::string& clientKey) {
        std::string magic = clientKey + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
        auto sha = sha1Raw(magic);
        return base64Encode(sha);
    }

    // ── JSON helpers ──────────────────────────────────────────
    static std::string jsonString(const std::string& s) {
        std::string r = "\"";
        for (char c : s) {
            if      (c == '"')  r += "\\\"";
            else if (c == '\\') r += "\\\\";
            else if (c == '\n') r += "\\n";
            else if (c == '\r') r += "\\r";
            else if (c == '\t') r += "\\t";
            else                r += c;
        }
        return r + "\"";
    }

    static std::string extractJsonField(const std::string& json, const std::string& key) {
        std::string search = "\"" + key + "\"";
        size_t pos = json.find(search);
        if (pos == std::string::npos) return "";
        pos = json.find(':', pos + search.size());
        if (pos == std::string::npos) return "";
        ++pos;
        while (pos < json.size() && json[pos] == ' ') ++pos;
        if (pos >= json.size()) return "";
        if (json[pos] == '"') {
            ++pos;
            std::string val;
            while (pos < json.size() && json[pos] != '"') {
                if (json[pos] == '\\' && pos + 1 < json.size()) {
                    ++pos;
                    if      (json[pos] == 'n') val += '\n';
                    else if (json[pos] == 't') val += '\t';
                    else                       val += json[pos];
                } else {
                    val += json[pos];
                }
                ++pos;
            }
            return val;
        } else {
            size_t end = pos;
            while (end < json.size() &&
                   json[end] != ',' && json[end] != '}' && json[end] != ' ')
                ++end;
            return json.substr(pos, end - pos);
        }
    }

    static std::string buildChangeJson(const std::string& op,
                                        const std::string& table,
                                        const std::vector<std::string>& cols,
                                        const std::vector<std::string>& vals) {
        std::string j = "{\"type\":\"change\",\"op\":" + jsonString(op) +
                        ",\"table\":" + jsonString(table) + ",\"data\":{";
        for (size_t i = 0; i < cols.size() && i < vals.size(); ++i) {
            if (i > 0) j += ",";
            j += jsonString(cols[i]) + ":" + jsonString(vals[i]);
        }
        j += "}}";
        return j;
    }
};

// Note: g_wsServer() is forward-declared in dispatch.hpp.
// It is already defined there; no duplicate definition here.

// ── recv_all helper: loop until we have exactly n bytes ──────
static inline bool ws_recv_all(ws_sock_t sock, uint8_t* buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        int r = recv(sock, reinterpret_cast<char*>(buf + got),
                     static_cast<int>(n - got), 0);
        if (r <= 0) return false;
        got += static_cast<size_t>(r);
    }
    return true;
}

// ── Method implementations ────────────────────────────────────

inline void WebSocketServer::start() {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    listenSock_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listenSock_ == WS_INVALID_SOCK) return;

    int opt = 1;
    setsockopt(listenSock_, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&opt), sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(static_cast<uint16_t>(port_));

    if (bind(listenSock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ws_closesocket(listenSock_);
        listenSock_ = WS_INVALID_SOCK;
        return;
    }
    listen(listenSock_, SOMAXCONN);

    running_.store(true);
    std::thread(&WebSocketServer::listenerLoop, this).detach();
}

inline void WebSocketServer::stop() {
    running_.store(false);
    if (listenSock_ != WS_INVALID_SOCK) {
        ws_closesocket(listenSock_);
        listenSock_ = WS_INVALID_SOCK;
    }
}

inline void WebSocketServer::listenerLoop() {
    while (running_.load()) {
        sockaddr_in clientAddr{};
        socklen_t len = sizeof(clientAddr);
        ws_sock_t client = accept(listenSock_,
                                  reinterpret_cast<sockaddr*>(&clientAddr), &len);
        if (client == WS_INVALID_SOCK) break;
        std::thread(&WebSocketServer::handleClient, this, client).detach();
    }
}

inline void WebSocketServer::handleClient(ws_sock_t sock) {
    if (!performHandshake(sock)) {
        ws_closesocket(sock);
        return;
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        clients_.insert(sock);
    }
    while (running_.load()) {
        std::string frame = readFrame(sock);
        if (frame.empty()) break;
        handleMessage(sock, frame);
    }
    removeClient(sock);
    ws_closesocket(sock);
}

inline bool WebSocketServer::performHandshake(ws_sock_t sock) {
    // Read HTTP upgrade request
    std::string raw;
    char buf[4096];
    while (raw.find("\r\n\r\n") == std::string::npos) {
        int n = recv(sock, buf, static_cast<int>(sizeof(buf) - 1), 0);
        if (n <= 0) return false;
        buf[n] = '\0';
        raw += buf;
    }

    // Extract Sec-WebSocket-Key
    std::string keyHeader = "Sec-WebSocket-Key:";
    size_t keyPos = raw.find(keyHeader);
    if (keyPos == std::string::npos) {
        // case-insensitive check
        std::string rawLower = raw;
        for (char& c : rawLower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        keyHeader = "sec-websocket-key:";
        keyPos = rawLower.find(keyHeader);
        if (keyPos == std::string::npos) return false;
    }
    size_t start = keyPos + keyHeader.size();
    while (start < raw.size() && raw[start] == ' ') ++start;
    size_t end = raw.find("\r\n", start);
    if (end == std::string::npos) return false;
    std::string clientKey = raw.substr(start, end - start);
    // Trim trailing whitespace
    while (!clientKey.empty() && (clientKey.back() == ' ' || clientKey.back() == '\r'))
        clientKey.pop_back();

    std::string acceptKey = computeAcceptKey(clientKey);

    std::string response =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " + acceptKey + "\r\n"
        "\r\n";

    size_t sent = 0;
    while (sent < response.size()) {
        int n = send(sock, response.c_str() + sent,
                     static_cast<int>(response.size() - sent), 0);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

inline std::string WebSocketServer::readFrame(ws_sock_t sock) {
    uint8_t header[2];
    if (!ws_recv_all(sock, header, 2)) return "";

    uint8_t opcode     = header[0] & 0x0F;
    bool    masked     = (header[1] & 0x80) != 0;
    uint64_t payloadLen = header[1] & 0x7F;

    if (payloadLen == 126) {
        uint8_t ext[2];
        if (!ws_recv_all(sock, ext, 2)) return "";
        payloadLen = (static_cast<uint64_t>(ext[0]) << 8) | ext[1];
    } else if (payloadLen == 127) {
        uint8_t ext[8];
        if (!ws_recv_all(sock, ext, 8)) return "";
        payloadLen = 0;
        for (int i = 0; i < 8; i++)
            payloadLen = (payloadLen << 8) | ext[i];
    }

    if (opcode == 0x8) return ""; // Close frame

    uint8_t maskKey[4] = {0, 0, 0, 0};
    if (masked) {
        if (!ws_recv_all(sock, maskKey, 4)) return "";
    }

    // Safety limit: 64 KB
    if (payloadLen > 65536) return "";

    std::vector<uint8_t> payload(static_cast<size_t>(payloadLen));
    if (payloadLen > 0) {
        if (!ws_recv_all(sock, payload.data(), static_cast<size_t>(payloadLen))) return "";
        if (masked) {
            for (size_t i = 0; i < static_cast<size_t>(payloadLen); i++)
                payload[i] ^= maskKey[i % 4];
        }
    }

    if (opcode == 0x9) {
        // Ping → send Pong
        uint8_t pong[2] = {0x8A, 0x00};
        send(sock, reinterpret_cast<char*>(pong), 2, 0);
        return readFrame(sock);
    }

    return std::string(payload.begin(), payload.end());
}

inline void WebSocketServer::sendFrame(ws_sock_t sock, const std::string& payload) {
    std::vector<uint8_t> frame;
    frame.push_back(0x81); // FIN + text opcode

    size_t len = payload.size();
    if (len < 126) {
        frame.push_back(static_cast<uint8_t>(len));
    } else if (len < 65536) {
        frame.push_back(126);
        frame.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
        frame.push_back(static_cast<uint8_t>( len       & 0xFF));
    } else {
        frame.push_back(127);
        for (int i = 7; i >= 0; i--)
            frame.push_back(static_cast<uint8_t>((len >> (8 * i)) & 0xFF));
    }

    frame.insert(frame.end(), payload.begin(), payload.end());
    send(sock, reinterpret_cast<char*>(frame.data()),
         static_cast<int>(frame.size()), 0);
}

inline void WebSocketServer::sendClose(ws_sock_t sock) {
    uint8_t closeFrame[2] = {0x88, 0x00};
    send(sock, reinterpret_cast<char*>(closeFrame), 2, 0);
}

inline void WebSocketServer::handleMessage(ws_sock_t sock, const std::string& payload) {
    std::string type = extractJsonField(payload, "type");

    if (type == "query") {
        std::string sql = extractJsonField(payload, "sql");
        std::string result = executeQuery(sql);
        sendFrame(sock, result);
    } else if (type == "subscribe") {
        std::string table = extractJsonField(payload, "table");
        {
            std::lock_guard<std::mutex> lk(mu_);
            subscriptions_[sock].insert(table);
        }
        std::string resp = "{\"type\":\"subscribed\",\"table\":" + jsonString(table) + "}";
        sendFrame(sock, resp);
    } else if (type == "unsubscribe") {
        std::string table = extractJsonField(payload, "table");
        {
            std::lock_guard<std::mutex> lk(mu_);
            subscriptions_[sock].erase(table);
        }
    } else if (type == "listen") {
        std::string channel = extractJsonField(payload, "channel");
        std::string resp = "{\"type\":\"listening\",\"channel\":" + jsonString(channel) + "}";
        sendFrame(sock, resp);
    } else {
        std::string resp = "{\"type\":\"error\",\"message\":\"Unknown message type: " + type + "\"}";
        sendFrame(sock, resp);
    }
}

inline std::string WebSocketServer::executeQuery(const std::string& sql) const {
    std::ostringstream oss;
    std::streambuf* old = std::cout.rdbuf(oss.rdbuf());
    try {
        Parser p;
        auto cmd = p.parse(sql);
        auto noop = [](){};
        dispatchCommand(cmd, engine_, p, sql, noop, noop, noop);
    } catch (const std::exception& e) {
        std::cout.rdbuf(old);
        return "{\"type\":\"error\",\"message\":" + jsonString(e.what()) + "}";
    }
    std::cout.rdbuf(old);
    std::string output = oss.str();
    return "{\"type\":\"result\",\"sql\":" + jsonString(sql) +
           ",\"output\":" + jsonString(output) + "}";
}

inline void WebSocketServer::notifyTableChange(const std::string& tableName,
                                                const std::string& op,
                                                const std::vector<std::string>& colNames,
                                                const std::vector<std::string>& values) {
    std::string msg = buildChangeJson(op, tableName, colNames, values);
    std::lock_guard<std::mutex> lk(mu_);
    for (ws_sock_t sock : clients_) {
        auto it = subscriptions_.find(sock);
        if (it != subscriptions_.end() &&
            (it->second.count(tableName) > 0 || it->second.count("*") > 0)) {
            sendFrame(sock, msg);
        }
    }
}

inline void WebSocketServer::removeClient(ws_sock_t sock) {
    std::lock_guard<std::mutex> lk(mu_);
    clients_.erase(sock);
    subscriptions_.erase(sock);
}

} // namespace milansql
