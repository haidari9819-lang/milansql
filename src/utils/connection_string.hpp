#pragma once
// ============================================================
// connection_string.hpp — Phase 79: DSN / Connection String Parser
// Supported formats:
//   milansql://user:pass@host:port/database
//   mysql://user@host:port/database
//   jdbc:milansql://host:port/database
// ============================================================

#include <string>
#include <stdexcept>
#include <cctype>

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

} // namespace milansql
