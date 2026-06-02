#pragma once
// ============================================================
// graph_store.hpp — Phase 116: Graph Database (Neo4j-like)
//
// In-memory property graph with:
//   - Nodes: id, label, name (identifier), properties (key→val)
//   - Edges: id, from, to, type, properties
//   - MATCH pattern:  (fromLabel)-[:edgeType]->(toLabel)
//   - SHORTEST PATH:  BFS from source to destination
//   - NEIGHBORS OF:   BFS up to given depth
//   - Persistence: graph.nodes + graph.edges (tab-separated text)
// ============================================================

#include <string>
#include <vector>
#include <map>
#include <set>
#include <queue>
#include <mutex>
#include <atomic>
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <cstdint>

namespace milansql {

// ── Graph Node ─────────────────────────────────────────────────
struct GraphNode {
    int         id    = 0;
    std::string label;               // e.g., "Person"
    std::string name;                // identifier, e.g., "alice"
    std::map<std::string, std::string> props;  // arbitrary properties
};

// ── Graph Edge ─────────────────────────────────────────────────
struct GraphEdge {
    int         id     = 0;
    int         fromId = 0;
    int         toId   = 0;
    std::string type;                // e.g., "KNOWS"
    std::map<std::string, std::string> props;
};

// ── MATCH result row ──────────────────────────────────────────
struct MatchRow {
    std::string fromName;
    std::string fromLabel;
    std::string edgeType;
    std::string toName;
    std::string toLabel;
};

// ── GraphStore ─────────────────────────────────────────────────
class GraphStore {
public:
    // ── Create a node ────────────────────────────────────────────
    int createNode(const std::string& label, const std::string& name,
                   const std::map<std::string, std::string>& props = {}) {
        std::lock_guard<std::mutex> g(mu_);
        int id = ++nextNodeId_;
        GraphNode node;
        node.id    = id;
        node.label = label;
        node.name  = name;
        node.props = props;
        nodes_[id]  = std::move(node);
        nameToId_[name] = id;
        return id;
    }

    // ── Create an edge ────────────────────────────────────────────
    // fromName / toName are node identifiers (not IDs)
    bool createEdge(const std::string& fromName, const std::string& type,
                    const std::string& toName,
                    const std::map<std::string, std::string>& props = {}) {
        std::lock_guard<std::mutex> g(mu_);
        auto fi = nameToId_.find(fromName);
        auto ti = nameToId_.find(toName);
        if (fi == nameToId_.end() || ti == nameToId_.end()) return false;

        int id = ++nextEdgeId_;
        GraphEdge edge;
        edge.id     = id;
        edge.fromId = fi->second;
        edge.toId   = ti->second;
        edge.type   = type;
        edge.props  = props;
        edges_.push_back(std::move(edge));
        return true;
    }

    // ── MATCH (fromLabel)-[:edgeType]->(toLabel) ─────────────────
    // Returns all matching (from, edge, to) triples.
    std::vector<MatchRow> matchPattern(const std::string& fromLabel,
                                       const std::string& edgeType,
                                       const std::string& toLabel) const {
        std::lock_guard<std::mutex> g(mu_);
        std::vector<MatchRow> results;
        for (const auto& e : edges_) {
            if (!edgeType.empty() && e.type != edgeType) continue;

            auto fi = nodes_.find(e.fromId);
            auto ti = nodes_.find(e.toId);
            if (fi == nodes_.end() || ti == nodes_.end()) continue;

            const GraphNode& fn = fi->second;
            const GraphNode& tn = ti->second;

            if (!fromLabel.empty() && fn.label != fromLabel) continue;
            if (!toLabel.empty()   && tn.label != toLabel)   continue;

            MatchRow row;
            row.fromName  = fn.name;
            row.fromLabel = fn.label;
            row.edgeType  = e.type;
            row.toName    = tn.name;
            row.toLabel   = tn.label;
            results.push_back(std::move(row));
        }
        return results;
    }

    // ── Shortest path (BFS) from fromName to toName ──────────────
    // Returns the sequence of node names, empty if no path found.
    std::vector<std::string> shortestPath(const std::string& fromName,
                                          const std::string& toName) const {
        std::lock_guard<std::mutex> g(mu_);
        auto fi = nameToId_.find(fromName);
        auto ti = nameToId_.find(toName);
        if (fi == nameToId_.end() || ti == nameToId_.end()) return {};

        int srcId  = fi->second;
        int destId = ti->second;
        if (srcId == destId) return {fromName};

        // BFS
        std::map<int, int> parent;   // id → parent id
        std::queue<int>    q;
        std::set<int>      visited;

        q.push(srcId);
        visited.insert(srcId);
        parent[srcId] = -1;

        while (!q.empty()) {
            int cur = q.front(); q.pop();
            // Find all outgoing neighbours
            for (const auto& e : edges_) {
                int next = -1;
                if      (e.fromId == cur && !visited.count(e.toId))   next = e.toId;
                else if (e.toId   == cur && !visited.count(e.fromId)) next = e.fromId;
                if (next < 0) continue;

                visited.insert(next);
                parent[next] = cur;
                if (next == destId) goto found;
                q.push(next);
            }
        }
        return {};  // no path

    found:
        // Reconstruct path
        std::vector<int> pathIds;
        for (int cur = destId; cur != -1; cur = parent.at(cur))
            pathIds.push_back(cur);
        std::reverse(pathIds.begin(), pathIds.end());

        std::vector<std::string> path;
        for (int id : pathIds) {
            auto it = nodes_.find(id);
            if (it != nodes_.end()) path.push_back(it->second.name);
        }
        return path;
    }

    // ── Neighbors of node up to given BFS depth ──────────────────
    std::vector<std::string> neighbors(const std::string& nodeName,
                                       int depth) const {
        std::lock_guard<std::mutex> g(mu_);
        auto it = nameToId_.find(nodeName);
        if (it == nameToId_.end()) return {};

        int srcId = it->second;
        std::set<int> visited;
        std::queue<std::pair<int,int>> q;  // (nodeId, currentDepth)
        visited.insert(srcId);
        q.push({srcId, 0});

        std::vector<std::string> result;

        while (!q.empty()) {
            auto [cur, d] = q.front(); q.pop();
            if (d >= depth) continue;

            for (const auto& e : edges_) {
                int next = -1;
                if      (e.fromId == cur && !visited.count(e.toId))   next = e.toId;
                else if (e.toId   == cur && !visited.count(e.fromId)) next = e.fromId;
                if (next < 0) continue;

                visited.insert(next);
                auto ni = nodes_.find(next);
                if (ni != nodes_.end()) result.push_back(ni->second.name);
                q.push({next, d + 1});
            }
        }
        return result;
    }

    // ── Accessors ─────────────────────────────────────────────────
    std::vector<GraphNode> allNodes() const {
        std::lock_guard<std::mutex> g(mu_);
        std::vector<GraphNode> out;
        for (const auto& kv : nodes_) out.push_back(kv.second);
        return out;
    }

    const std::vector<GraphEdge>& allEdges() const {
        // Caller should hold mu_ or accept races; for display only
        return edges_;
    }

    bool hasNode(const std::string& name) const {
        std::lock_guard<std::mutex> g(mu_);
        return nameToId_.count(name) > 0;
    }

    int nodeIdByName(const std::string& name) const {
        std::lock_guard<std::mutex> g(mu_);
        auto it = nameToId_.find(name);
        return it == nameToId_.end() ? -1 : it->second;
    }

    const GraphNode* nodeByName(const std::string& name) const {
        std::lock_guard<std::mutex> g(mu_);
        auto it = nameToId_.find(name);
        if (it == nameToId_.end()) return nullptr;
        auto ni = nodes_.find(it->second);
        return ni == nodes_.end() ? nullptr : &ni->second;
    }

    // ── Persistence ───────────────────────────────────────────────
    void persistNodes(const std::string& path = "graph.nodes") const {
        std::lock_guard<std::mutex> g(mu_);
        std::ofstream f(path);
        for (const auto& [id, n] : nodes_) {
            f << n.id << "\t" << n.label << "\t" << n.name;
            for (const auto& [k, v] : n.props)
                f << "\t" << k << "=" << v;
            f << "\n";
        }
    }

    void persistEdges(const std::string& path = "graph.edges") const {
        std::lock_guard<std::mutex> g(mu_);
        std::ofstream f(path);
        for (const auto& e : edges_) {
            f << e.id << "\t" << e.fromId << "\t" << e.toId << "\t" << e.type;
            for (const auto& [k, v] : e.props)
                f << "\t" << k << "=" << v;
            f << "\n";
        }
    }

    void loadNodes(const std::string& path = "graph.nodes") {
        std::lock_guard<std::mutex> g(mu_);
        std::ifstream f(path);
        if (!f) return;
        std::string line;
        while (std::getline(f, line)) {
            if (line.empty()) continue;
            std::istringstream ss(line);
            GraphNode n;
            std::string tok;
            std::getline(ss, tok, '\t'); n.id    = std::stoi(tok);
            std::getline(ss, n.label, '\t');
            std::getline(ss, n.name,  '\t');
            while (std::getline(ss, tok, '\t')) {
                auto eq = tok.find('=');
                if (eq != std::string::npos)
                    n.props[tok.substr(0, eq)] = tok.substr(eq + 1);
            }
            nodes_[n.id] = n;
            nameToId_[n.name] = n.id;
            if (n.id >= nextNodeId_) nextNodeId_ = n.id + 1;
        }
    }

    void loadEdges(const std::string& path = "graph.edges") {
        std::lock_guard<std::mutex> g(mu_);
        std::ifstream f(path);
        if (!f) return;
        std::string line;
        while (std::getline(f, line)) {
            if (line.empty()) continue;
            std::istringstream ss(line);
            GraphEdge e;
            std::string tok;
            std::getline(ss, tok, '\t'); e.id     = std::stoi(tok);
            std::getline(ss, tok, '\t'); e.fromId = std::stoi(tok);
            std::getline(ss, tok, '\t'); e.toId   = std::stoi(tok);
            std::getline(ss, e.type,    '\t');
            while (std::getline(ss, tok, '\t')) {
                auto eq = tok.find('=');
                if (eq != std::string::npos)
                    e.props[tok.substr(0, eq)] = tok.substr(eq + 1);
            }
            edges_.push_back(e);
            if (e.id >= nextEdgeId_) nextEdgeId_ = e.id + 1;
        }
    }

private:
    mutable std::mutex          mu_;
    std::map<int, GraphNode>    nodes_;
    std::vector<GraphEdge>      edges_;
    std::map<std::string, int>  nameToId_;   // name → id
    int                         nextNodeId_ = 1;
    int                         nextEdgeId_ = 1;
};

// ── Global singleton ───────────────────────────────────────────
inline GraphStore& g_graphStore() {
    static GraphStore gs;
    return gs;
}

// ── Parse a Cypher-like node pattern ──────────────────────────
// Input: "(alice:Person {name: Alice, age: 30})"
// Or:    "(alice:Person)"
// Or:    "(alice)"
struct ParsedNodePattern {
    std::string var;
    std::string label;
    std::map<std::string, std::string> props;
};

inline ParsedNodePattern parseCypherNode(const std::string& input) {
    ParsedNodePattern p;
    // Find content between ( and )
    size_t open  = input.find('(');
    size_t close = input.rfind(')');
    if (open == std::string::npos) return p;
    std::string inner = input.substr(open + 1,
                                     (close != std::string::npos ? close : input.size()) - open - 1);
    // Trim
    while (!inner.empty() && inner.front() == ' ') inner = inner.substr(1);
    while (!inner.empty() && inner.back()  == ' ') inner.pop_back();

    // Split at colon and {
    size_t colon = inner.find(':');
    size_t brace = inner.find('{');

    if (colon == std::string::npos) {
        p.var = inner.substr(0, brace != std::string::npos ? brace : inner.size());
    } else {
        p.var = inner.substr(0, colon);
        size_t lend = brace != std::string::npos ? brace : inner.size();
        p.label = inner.substr(colon + 1, lend - colon - 1);
    }

    // Trim var / label
    while (!p.var.empty()   && p.var.back()   == ' ') p.var.pop_back();
    while (!p.label.empty() && p.label.back() == ' ') p.label.pop_back();

    // Parse properties {key: val, key2: val2}
    if (brace != std::string::npos) {
        size_t bclose = inner.rfind('}');
        std::string propsStr = inner.substr(brace + 1,
            bclose != std::string::npos ? bclose - brace - 1 : inner.size() - brace - 1);
        // Split by comma, then key: val
        std::istringstream pss(propsStr);
        std::string tok;
        while (std::getline(pss, tok, ',')) {
            // trim
            while (!tok.empty() && tok.front() == ' ') tok = tok.substr(1);
            while (!tok.empty() && tok.back()  == ' ') tok.pop_back();
            size_t col2 = tok.find(':');
            if (col2 == std::string::npos) continue;
            std::string k = tok.substr(0, col2);
            std::string v = tok.substr(col2 + 1);
            while (!k.empty() && k.back()  == ' ') k.pop_back();
            while (!v.empty() && v.front() == ' ') v = v.substr(1);
            while (!v.empty() && v.back()  == ' ') v.pop_back();
            p.props[k] = v;
        }
    }
    return p;
}

} // namespace milansql
