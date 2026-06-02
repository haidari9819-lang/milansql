#pragma once
// ============================================================
// document_store.hpp — Phase 116: Document Store (MongoDB-like)
//
// Collections of JSON documents with field-level filtering.
// Documents are stored in-memory and optionally persisted to
// <collection>.docs files (one JSON per line, prefixed by id).
//
// Supported filter operators:
//   =   equal (string or numeric)
//   !=  not equal
//   >   greater-than  (numeric)
//   >=  greater-or-equal (numeric)
//   <   less-than (numeric)
//   <=  less-or-equal (numeric)
//   CONTAINS  array or substring match
// ============================================================

#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <atomic>
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <ctime>
#include <cstdint>

namespace milansql {

// ── JSON document ──────────────────────────────────────────────
struct JsonDoc {
    int         id      = 0;
    std::string json;        // raw JSON string (may have _id injected)
    std::string created;     // ISO timestamp
};

// ── Minimal JSON field extractor (no external deps) ───────────
// Handles: {"key": "val"}, {"key": 42}, {"key": [...]}, {"key": {...}}
// Returns the raw value string (without enclosing quotes for strings).
inline std::string jsonExtractField(const std::string& json,
                                    const std::string& key) {
    std::string needle = "\"" + key + "\"";
    size_t kpos = json.find(needle);
    if (kpos == std::string::npos) return "";

    // Find colon after key
    size_t colon = json.find(':', kpos + needle.size());
    if (colon == std::string::npos) return "";

    // Skip whitespace
    size_t vs = colon + 1;
    while (vs < json.size() && (json[vs] == ' ' || json[vs] == '\t')) ++vs;
    if (vs >= json.size()) return "";

    char first = json[vs];
    if (first == '"') {
        // String value
        size_t end = json.find('"', vs + 1);
        if (end == std::string::npos) return "";
        return json.substr(vs + 1, end - vs - 1);
    }
    if (first == '[') {
        // Array: find matching ]
        int depth = 0;
        size_t end = vs;
        for (; end < json.size(); ++end) {
            if (json[end] == '[') ++depth;
            else if (json[end] == ']') { --depth; if (depth == 0) break; }
        }
        return json.substr(vs, end - vs + 1);
    }
    if (first == '{') {
        // Object: find matching }
        int depth = 0;
        size_t end = vs;
        for (; end < json.size(); ++end) {
            if (json[end] == '{') ++depth;
            else if (json[end] == '}') { --depth; if (depth == 0) break; }
        }
        return json.substr(vs, end - vs + 1);
    }
    // Number / bool / null — read until , } ]
    size_t end = vs;
    while (end < json.size() &&
           json[end] != ',' && json[end] != '}' && json[end] != ']')
        ++end;
    std::string val = json.substr(vs, end - vs);
    // trim trailing whitespace
    while (!val.empty() && (val.back() == ' ' || val.back() == '\t'))
        val.pop_back();
    return val;
}

// Check if a JSON array value (e.g. ["IT","premium"]) contains token.
inline bool jsonArrayContains(const std::string& arrStr,
                              const std::string& token) {
    // Simple: look for the token as a word or quoted value
    if (arrStr.find(token) != std::string::npos) return true;
    return false;
}

// ── DocumentStore ──────────────────────────────────────────────
class DocumentStore {
public:
    // ── Insert a document ────────────────────────────────────────
    int insert(const std::string& collection, const std::string& jsonIn) {
        std::lock_guard<std::mutex> g(mu_);
        int id = ++nextId_;
        JsonDoc doc;
        doc.id   = id;
        doc.json = injectId(jsonIn, id);
        doc.created = currentTimestamp();
        collections_[collection].push_back(std::move(doc));
        dirty_.insert(collection);
        return id;
    }

    // ── Find documents matching a filter ────────────────────────
    std::vector<JsonDoc> find(const std::string& collection,
                              const std::string& field,
                              const std::string& op,
                              const std::string& value) const {
        std::lock_guard<std::mutex> g(mu_);
        std::vector<JsonDoc> out;
        auto it = collections_.find(collection);
        if (it == collections_.end()) return out;
        for (const auto& doc : it->second) {
            if (matches(doc, field, op, value)) out.push_back(doc);
        }
        return out;
    }

    // ── Find all documents in a collection ──────────────────────
    std::vector<JsonDoc> findAll(const std::string& collection) const {
        std::lock_guard<std::mutex> g(mu_);
        auto it = collections_.find(collection);
        if (it == collections_.end()) return {};
        return it->second;
    }

    // ── Update documents matching a filter ──────────────────────
    int update(const std::string& collection,
               const std::string& field, const std::string& op,
               const std::string& value,
               const std::string& newJson) {
        std::lock_guard<std::mutex> g(mu_);
        auto it = collections_.find(collection);
        if (it == collections_.end()) return 0;
        int count = 0;
        for (auto& doc : it->second) {
            if (matches(doc, field, op, value)) {
                doc.json = injectId(newJson, doc.id);
                ++count;
            }
        }
        if (count) dirty_.insert(collection);
        return count;
    }

    // ── Delete documents matching a filter ──────────────────────
    int remove(const std::string& collection,
               const std::string& field, const std::string& op,
               const std::string& value) {
        std::lock_guard<std::mutex> g(mu_);
        auto it = collections_.find(collection);
        if (it == collections_.end()) return 0;
        auto& vec = it->second;
        int before = static_cast<int>(vec.size());
        vec.erase(std::remove_if(vec.begin(), vec.end(),
            [&](const JsonDoc& d) { return matches(d, field, op, value); }),
            vec.end());
        int count = before - static_cast<int>(vec.size());
        if (count) dirty_.insert(collection);
        return count;
    }

    // ── Create empty collection ──────────────────────────────────
    void createCollection(const std::string& name) {
        std::lock_guard<std::mutex> g(mu_);
        if (!collections_.count(name)) {
            collections_[name]; // default-construct empty vector
        }
    }

    // ── Drop collection ──────────────────────────────────────────
    bool dropCollection(const std::string& name) {
        std::lock_guard<std::mutex> g(mu_);
        return collections_.erase(name) > 0;
    }

    // ── List all collections ─────────────────────────────────────
    std::vector<std::string> listCollections() const {
        std::lock_guard<std::mutex> g(mu_);
        std::vector<std::string> names;
        for (const auto& kv : collections_)
            names.push_back(kv.first);
        return names;
    }

    size_t count(const std::string& collection) const {
        std::lock_guard<std::mutex> g(mu_);
        auto it = collections_.find(collection);
        return it == collections_.end() ? 0 : it->second.size();
    }

    bool hasCollection(const std::string& name) const {
        std::lock_guard<std::mutex> g(mu_);
        return collections_.count(name) > 0;
    }

    // ── Persistence ──────────────────────────────────────────────
    void persist() {
        std::lock_guard<std::mutex> g(mu_);
        for (const auto& cname : dirty_) {
            auto it = collections_.find(cname);
            if (it == collections_.end()) continue;
            std::ofstream f(cname + ".docs");
            for (const auto& doc : it->second)
                f << doc.id << "\t" << doc.created << "\t" << doc.json << "\n";
        }
        dirty_.clear();
    }

    void load(const std::string& collection) {
        std::lock_guard<std::mutex> g(mu_);
        std::ifstream f(collection + ".docs");
        if (!f) return;
        collections_[collection].clear();
        std::string line;
        int maxId = nextId_.load();
        while (std::getline(f, line)) {
            if (line.empty()) continue;
            std::istringstream ss(line);
            JsonDoc doc;
            std::string ts;
            ss >> doc.id;
            ss >> ts;
            doc.created = ts;
            std::getline(ss, doc.json);
            if (!doc.json.empty() && doc.json.front() == '\t')
                doc.json = doc.json.substr(1);
            if (doc.id > maxId) maxId = doc.id;
            collections_[collection].push_back(std::move(doc));
        }
        if (maxId >= nextId_.load()) nextId_.store(maxId + 1);
    }

private:
    mutable std::mutex                            mu_;
    std::map<std::string, std::vector<JsonDoc>>   collections_;
    std::set<std::string>                         dirty_;
    std::atomic<int>                              nextId_{0};

    static std::string currentTimestamp() {
        std::time_t t = std::time(nullptr);
        char buf[32]; struct tm tmb{};
#if defined(_WIN32)
        localtime_s(&tmb, &t);
#else
        localtime_r(&t, &tmb);
#endif
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tmb);
        return std::string(buf);
    }

    static std::string injectId(const std::string& json, int id) {
        // Inject "_id":<id> as first field if not present
        if (json.find("\"_id\"") != std::string::npos) return json;
        size_t brace = json.find('{');
        if (brace == std::string::npos) return json;
        return json.substr(0, brace + 1) +
               "\"_id\":" + std::to_string(id) + ", " +
               json.substr(brace + 1);
    }

    bool matches(const JsonDoc& doc,
                 const std::string& field,
                 const std::string& op,
                 const std::string& value) const {
        if (field.empty() && op.empty()) return true; // no filter

        if (op == "CONTAINS") {
            // Array or substring contains
            std::string arrVal = jsonExtractField(doc.json, field);
            return jsonArrayContains(arrVal, value);
        }

        std::string fv = jsonExtractField(doc.json, field);

        if (op == "=")  return fv == value;
        if (op == "!=") return fv != value;

        // Numeric comparisons
        try {
            double lv = std::stod(fv);
            double rv = std::stod(value);
            if (op == ">")  return lv >  rv;
            if (op == ">=") return lv >= rv;
            if (op == "<")  return lv <  rv;
            if (op == "<=") return lv <= rv;
        } catch (...) {
            if (op == ">")  return fv >  value;
            if (op == ">=") return fv >= value;
            if (op == "<")  return fv <  value;
            if (op == "<=") return fv <= value;
        }
        return false;
    }
};

// ── Global singleton ───────────────────────────────────────────
inline DocumentStore& g_documentStore() {
    static DocumentStore ds;
    return ds;
}

} // namespace milansql
