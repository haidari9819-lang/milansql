#pragma once
// ============================================================
// http_fdw.hpp — HTTP/JSON Foreign Data Wrapper
// Phase 89: Foreign Data Wrapper
// Platform-agnostic HTTP GET (Windows Winsock2 + POSIX)
// ============================================================

#include "foreign_data_wrapper.hpp"
#include <string>
#include <vector>
#include <map>
#include <algorithm>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
  #define NOMINMAX
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  // Undefine Windows macros that clash with C++ / MilanSQL identifiers
  #ifdef DELETE
  #undef DELETE
  #endif
  #ifdef IN
  #undef IN
  #endif
  #ifdef OUT
  #undef OUT
  #endif
  #ifdef BOOL
  #undef BOOL
  #endif
  #ifdef ERROR
  #undef ERROR
  #endif
  #ifdef OPTIONAL
  #undef OPTIONAL
  #endif
#else
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <netdb.h>
  #include <unistd.h>
  #include <arpa/inet.h>
#endif

namespace milansql {

class HttpFdw : public ForeignDataWrapper {
public:
    std::vector<std::vector<std::string>> scan(
        const ForeignTableDef& tbl,
        const std::vector<std::string>& /*colNames*/) override
    {
        std::string url = tbl.options.count("url") ? tbl.options.at("url") : "";
        if (url.empty()) return {};
        std::string body = httpGet(url);
        return parseJsonArray(body, tbl.colNames);
    }

private:
    // Parse host, path, port from URL like http://host:port/path
    static void parseUrl(const std::string& url,
                         std::string& host,
                         std::string& path,
                         int& port)
    {
        port = 80;
        std::string u = url;
        if (u.size() >= 7 && u.substr(0, 7) == "http://") u = u.substr(7);
        size_t slash = u.find('/');
        std::string hostport = (slash != std::string::npos) ? u.substr(0, slash) : u;
        path = (slash != std::string::npos) ? u.substr(slash) : "/";
        size_t colon = hostport.find(':');
        if (colon != std::string::npos) {
            host = hostport.substr(0, colon);
            try { port = std::stoi(hostport.substr(colon + 1)); } catch (...) { port = 80; }
        } else {
            host = hostport;
        }
    }

    // Block SSRF to private/internal networks
    static bool isPrivateHost(const std::string& host) {
        if (host == "localhost" || host == "127.0.0.1" || host == "::1") return true;
        if (host.substr(0, 3) == "10.") return true;
        if (host.substr(0, 8) == "192.168.") return true;
        if (host.substr(0, 4) == "172.") {
            try { int oct2 = std::stoi(host.substr(4)); if (oct2 >= 16 && oct2 <= 31) return true; } catch (...) {}
        }
        if (host.substr(0, 8) == "169.254.") return true;  // link-local / cloud metadata
        if (host == "metadata.google.internal") return true;
        return false;
    }

    static std::string httpGet(const std::string& url) {
        std::string host, path;
        int port = 80;
        parseUrl(url, host, path, port);
        if (isPrivateHost(host)) return "{\"error\":\"SSRF blocked: private network access denied\"}";

#ifdef _WIN32
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
        struct addrinfo hints{};
        struct addrinfo* res = nullptr;
        hints.ai_family   = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0)
            return "";

#ifdef _WIN32
        SOCKET sock = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (sock == INVALID_SOCKET) { freeaddrinfo(res); return ""; }
        if (::connect(sock, res->ai_addr, static_cast<int>(res->ai_addrlen)) != 0) {
            closesocket(sock); freeaddrinfo(res); return "";
        }
#else
        int sock = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (sock < 0) { freeaddrinfo(res); return ""; }
        if (::connect(sock, res->ai_addr, static_cast<socklen_t>(res->ai_addrlen)) != 0) {
            ::close(sock); freeaddrinfo(res); return "";
        }
#endif
        freeaddrinfo(res);

        std::string req = "GET " + path + " HTTP/1.0\r\nHost: " + host
                        + "\r\nConnection: close\r\n\r\n";
#ifdef _WIN32
        ::send(sock, req.c_str(), static_cast<int>(req.size()), 0);
        std::string resp;
        char buf[4096];
        int n = 0;
        while ((n = ::recv(sock, buf, static_cast<int>(sizeof(buf) - 1), 0)) > 0) {
            buf[n] = '\0'; resp += buf;
        }
        closesocket(sock);
#else
        ::send(sock, req.c_str(), req.size(), 0);
        std::string resp;
        char buf[4096];
        ssize_t n = 0;
        while ((n = ::recv(sock, buf, sizeof(buf) - 1, 0)) > 0) {
            buf[n] = '\0'; resp += buf;
        }
        ::close(sock);
#endif

        // Strip HTTP headers
        size_t hend = resp.find("\r\n\r\n");
        if (hend == std::string::npos) hend = resp.find("\n\n");
        return (hend != std::string::npos) ? resp.substr(hend + 4) : resp;
    }

    // Parse JSON array: [{"col":"val",...},...]
    static std::vector<std::vector<std::string>> parseJsonArray(
        const std::string& json,
        const std::vector<std::string>& cols)
    {
        std::vector<std::vector<std::string>> result;
        size_t start = json.find('[');
        size_t end   = json.rfind(']');
        if (start == std::string::npos || end == std::string::npos) return result;

        std::string body = json.substr(start + 1, end - start - 1);

        // Split into objects {...}
        std::vector<std::string> objects;
        int depth = 0;
        std::string cur;
        for (char c : body) {
            if (c == '{') { depth++; cur += c; }
            else if (c == '}') {
                cur += c;
                depth--;
                if (depth == 0) { objects.push_back(cur); cur.clear(); }
            } else if (depth > 0) {
                cur += c;
            }
        }

        for (const auto& obj : objects) {
            std::map<std::string, std::string> kv = parseJsonObject(obj);
            std::vector<std::string> row;
            for (const auto& cn : cols) {
                std::string lc = cn;
                for (auto& ch : lc) ch = static_cast<char>(
                    std::tolower(static_cast<unsigned char>(ch)));
                if (kv.count(cn))
                    row.push_back(kv.at(cn));
                else if (kv.count(lc))
                    row.push_back(kv.at(lc));
                else
                    row.push_back("NULL");
            }
            result.push_back(std::move(row));
        }
        return result;
    }

    static std::map<std::string, std::string> parseJsonObject(const std::string& obj) {
        std::map<std::string, std::string> m;
        size_t i = 0;
        while (i < obj.size()) {
            // find opening quote for key
            size_t ks = obj.find('"', i);
            if (ks == std::string::npos) break;
            size_t ke = obj.find('"', ks + 1);
            if (ke == std::string::npos) break;
            std::string key = obj.substr(ks + 1, ke - ks - 1);
            // find colon
            size_t colon = obj.find(':', ke + 1);
            if (colon == std::string::npos) break;
            i = colon + 1;
            // skip whitespace
            while (i < obj.size() && obj[i] == ' ') ++i;
            std::string val;
            if (i < obj.size() && obj[i] == '"') {
                // string value
                size_t vs = i + 1;
                size_t ve = obj.find('"', vs);
                if (ve == std::string::npos) break;
                val = obj.substr(vs, ve - vs);
                i = ve + 1;
            } else {
                // number / bool / null
                size_t ve = i;
                while (ve < obj.size() && obj[ve] != ',' && obj[ve] != '}') ++ve;
                val = obj.substr(i, ve - i);
                while (!val.empty() && val.back() == ' ') val.pop_back();
                i = ve;
            }
            m[key] = val;
        }
        return m;
    }
};

} // namespace milansql
