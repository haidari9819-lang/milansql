#!/usr/bin/env python3
"""Fix UNION/INTERSECT/EXCEPT to use O(n) hash set dedup."""

ENGINE = '/opt/milansql/src/engine/engine.hpp'
with open(ENGINE, 'r') as f:
    engine = f.read()

# Fix UNION block — already has _UnionHash and unordered_set but still uses isDup+push_back
old_union = '''        } else if (op == "UNION") {
            // Vereinigung ohne Duplikate
            struct _UnionHash { size_t operator()(const std::vector<std::string>& v) const {
                size_t h = 0; for (const auto& s : v) h ^= std::hash<std::string>{}(s) + 0x9e3779b9 + (h << 6) + (h >> 2); return h;
            }};
            std::unordered_set<std::vector<std::string>, _UnionHash> seen;
            auto isDup = [&](const Row& r) {
                for (const auto& s : seen) if (s == r.values) return true;
                return false;
            };
            for (const auto& r : left.rows())
                if (!isDup(r)) { result.insert(r); seen.push_back(r.values); }
            for (const auto& r : right.rows())
                if (!isDup(r)) { result.insert(r); seen.push_back(r.values); }'''

new_union = '''        } else if (op == "UNION") {
            // Phase 173: O(n) dedup using hash set
            struct _UnionHash { size_t operator()(const std::vector<std::string>& v) const {
                size_t h = 0; for (const auto& s : v) h ^= std::hash<std::string>{}(s) + 0x9e3779b9 + (h << 6) + (h >> 2); return h;
            }};
            std::unordered_set<std::vector<std::string>, _UnionHash> seen;
            for (const auto& r : left.rows())
                if (seen.insert(r.values).second) result.insert(r);
            for (const auto& r : right.rows())
                if (seen.insert(r.values).second) result.insert(r);'''

if old_union in engine:
    engine = engine.replace(old_union, new_union)
    print("OK: UNION dedup fixed to O(n)")
else:
    print("WARN: UNION pattern not found")

# Fix INTERSECT block — still uses vector + O(n²)
old_intersect = '''        } else if (op == "INTERSECT") {
            // Nur Zeilen, die in beiden vorkommen (ohne Duplikate)
            std::vector<std::vector<std::string>> seen;
            auto isDup = [&](const Row& r) {
                for (const auto& s : seen) if (s == r.values) return true;
                return false;
            };
            for (const auto& lr : left.rows()) {
                if (isDup(lr)) continue;
                for (const auto& rr : right.rows()) {
                    if (lr.values == rr.values) {
                        result.insert(lr);
                        seen.push_back(lr.values);
                        break;
                    }
                }
            }'''

new_intersect = '''        } else if (op == "INTERSECT") {
            // Phase 173: O(n) INTERSECT using hash sets
            struct _IntHash { size_t operator()(const std::vector<std::string>& v) const {
                size_t h = 0; for (const auto& s : v) h ^= std::hash<std::string>{}(s) + 0x9e3779b9 + (h << 6) + (h >> 2); return h;
            }};
            std::unordered_set<std::vector<std::string>, _IntHash> rightSet;
            for (const auto& rr : right.rows()) rightSet.insert(rr.values);
            std::unordered_set<std::vector<std::string>, _IntHash> seen;
            for (const auto& lr : left.rows()) {
                if (rightSet.count(lr.values) && seen.insert(lr.values).second)
                    result.insert(lr);
            }'''

if old_intersect in engine:
    engine = engine.replace(old_intersect, new_intersect)
    print("OK: INTERSECT dedup fixed to O(n)")
else:
    print("WARN: INTERSECT pattern not found")

# Fix EXCEPT block — still uses vector + O(n²)
old_except = '''        } else if (op == "EXCEPT") {
            // Zeilen aus links, die NICHT in rechts vorkommen (ohne Duplikate)
            std::vector<std::vector<std::string>> seen;
            auto isDup = [&](const Row& r) {
                for (const auto& s : seen) if (s == r.values) return true;
                return false;
            };
            for (const auto& lr : left.rows()) {
                if (isDup(lr)) continue;
                bool inRight = false;
                for (const auto& rr : right.rows())
                    if (lr.values == rr.values) { inRight = true; break; }
                if (!inRight) { result.insert(lr); seen.push_back(lr.values); }
            }'''

new_except = '''        } else if (op == "EXCEPT") {
            // Phase 173: O(n) EXCEPT using hash sets
            struct _ExcHash { size_t operator()(const std::vector<std::string>& v) const {
                size_t h = 0; for (const auto& s : v) h ^= std::hash<std::string>{}(s) + 0x9e3779b9 + (h << 6) + (h >> 2); return h;
            }};
            std::unordered_set<std::vector<std::string>, _ExcHash> rightSet;
            for (const auto& rr : right.rows()) rightSet.insert(rr.values);
            std::unordered_set<std::vector<std::string>, _ExcHash> seen;
            for (const auto& lr : left.rows()) {
                if (!rightSet.count(lr.values) && seen.insert(lr.values).second)
                    result.insert(lr);
            }'''

if old_except in engine:
    engine = engine.replace(old_except, new_except)
    print("OK: EXCEPT dedup fixed to O(n)")
else:
    print("WARN: EXCEPT pattern not found")

with open(ENGINE, 'w') as f:
    f.write(engine)
print("DONE: All set operations now O(n)")
