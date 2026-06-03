#pragma once
// ============================================================
// connection_string.hpp — Phase 79: DSN / Connection String Parser
// Phase 129: Connection String V2 + Service Discovery
// Supported formats:
//   milansql://user:pass@host:port/database
//   milansql://user:pass@host1:port,host2:port/database?ssl=true&pool_size=5
//   milansql+srv://user@milansql.local/mydb
//   mysql://user@host:port/database
//   jdbc:milansql://host:port/database
// ============================================================

#include <string>
#include <stdexcept>
#include <cctype>
#include <map>
#include <vector>
#include <sstream>

namespace milansql {

struct ConnectionString {
    std::string protocol  = "milansql";
    std::string user      = "root";
    std::string password;
    std::string host      = "localhost";
    int         port      = 4406;
    std::string database  = "public";

    // Returns true if str looks like a supported DSN
    static bool isDsn(const std::string& str) {
        if (str.size() >= 11 && str.substr(0, 11) == "milansql://") return true;
        if (str.size() >=  8 && str.substr(0,  8) == "mysql://")    return true;
        if (str.size() >= 16 && str.substr(0, 16) == "jdbc:milansql://") return true;
        return false;
    }

    // Parse a DSN string into a ConnectionString.
    // Throws std::invalid_argument on invalid format.
    static ConnectionString parse(const std::string& raw) {
        ConnectionString cs;
        std::string str = raw;

        // Strip optional "jdbc:" prefix
        if (str.size() >= 5 && str.substr(0, 5) == "jdbc:") {
            str = str.substr(5);
        }

        // Extract protocol (everything before "://")
        auto schemeEnd = str.find("://");
        if (schemeEnd == std::string::npos)
            throw std::invalid_argument("Ungueltige DSN: '://' fehlt");
        cs.protocol = str.substr(0, schemeEnd);
        str = str.substr(schemeEnd + 3);

        // Normalize protocol to lowercase
        for (auto& c : cs.protocol)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (cs.protocol != "milansql" && cs.protocol != "mysql")
            throw std::invalid_argument("Unbekanntes Protokoll: " + cs.protocol);

        // Default port: mysql → 3306, milansql → 4406
        cs.port = (cs.protocol == "mysql") ? 3306 : 4406;

        // Split user info at last '@'
        auto atPos = str.rfind('@');
        if (atPos != std::string::npos) {
            std::string userinfo = str.substr(0, atPos);
            str = str.substr(atPos + 1);

            // Split userinfo at first ':'
            auto colonPos = userinfo.find(':');
            if (colonPos != std::string::npos) {
                cs.user     = userinfo.substr(0, colonPos);
                cs.password = userinfo.substr(colonPos + 1);
            } else {
                cs.user = userinfo;
            }
        }

        // Strip query params before parsing host/db
        auto qPos = str.find('?');
        if (qPos != std::string::npos) {
            str = str.substr(0, qPos);
        }

        // Split remaining "host:port/database"
        auto slashPos = str.find('/');
        std::string hostport;
        if (slashPos != std::string::npos) {
            hostport    = str.substr(0, slashPos);
            cs.database = str.substr(slashPos + 1);
            if (cs.database.empty()) cs.database = "public";
        } else {
            hostport = str;
        }

        // Split "host:port"
        auto colonPos = hostport.find(':');
        if (colonPos != std::string::npos) {
            cs.host = hostport.substr(0, colonPos);
            try {
                cs.port = std::stoi(hostport.substr(colonPos + 1));
            } catch (...) {
                throw std::invalid_argument("Ungueltiger Port in DSN");
            }
        } else if (!hostport.empty()) {
            cs.host = hostport;
        }

        return cs;
    }

    // Throw if configuration is invalid
    void validate() const {
        if (host.empty())
            throw std::invalid_argument("DSN: Host darf nicht leer sein");
        if (port <= 0 || port > 65535)
            throw std::invalid_argument("DSN: Port ausserhalb des gueltigen Bereichs");
        if (user.empty())
            throw std::invalid_argument("DSN: User darf nicht leer sein");
    }

    // "milansql://user@host:port/db"  (password omitted)
    std::string toDisplayString() const {
        return protocol + "://" + user + "@" + host + ":" + std::to_string(port) + "/" + database;
    }
};

// ============================================================
// Phase 129: ConnectionStringParser — V2 with multi-host,
// query params, URL decoding, and SRV service discovery
// ============================================================

struct ConnectionStringParser {

    // URL decode: %20 → space, %40 → @, etc.
    static std::string urlDecode(const std::string& s) {
        std::string result;
        for (size_t i = 0; i < s.size(); i++) {
            if (s[i] == '%' && i+2 < s.size()) {
                int val = 0;
                std::istringstream hex(s.substr(i+1, 2));
                hex >> std::hex >> val;
                result += static_cast<char>(val);
                i += 2;
            } else if (s[i] == '+') {
                result += ' ';
            } else {
                result += s[i];
            }
        }
        return result;
    }

    // Parse query parameters: "ssl=true&timeout=30&pool_size=5"
    static std::map<std::string, std::string> parseParams(const std::string& queryString) {
        std::map<std::string, std::string> params;
        std::istringstream iss(queryString);
        std::string pair;
        while (std::getline(iss, pair, '&')) {
            auto eq = pair.find('=');
            if (eq != std::string::npos) {
                std::string key = urlDecode(pair.substr(0, eq));
                std::string val = urlDecode(pair.substr(eq + 1));
                params[key] = val;
            }
        }
        return params;
    }

    // Parse multi-host: "host1:4406,host2:4407"
    static std::vector<std::pair<std::string,int>> parseHosts(const std::string& hostStr) {
        std::vector<std::pair<std::string,int>> hosts;
        std::istringstream iss(hostStr);
        std::string part;
        while (std::getline(iss, part, ',')) {
            auto colon = part.rfind(':');
            if (colon != std::string::npos) {
                std::string h = part.substr(0, colon);
                int p = 4406;
                try { p = std::stoi(part.substr(colon+1)); } catch(...) {}
                hosts.push_back({h, p});
            } else if (!part.empty()) {
                hosts.push_back({part, 4406});
            }
        }
        return hosts;
    }

    struct ParsedConnectionString {
        std::string scheme;      // "milansql", "milansql+srv"
        std::string user;
        std::string password;
        std::vector<std::pair<std::string,int>> hosts;
        std::string database;
        std::map<std::string, std::string> params;
        bool srvLookup = false;
        bool valid = false;
        std::string error;

        // Helper getters
        bool ssl()        const { auto it=params.find("ssl");       return it!=params.end() && it->second=="true"; }
        int timeout()     const { auto it=params.find("timeout");   return it!=params.end() ? std::stoi(it->second) : 30; }
        std::string routing() const { auto it=params.find("routing"); return it!=params.end() ? it->second : "auto"; }
        int poolSize()    const { auto it=params.find("pool_size"); return it!=params.end() ? std::stoi(it->second) : 10; }
        int retry()       const { auto it=params.find("retry");     return it!=params.end() ? std::stoi(it->second) : 3; }
        std::string tenant() const { auto it=params.find("tenant"); return it!=params.end() ? it->second : ""; }
        bool compress()   const { auto it=params.find("compress");  return it!=params.end() && it->second=="true"; }

        // Primary host
        std::string host() const { return hosts.empty() ? "localhost" : hosts[0].first; }
        int port()         const { return hosts.empty() ? 4406 : hosts[0].second; }

        std::string toDSN() const {
            std::string dsn = scheme + "://" + user;
            if (!password.empty()) dsn += ":" + password;
            if (!user.empty() || !password.empty()) dsn += "@";
            for (size_t i = 0; i < hosts.size(); i++) {
                if (i > 0) dsn += ",";
                dsn += hosts[i].first + ":" + std::to_string(hosts[i].second);
            }
            dsn += "/" + database;
            bool first = true;
            for (auto& [k, v] : params) {
                dsn += (first ? "?" : "&") + k + "=" + v;
                first = false;
            }
            return dsn;
        }
    };

    // Parse full connection string: milansql://user:pass@host1:port,host2:port/db?params
    static ParsedConnectionString parse(const std::string& connStr) {
        ParsedConnectionString result;
        result.valid = false;

        // Find scheme
        auto schemeSep = connStr.find("://");
        if (schemeSep == std::string::npos) { result.error = "Missing ://"; return result; }
        result.scheme = connStr.substr(0, schemeSep);
        result.srvLookup = (result.scheme == "milansql+srv");

        std::string rest = connStr.substr(schemeSep + 3);

        // Split on @ for user info
        auto atPos = rest.find('@');
        if (atPos != std::string::npos) {
            std::string userInfo = rest.substr(0, atPos);
            rest = rest.substr(atPos + 1);
            auto colon = userInfo.find(':');
            if (colon != std::string::npos) {
                result.user     = urlDecode(userInfo.substr(0, colon));
                result.password = urlDecode(userInfo.substr(colon + 1));
            } else {
                result.user = urlDecode(userInfo);
            }
        }

        // Split on ? for params
        auto qPos = rest.find('?');
        if (qPos != std::string::npos) {
            result.params = parseParams(rest.substr(qPos + 1));
            rest = rest.substr(0, qPos);
        }

        // Split on / for database
        auto slashPos = rest.find('/');
        std::string hostPart;
        if (slashPos != std::string::npos) {
            hostPart        = rest.substr(0, slashPos);
            result.database = rest.substr(slashPos + 1);
        } else {
            hostPart = rest;
        }

        result.hosts = parseHosts(hostPart);
        result.valid = true;
        return result;
    }
};

} // namespace milansql
