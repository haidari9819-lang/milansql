#pragma once
// ============================================================
// fortress.hpp — MilanSQL Security Fortress
// Schicht 1: Honeypot + Aktive Täuschung
// Schicht 2: Intelligentes Rate Limiting + Subnet Blocking
// Schicht 3: Query Analyse + Anomalie-Erkennung
// Schicht 4: Canary Tokens
// Schicht 5: Hardening Utilities
// ============================================================

#include <string>
#include <vector>
#include <map>
#include <set>
#include <mutex>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <algorithm>
#include <fstream>
#include <atomic>
#include <random>
#include <sstream>
#include <functional>
#include <cctype>
#include "../utils/date_utils.hpp"

namespace milansql {

// ── Schicht 1: Honeypot / Aktive Täuschung ──────────────────

// Honeypot endpoints that trigger immediate IP bans
static const std::vector<std::string> HONEYPOT_PATHS = {
    "/admin", "/phpmyadmin", "/wp-admin", "/wp-login.php",
    "/.env", "/config.php", "/.git/config", "/.git/HEAD",
    "/xmlrpc.php", "/wp-content", "/wp-includes",
    "/administrator", "/server-status", "/server-info",
    "/.htaccess", "/.htpasswd", "/backup.sql", "/dump.sql",
    "/db.sql", "/database.sql", "/phpinfo.php",
    "/cgi-bin/", "/shell", "/cmd", "/eval",
    "/actuator", "/api/debug", "/debug/vars",
    "/solr/admin", "/manager/html", "/jenkins",
    "/grafana", "/kibana", "/elasticsearch",
    "/.well-known/security.txt"  // except real one
};

// Honeypot table names — access = immediate ban
static const std::vector<std::string> HONEYPOT_TABLES = {
    "_admin_secrets", "_password_backup", "_user_tokens_dump",
    "_system_credentials", "_root_access", "_cc_numbers",
    "_payment_data_raw", "_ssn_records"
};

// Fake SQL error messages that look realistic but reveal nothing
static const std::vector<std::string> FAKE_SQL_ERRORS = {
    "ERROR 1045 (28000): Access denied for user 'admin'@'%' (using password: YES)",
    "ERROR 1146 (42S02): Table 'information_schema.TABLES' doesn't exist",
    "FATAL: password authentication failed for user \"postgres\"",
    "ERROR: relation \"pg_shadow\" does not exist",
    "ERROR 1044 (42000): Access denied for user 'root'@'%' to database 'mysql'",
    "FATAL: no pg_hba.conf entry for host, user, database",
    "ERROR 2003 (HY000): Can't connect to MySQL server on 'localhost'",
    "psql: error: connection refused. Is the server running?"
};

// ── Schicht 2: Intelligentes Rate Limiting ──────────────────

struct IpThreatInfo {
    int      failCount      = 0;
    int      honeypotHits   = 0;
    int      sqliAttempts   = 0;
    int      bruteForceHits = 0;
    double   delaySeconds   = 0.0;
    bool     permanentBan   = false;
    std::chrono::steady_clock::time_point banUntil{};
    std::chrono::steady_clock::time_point lastSeen{};
    std::vector<std::string> recentPaths;  // last 10 paths
};

struct SubnetInfo {
    int      flaggedIps = 0;
    bool     blocked    = false;
    std::chrono::steady_clock::time_point blockedUntil{};
};

// ── Schicht 4: Canary Tokens ────────────────────────────────

struct CanaryToken {
    std::string token;
    std::string type;   // "api_key", "password", "bearer"
    std::string context; // where it was planted
};

// ══════════════════════════════════════════════════════════════
// FortressEngine — Central security coordinator
// ══════════════════════════════════════════════════════════════

class FortressEngine {
public:
    FortressEngine() {
        generateCanaryTokens();
        // Default whitelist: localhost + common internal ranges
        whitelist_.insert("127.0.0.1");
        whitelist_.insert("::1");
    }

    // ── Whitelist Management ─────────────────────────────────

    void addWhitelist(const std::string& ip) {
        std::lock_guard<std::mutex> lk(mu_);
        whitelist_.insert(ip);
        // Remove any existing bans for this IP
        threats_.erase(ip);
    }

    void removeWhitelist(const std::string& ip) {
        std::lock_guard<std::mutex> lk(mu_);
        whitelist_.erase(ip);
    }

    bool isWhitelisted(const std::string& ip) const {
        std::lock_guard<std::mutex> lk(mu_);
        return whitelist_.count(ip) > 0;
    }

    // Remove all bans for a specific IP
    void unban(const std::string& ip) {
        std::lock_guard<std::mutex> lk(mu_);
        threats_.erase(ip);
    }

    // Remove ALL bans (emergency reset)
    void unbanAll() {
        std::lock_guard<std::mutex> lk(mu_);
        threats_.clear();
        subnets_.clear();
    }

    // Load whitelist from file (one IP per line)
    void loadWhitelist(const std::string& path) {
        std::ifstream f(path);
        if (!f) return;
        std::lock_guard<std::mutex> lk(mu_);
        std::string line;
        while (std::getline(f, line)) {
            if (!line.empty() && line[0] != '#')
                whitelist_.insert(line);
        }
    }

    // Save whitelist to file
    void saveWhitelist(const std::string& path) const {
        std::lock_guard<std::mutex> lk(mu_);
        std::ofstream f(path);
        if (!f) return;
        f << "# MilanSQL Fortress Whitelist\n";
        for (const auto& ip : whitelist_)
            f << ip << "\n";
    }

    // ── Schicht 1: Honeypot Check ────────────────────────────

    bool isHoneypotPath(const std::string& path) const {
        std::string lower = toLower(path);
        for (const auto& hp : HONEYPOT_PATHS) {
            if (lower == hp || lower.find(hp) == 0) return true;
        }
        // Common scanner patterns
        if (lower.find(".php") != std::string::npos && lower != "/index.php") return true;
        if (lower.find("..") != std::string::npos) return true;
        if (lower.find("\\") != std::string::npos) return true;
        return false;
    }

    bool isHoneypotTable(const std::string& tableName) const {
        std::string lower = toLower(tableName);
        for (const auto& ht : HONEYPOT_TABLES) {
            if (lower == ht) return true;
        }
        return false;
    }

    std::string getFakeSqlError() const {
        // Deterministic but looks random
        size_t idx = static_cast<size_t>(
            std::chrono::steady_clock::now().time_since_epoch().count()
        ) % FAKE_SQL_ERRORS.size();
        return FAKE_SQL_ERRORS[idx];
    }

    // Returns a fake login response with canary credentials
    std::string getHoneypotLoginResponse() const {
        return "{\"success\":true,\"token\":\"" + canaryTokens_[0].token +
               "\",\"user_id\":1,\"username\":\"admin\","
               "\"message\":\"Login successful\","
               "\"api_key\":\"" + canaryTokens_[1].token + "\"}";
    }

    // ── Schicht 2: Intelligent Rate Limiting ─────────────────

    // Record a failed attempt. Returns action to take.
    enum class ThreatAction {
        ALLOW,           // No issue
        DELAY,           // Add progressive delay
        BLOCK_1H,        // Block for 1 hour
        BLOCK_24H,       // Block for 24 hours
        BLOCK_PERMANENT, // Permanent ban
        BLOCK_SUBNET     // Block entire /24 subnet
    };

    ThreatAction recordFailure(const std::string& ip, const std::string& reason) {
        std::lock_guard<std::mutex> lk(mu_);
        if (whitelist_.count(ip) > 0) return ThreatAction::ALLOW;
        auto& info = threats_[ip];
        info.failCount++;
        info.lastSeen = std::chrono::steady_clock::now();

        logThreat(ip, reason, info.failCount);

        // Progressive delay: 1s, 2s, 4s, 8s, 16s...
        if (info.failCount <= 4) {
            info.delaySeconds = static_cast<double>(1 << (info.failCount - 1));
            return ThreatAction::DELAY;
        }

        // After 5 failures: 1 hour block
        if (info.failCount >= 5 && info.failCount < 10) {
            info.banUntil = std::chrono::steady_clock::now() + std::chrono::hours(1);
            checkSubnet(ip);
            return ThreatAction::BLOCK_1H;
        }

        // After 10 failures: permanent block + alert
        if (info.failCount >= 10) {
            info.permanentBan = true;
            logAlert(ip, "PERMANENT BAN after " + std::to_string(info.failCount) + " failures: " + reason);
            checkSubnet(ip);
            return ThreatAction::BLOCK_PERMANENT;
        }

        return ThreatAction::DELAY;
    }

    // Record honeypot hit — immediate 24h ban
    ThreatAction recordHoneypotHit(const std::string& ip, const std::string& path) {
        std::lock_guard<std::mutex> lk(mu_);
        // Log but never ban whitelisted IPs
        if (whitelist_.count(ip) > 0) {
            logThreat(ip, "HONEYPOT (whitelisted, no ban): " + path, 0);
            return ThreatAction::ALLOW;
        }
        auto& info = threats_[ip];
        info.honeypotHits++;
        info.failCount += 5;  // Honeypot = 5 strikes at once
        info.banUntil = std::chrono::steady_clock::now() + std::chrono::hours(24);
        info.lastSeen = std::chrono::steady_clock::now();

        logThreat(ip, "HONEYPOT: " + path, info.failCount);
        logAlert(ip, "Honeypot triggered: " + path);
        checkSubnet(ip);

        if (info.honeypotHits >= 3) {
            info.permanentBan = true;
            return ThreatAction::BLOCK_PERMANENT;
        }
        return ThreatAction::BLOCK_24H;
    }

    // Record SQL injection attempt — immediate block
    ThreatAction recordSqliAttempt(const std::string& ip, const std::string& pattern) {
        std::lock_guard<std::mutex> lk(mu_);
        if (whitelist_.count(ip) > 0) {
            logThreat(ip, "SQLI (whitelisted, no ban): " + pattern, 0);
            return ThreatAction::ALLOW;
        }
        auto& info = threats_[ip];
        info.sqliAttempts++;
        info.failCount += 3;

        logThreat(ip, "SQLI: " + pattern, info.failCount);
        logAlert(ip, "SQL Injection attempt: " + pattern);

        if (info.sqliAttempts >= 2) {
            info.permanentBan = true;
            return ThreatAction::BLOCK_PERMANENT;
        }
        info.banUntil = std::chrono::steady_clock::now() + std::chrono::hours(1);
        checkSubnet(ip);
        return ThreatAction::BLOCK_1H;
    }

    // Check if an IP is currently blocked
    bool isBlocked(const std::string& ip) const {
        if (ip.empty()) return false;

        std::lock_guard<std::mutex> lk(mu_);

        // Never block whitelisted IPs
        if (whitelist_.count(ip) > 0) return false;

        // Check direct IP ban
        auto it = threats_.find(ip);
        if (it != threats_.end()) {
            if (it->second.permanentBan) return true;
            auto now = std::chrono::steady_clock::now();
            if (now < it->second.banUntil) return true;
        }

        // Check subnet ban
        std::string subnet = getSubnet(ip);
        auto sit = subnets_.find(subnet);
        if (sit != subnets_.end() && sit->second.blocked) {
            auto now = std::chrono::steady_clock::now();
            if (now < sit->second.blockedUntil) return true;
        }

        return false;
    }

    // Get progressive delay for an IP (seconds)
    double getDelay(const std::string& ip) const {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = threats_.find(ip);
        if (it == threats_.end()) return 0.0;
        return it->second.delaySeconds;
    }

    // ── Schicht 3: Query Analyse / Anomalie-Erkennung ────────

    struct SqliResult {
        bool    detected = false;
        std::string pattern;
        int     severity = 0;  // 1=low, 2=medium, 3=high
    };

    SqliResult analyzeQuery(const std::string& sql) const {
        std::string upper = toUpper(sql);

        // High severity: direct attack patterns
        static const std::vector<std::pair<std::string, std::string>> HIGH_PATTERNS = {
            {"UNION SELECT",        "UNION-based injection"},
            {"UNION ALL SELECT",    "UNION-based injection"},
            {"' OR '1'='1",         "Classic OR injection"},
            {"' OR 1=1",            "Classic OR injection"},
            {"'; DROP",             "Piggyback DROP"},
            {"'; DELETE",           "Piggyback DELETE"},
            {"'; TRUNCATE",         "Piggyback TRUNCATE"},
            {"'; UPDATE",           "Piggyback UPDATE"},
            {"'; INSERT",           "Piggyback INSERT"},
            {"SLEEP(",              "Time-based injection"},
            {"BENCHMARK(",          "Time-based injection"},
            {"WAITFOR DELAY",       "Time-based injection"},
            {"PG_SLEEP(",           "Time-based injection"},
            {"DBMS_LOCK.SLEEP",     "Time-based injection"},
            {"LOAD_FILE(",          "File read injection"},
            {"INTO OUTFILE",        "File write injection"},
            {"INTO DUMPFILE",       "File write injection"},
            {"UTL_HTTP",            "SSRF via SQL"},
            {"XP_CMDSHELL",         "OS command injection"},
            {"EXEC MASTER",         "Privilege escalation"},
            {"0x",                  "Hex-encoded injection"},
        };

        // Medium severity: reconnaissance patterns
        static const std::vector<std::pair<std::string, std::string>> MEDIUM_PATTERNS = {
            {"INFORMATION_SCHEMA",  "Schema enumeration"},
            {"PG_CATALOG",          "PostgreSQL catalog probe"},
            {"PG_SHADOW",           "Password hash extraction"},
            {"MYSQL.USER",          "MySQL user table probe"},
            {"SYS.SYSLOGINS",       "MSSQL login probe"},
            {"SYSOBJECTS",          "MSSQL object enum"},
            {"ALL_USERS",           "Oracle user enum"},
            {"V$SESSION",           "Oracle session probe"},
            {"@@VERSION",           "Version fingerprinting"},
            {"VERSION()",           "Version fingerprinting"},
            {"USER()",              "User enumeration"},
            {"CURRENT_USER",        "User enumeration"},
            {"DATABASE()",          "Database enumeration"},
            {"SCHEMA()",            "Schema enumeration"},
        };

        // Low severity: suspicious but may be legitimate
        static const std::vector<std::pair<std::string, std::string>> LOW_PATTERNS = {
            {"CHAR(",               "Character encoding evasion"},
            {"CONCAT(",             "String concatenation (possible injection)"},
            {"GROUP_CONCAT(",       "Data exfiltration"},
            {"EXTRACTVALUE(",       "XML injection"},
            {"UPDATEXML(",          "XML injection"},
        };

        // Check comment-based injection (-- or /* */)
        if (sql.find("--") != std::string::npos && sql.find("'") != std::string::npos) {
            return {true, "Comment-based injection", 3};
        }
        if (sql.find("/*") != std::string::npos && sql.find("*/") != std::string::npos) {
            // Allow /* */ only if it looks like a legitimate hint
            size_t pos = sql.find("/*");
            if (pos > 0 && sql[pos-1] != ' ') {
                return {true, "Inline comment injection", 3};
            }
        }

        // Stacked queries (multiple ;-separated statements with dangerous verbs)
        {
            int semiCount = 0;
            for (char c : sql) if (c == ';') semiCount++;
            if (semiCount > 1) {
                // Allow legitimate multi-statement only for benign ops
                bool hasDangerous = false;
                for (const auto& [pat, desc] : HIGH_PATTERNS) {
                    if (upper.find(pat) != std::string::npos) {
                        hasDangerous = true;
                        break;
                    }
                }
                if (hasDangerous) return {true, "Stacked query attack", 3};
            }
        }

        for (const auto& [pat, desc] : HIGH_PATTERNS) {
            if (upper.find(pat) != std::string::npos) {
                return {true, desc, 3};
            }
        }
        for (const auto& [pat, desc] : MEDIUM_PATTERNS) {
            if (upper.find(pat) != std::string::npos) {
                return {true, desc, 2};
            }
        }
        // Low patterns only flagged if combined with suspicious chars
        bool hasSuspiciousChars = (sql.find("'") != std::string::npos &&
                                   sql.find("=") != std::string::npos) ||
                                  (sql.find("\"") != std::string::npos &&
                                   sql.find("=") != std::string::npos);
        if (hasSuspiciousChars) {
            for (const auto& [pat, desc] : LOW_PATTERNS) {
                if (upper.find(pat) != std::string::npos) {
                    return {true, desc, 1};
                }
            }
        }

        return {false, "", 0};
    }

    // Check for honeypot table access in SQL
    bool checkHoneypotTableAccess(const std::string& sql) const {
        std::string lower = toLower(sql);
        for (const auto& ht : HONEYPOT_TABLES) {
            if (lower.find(ht) != std::string::npos) return true;
        }
        return false;
    }

    // ── Schicht 4: Canary Token Check ────────────────────────

    bool isCanaryToken(const std::string& token) const {
        std::lock_guard<std::mutex> lk(mu_);
        for (const auto& ct : canaryTokens_) {
            if (ct.token == token) return true;
        }
        return false;
    }

    // Check if credentials match canary creds
    bool isCanaryCredential(const std::string& username, const std::string& password) const {
        (void)password;
        // Canary usernames
        static const std::vector<std::string> CANARY_USERS = {
            "admin", "administrator", "sa", "dba",
            "postgres", "mysql", "oracle", "test"
        };
        std::string lower = toLower(username);
        for (const auto& cu : CANARY_USERS) {
            if (lower == cu) return true;
        }
        return false;
    }

    // ── Schicht 5: Timing-safe response ──────────────────────

    // Pad response time to fixed duration to prevent timing attacks
    static void padResponseTime(std::chrono::steady_clock::time_point start,
                                std::chrono::milliseconds targetDuration = std::chrono::milliseconds(100)) {
        auto elapsed = std::chrono::steady_clock::now() - start;
        if (elapsed < targetDuration) {
            std::this_thread::sleep_for(targetDuration - elapsed);
        }
    }

    // ── Security Headers ─────────────────────────────────────

    static std::string getSecurityHeaders() {
        return "X-Frame-Options: DENY\r\n"
               "X-Content-Type-Options: nosniff\r\n"
               "X-XSS-Protection: 1; mode=block\r\n"
               "Referrer-Policy: strict-origin-when-cross-origin\r\n"
               "Content-Security-Policy: default-src 'self'; script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline'\r\n"
               "Strict-Transport-Security: max-age=63072000; includeSubDomains; preload\r\n"
               "Permissions-Policy: geolocation=(), camera=(), microphone=(), usb=()\r\n"
               "Cache-Control: no-store, no-cache, must-revalidate\r\n"
               "Pragma: no-cache\r\n";
    }

    // ── Stats & Logging ──────────────────────────────────────

    std::string getStats() const {
        std::lock_guard<std::mutex> lk(mu_);
        int totalBlocked = 0, totalPermanent = 0, totalSubnets = 0;
        for (const auto& [ip, info] : threats_) {
            if (info.permanentBan) totalPermanent++;
            else if (std::chrono::steady_clock::now() < info.banUntil) totalBlocked++;
        }
        for (const auto& [subnet, info] : subnets_) {
            if (info.blocked) totalSubnets++;
        }

        char buf[1024];
        std::snprintf(buf, sizeof(buf),
            "{\"fortress\":{\"blocked_ips\":%d,\"permanent_bans\":%d,"
            "\"blocked_subnets\":%d,\"total_threats\":%zu,"
            "\"honeypot_triggers\":%d,\"sqli_blocked\":%d,"
            "\"alerts\":%zu}}",
            totalBlocked, totalPermanent, totalSubnets,
            threats_.size(),
            totalHoneypotHits_.load(), totalSqliBlocked_.load(),
            alerts_.size());
        return buf;
    }

    std::string getAlertLog() const {
        std::lock_guard<std::mutex> lk(mu_);
        std::string out = "[";
        bool first = true;
        // Last 50 alerts
        size_t start = alerts_.size() > 50 ? alerts_.size() - 50 : 0;
        for (size_t i = start; i < alerts_.size(); i++) {
            if (!first) out += ",";
            out += "\"" + alerts_[i] + "\"";
            first = false;
        }
        out += "]";
        return out;
    }

    std::string getThreatLog() const {
        std::lock_guard<std::mutex> lk(mu_);
        std::string out = "[";
        bool first = true;
        // Last 100 threat entries
        size_t start = threatLog_.size() > 100 ? threatLog_.size() - 100 : 0;
        for (size_t i = start; i < threatLog_.size(); i++) {
            if (!first) out += ",";
            out += "\"" + threatLog_[i] + "\"";
            first = false;
        }
        out += "]";
        return out;
    }

    // Persist ban list to disk
    void saveBanList(const std::string& path) const {
        std::lock_guard<std::mutex> lk(mu_);
        std::ofstream f(path);
        if (!f) return;
        for (const auto& [ip, info] : threats_) {
            if (info.permanentBan || std::chrono::steady_clock::now() < info.banUntil) {
                f << ip << "\t" << info.failCount << "\t"
                  << (info.permanentBan ? "PERMANENT" : "TEMPORARY") << "\t"
                  << info.honeypotHits << "\t" << info.sqliAttempts << "\n";
            }
        }
    }

    // Load ban list from disk
    void loadBanList(const std::string& path) {
        std::lock_guard<std::mutex> lk(mu_);
        std::ifstream f(path);
        if (!f) return;
        std::string line;
        while (std::getline(f, line)) {
            if (line.empty()) continue;
            std::istringstream ss(line);
            std::string ip, banType;
            int failCount = 0, honeypotHits = 0, sqliAttempts = 0;
            if (!(ss >> ip >> failCount >> banType >> honeypotHits >> sqliAttempts)) continue;
            auto& info = threats_[ip];
            info.failCount = failCount;
            info.honeypotHits = honeypotHits;
            info.sqliAttempts = sqliAttempts;
            if (banType == "PERMANENT") {
                info.permanentBan = true;
            } else {
                info.banUntil = std::chrono::steady_clock::now() + std::chrono::hours(24);
            }
        }
    }

private:
    mutable std::mutex mu_;
    std::set<std::string>               whitelist_;
    std::map<std::string, IpThreatInfo> threats_;
    std::map<std::string, SubnetInfo>   subnets_;
    std::vector<CanaryToken>            canaryTokens_;
    std::vector<std::string>            alerts_;
    std::vector<std::string>            threatLog_;
    std::atomic<int>                    totalHoneypotHits_{0};
    std::atomic<int>                    totalSqliBlocked_{0};

    // ── Helpers ──────────────────────────────────────────────

    static std::string toLower(const std::string& s) {
        std::string r = s;
        for (auto& c : r) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return r;
    }

    static std::string toUpper(const std::string& s) {
        std::string r = s;
        for (auto& c : r) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return r;
    }

    // Extract /24 subnet from IP (e.g., "1.2.3.4" → "1.2.3")
    static std::string getSubnet(const std::string& ip) {
        auto pos = ip.rfind('.');
        if (pos == std::string::npos) return ip;
        return ip.substr(0, pos);
    }

    // Check subnet for coordinated attacks (3+ flagged IPs = block subnet)
    void checkSubnet(const std::string& ip) {
        std::string subnet = getSubnet(ip);
        auto& si = subnets_[subnet];

        // Count unique flagged IPs in this subnet
        int count = 0;
        for (const auto& [tip, tinfo] : threats_) {
            if (getSubnet(tip) == subnet && tinfo.failCount >= 3) count++;
        }
        si.flaggedIps = count;

        if (count >= 3 && !si.blocked) {
            si.blocked = true;
            si.blockedUntil = std::chrono::steady_clock::now() + std::chrono::hours(24);
            logAlert(subnet + ".*", "SUBNET BLOCKED: " + std::to_string(count) + " flagged IPs");
        }
    }

    // Generate canary tokens at startup
    void generateCanaryTokens() {
        // Deterministic-looking but useless tokens
        canaryTokens_ = {
            {"eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJ1c2VyIjoiYWRtaW4iLCJyb2xlIjoicm9vdCJ9.CANARY_INVALID",
             "bearer", "honeypot_login"},
            {"msql_ak_7f3d9a2b4c5e6f8a1b2c3d4e5f6a7b8c9d0e1f2a",
             "api_key", "honeypot_login"},
            {"msql_sk_live_a1b2c3d4e5f6g7h8i9j0k1l2m3n4o5p6q7r8s9",
             "api_key", "error_response"},
            {"msql_root_backup_xK9mP2nQ4rS6tU8vW0xY2zA4bC6dE8f",
             "api_key", "error_response"},
        };
    }

    void logThreat(const std::string& ip, const std::string& reason, int count) {
        char buf[256];
        auto now = std::time(nullptr);
        char timeBuf[20];
        std::tm ltm = milansql::safe_localtime(&now);
        std::strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", &ltm);
        std::snprintf(buf, sizeof(buf), "[%s] IP=%s count=%d reason=%s",
                      timeBuf, ip.c_str(), count, reason.c_str());
        threatLog_.push_back(buf);
        // Keep max 10000 entries
        if (threatLog_.size() > 10000) {
            threatLog_.erase(threatLog_.begin(), threatLog_.begin() + 5000);
        }
    }

    void logAlert(const std::string& ip, const std::string& msg) {
        char buf[256];
        auto now = std::time(nullptr);
        char timeBuf[20];
        std::tm ltm2 = milansql::safe_localtime(&now);
        std::strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", &ltm2);
        std::snprintf(buf, sizeof(buf), "[%s] ALERT IP=%s: %s",
                      timeBuf, ip.c_str(), msg.c_str());
        alerts_.push_back(buf);

        // Also write to disk immediately for persistence
        std::ofstream f("/opt/milansql/fortress_alerts.log", std::ios::app);
        if (f) f << buf << "\n";

        if (msg.find("HONEYPOT") != std::string::npos) totalHoneypotHits_++;
        if (msg.find("SQL Injection") != std::string::npos) totalSqliBlocked_++;
    }
};

// Global fortress instance
inline FortressEngine& g_fortress() {
    static FortressEngine instance;
    return instance;
}

} // namespace milansql
