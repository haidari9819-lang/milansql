#pragma once
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <chrono>
#include <sstream>
#include <stdexcept>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#define closesocket close
#endif

// ============================================================
// federation_manager.hpp — Phase 105: Query Federation
// Forward-declare Table to avoid circular include with engine.hpp.
// The full definitions are inlined here so this header is self-contained.
// ============================================================

namespace milansql {

// ── Minimal forward types needed without pulling in engine.hpp ──
struct FedColumn {
    std::string name;
    std::string type;
    FedColumn(std::string n, std::string t) : name(std::move(n)), type(std::move(t)) {}
};

struct FedRow {
    std::vector<std::string> values;
    explicit FedRow(std::vector<std::string> v) : values(std::move(v)) {}
};

// A lightweight result table produced by federation queries.
// Converted to the engine's Table type in dispatch.hpp.
struct FedTable {
    std::string name;
    std::vector<FedColumn>  columns;
    std::vector<FedRow>     rows;
};

struct FederationNode {
    std::string name;
    std::string host;
    int port = 4406;
    bool reachable = true;
    double lastLatencyMs = 0.0;
};

struct FederatedTableDef {
    std::string name;
    std::vector<std::string> nodeNames;  // which nodes
    std::string baseQuery;               // AS SELECT * FROM local_data
    // For sharding:
    bool isSharded = false;
    std::string shardKey;
    std::string shardStrategy; // "HASH"
};

class FederationManager {
public:
    // Node management
    void addNode(const std::string& name, const std::string& host, int port) {
        std::lock_guard<std::mutex> lk(mu_);
        nodes_[name] = FederationNode{name, host, port, true, 0.0};
    }

    void removeNode(const std::string& name) {
        std::lock_guard<std::mutex> lk(mu_);
        nodes_.erase(name);
    }

    // Federated table management
    void defineFederatedTable(FederatedTableDef def) {
        std::lock_guard<std::mutex> lk(mu_);
        tables_[def.name] = std::move(def);
    }

    bool isFederatedTable(const std::string& name) const {
        std::lock_guard<std::mutex> lk(mu_);
        return tables_.count(name) > 0;
    }

    const FederatedTableDef* getFederatedTable(const std::string& name) const {
        // NOTE: caller must hold no lock or call without lock — or use external sync
        auto it = tables_.find(name);
        return it != tables_.end() ? &it->second : nullptr;
    }

    // Execute a federated query: send to all nodes, merge results
    FedTable executeFederated(const std::string& tableName,
                              const std::string& additionalWhere,
                              const std::vector<std::string>& selectCols);

    std::string showStatus();

private:
    mutable std::mutex mu_;
    std::map<std::string, FederationNode> nodes_;
    std::map<std::string, FederatedTableDef> tables_;

    // Send SQL to a remote node, return response lines
    std::vector<std::string> sendToNode(FederationNode& node, const std::string& sql);

    // Parse tab/pipe-separated response into rows
    static std::vector<std::vector<std::string>> parseResponse(
        const std::vector<std::string>& lines,
        std::vector<std::string>& outCols);

    // Merge multiple result sets
    static FedTable mergeResults(
        const std::string& tableName,
        const std::vector<std::string>& colNames,
        const std::vector<std::vector<std::vector<std::string>>>& rowSets);
};

// ── Implementation ────────────────────────────────────────────

inline std::vector<std::string> FederationManager::sendToNode(
    FederationNode& node, const std::string& sql)
{
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    auto t0 = std::chrono::steady_clock::now();

    if (getaddrinfo(node.host.c_str(),
                    std::to_string(node.port).c_str(),
                    &hints, &res) != 0) {
        node.reachable = false;
        return {};
    }

    int sock = static_cast<int>(socket(res->ai_family,
                                       res->ai_socktype,
                                       res->ai_protocol));
    if (sock < 0) {
        freeaddrinfo(res);
        node.reachable = false;
        return {};
    }

    // Set 2-second timeout
#ifdef _WIN32
    DWORD timeout = 2000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO,
               reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
    struct timeval tv{2, 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif

    if (connect(sock, res->ai_addr, static_cast<int>(res->ai_addrlen)) != 0) {
        closesocket(sock);
        freeaddrinfo(res);
        node.reachable = false;
        return {};
    }
    freeaddrinfo(res);
    node.reachable = true;

    // Send: SQL_QUERY\n{sql}\nEND\n
    std::string msg = "SQL_QUERY\n" + sql + "\nEND\n";
    send(sock, msg.c_str(), static_cast<int>(msg.size()), 0);

    // Receive response until "END\n"
    std::string resp;
    char buf[4096];
    int n;
    while ((n = recv(sock, buf, static_cast<int>(sizeof(buf)) - 1, 0)) > 0) {
        buf[n] = '\0';
        resp += buf;
        if (resp.find("END\n") != std::string::npos) break;
    }
    closesocket(sock);

    auto t1 = std::chrono::steady_clock::now();
    node.lastLatencyMs =
        std::chrono::duration<double, std::milli>(t1 - t0).count();

    // Split into lines
    std::vector<std::string> lines;
    std::istringstream iss(resp);
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line == "END") break;
        lines.push_back(line);
    }
    return lines;
}

inline FedTable FederationManager::executeFederated(
    const std::string& tableName,
    const std::string& /*additionalWhere*/,
    const std::vector<std::string>& /*selectCols*/)
{
    std::lock_guard<std::mutex> lk(mu_);
    auto it = tables_.find(tableName);
    if (it == tables_.end())
        throw std::runtime_error("Federated table not found: " + tableName);

    auto& ftd = it->second;
    std::string sql = ftd.baseQuery;

    std::vector<std::string> allColNames;
    std::vector<std::vector<std::vector<std::string>>> allRowSets;

    for (auto& nodeName : ftd.nodeNames) {
        auto nodeIt = nodes_.find(nodeName);
        if (nodeIt == nodes_.end()) continue;

        auto lines = sendToNode(nodeIt->second, sql);
        std::vector<std::string> cols;
        auto rows = parseResponse(lines, cols);

        if (allColNames.empty() && !cols.empty()) allColNames = cols;
        if (!rows.empty()) allRowSets.push_back(rows);
    }

    if (allColNames.empty()) {
        // Could not connect to any node — return empty table
        FedTable t;
        t.name = tableName;
        return t;
    }

    return mergeResults(tableName, allColNames, allRowSets);
}

inline std::vector<std::vector<std::string>> FederationManager::parseResponse(
    const std::vector<std::string>& lines,
    std::vector<std::string>& outCols)
{
    std::vector<std::vector<std::string>> result;
    bool headerParsed = false;

    for (const auto& line : lines) {
        if (line.empty()) continue;
        // Skip decorative border lines and status lines
        if (line[0] == '+') continue;
        if (line.substr(0, 2) == "OK" || line.substr(0, 5) == "ERROR") continue;
        // Skip box-drawing border lines (UTF-8: ┌ = \xe2\x94\x8c, etc.)
        if (static_cast<unsigned char>(line[0]) == 0xe2) continue;

        // Determine separator
        bool hasPipe     = (line.find('|')  != std::string::npos);
        bool hasTab      = (line.find('\t') != std::string::npos);

        std::vector<std::string> parts;

        if (hasTab && !hasPipe) {
            std::istringstream iss(line);
            std::string token;
            while (std::getline(iss, token, '\t')) {
                // trim
                while (!token.empty() && token.front() == ' ') token.erase(token.begin());
                while (!token.empty() && token.back()  == ' ') token.pop_back();
                if (!token.empty()) parts.push_back(token);
            }
        } else if (hasPipe) {
            size_t pos = 0;
            while (pos < line.size()) {
                size_t next = line.find('|', pos);
                if (next == std::string::npos) next = line.size();
                std::string p = line.substr(pos, next - pos);
                while (!p.empty() && p.front() == ' ') p.erase(p.begin());
                while (!p.empty() && p.back()  == ' ') p.pop_back();
                if (!p.empty()) parts.push_back(p);
                pos = next + 1;
            }
        } else {
            // Treat whole line as single token
            std::string tok = line;
            while (!tok.empty() && tok.front() == ' ') tok.erase(tok.begin());
            while (!tok.empty() && tok.back()  == ' ') tok.pop_back();
            if (!tok.empty()) parts.push_back(tok);
        }

        if (parts.empty()) continue;

        if (!headerParsed) {
            outCols = parts;
            headerParsed = true;
        } else {
            result.push_back(parts);
        }
    }
    return result;
}

inline FedTable FederationManager::mergeResults(
    const std::string& tableName,
    const std::vector<std::string>& colNames,
    const std::vector<std::vector<std::vector<std::string>>>& rowSets)
{
    FedTable t;
    t.name = tableName;
    for (const auto& cn : colNames)
        t.columns.push_back(FedColumn{cn, "TEXT"});
    for (const auto& rowSet : rowSets) {
        for (const auto& row : rowSet) {
            FedRow r(row);
            // Pad to column width if needed
            while (r.values.size() < colNames.size())
                r.values.push_back("NULL");
            t.rows.push_back(std::move(r));
        }
    }
    return t;
}

inline std::string FederationManager::showStatus() {
    std::lock_guard<std::mutex> lk(mu_);
    std::ostringstream oss;
    oss << "Federation Status:\n";
    oss << "  Nodes: " << nodes_.size() << "\n";
    oss << "  Federated Tables: " << tables_.size() << "\n\n";
    for (const auto& kv : nodes_) {
        const auto& node = kv.second;
        oss << "  Node: " << node.name << "\n";
        oss << "    Connection: " << node.host << ":" << node.port << "\n";
        oss << "    Status:     " << (node.reachable ? "OK" : "ERROR") << "\n";
        oss << "    Latency:    " << node.lastLatencyMs << "ms\n";
    }
    for (const auto& kv : tables_) {
        const auto& ftd = kv.second;
        oss << "\n  Federated Table: " << ftd.name << "\n";
        oss << "    Nodes: ";
        for (const auto& n : ftd.nodeNames) oss << n << " ";
        oss << "\n    Query: " << ftd.baseQuery << "\n";
    }
    return oss.str();
}

} // namespace milansql
