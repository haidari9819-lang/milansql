#!/usr/bin/env python3
"""Add missing rangeCollect to BTree + indexRangeSearch to Table."""

BTREE = '/opt/milansql/src/engine/btree.hpp'
with open(BTREE, 'r') as f:
    btree = f.read()

# Add rangeCollect before countKeys
anchor = '    static size_t countKeys(const Node* n) {'
helper = '''    // Phase 173: Range collect helper (in-order traversal)
    void rangeCollect(const Node* node, const std::string& lo, const std::string& hi,
                       const std::string& op, std::vector<size_t>& result) const {
        if (!node) return;
        int n = static_cast<int>(node->keys.size());
        for (int i = 0; i < n; ++i) {
            if (!node->isLeaf)
                rangeCollect(node->children[i].get(), lo, hi, op, result);
            const auto& k = node->keys[i];
            bool match = false;
            bool numeric = true;
            double kd = 0, lod = 0, hid = 0;
            try { kd = std::stod(k); } catch (...) { numeric = false; }
            if (numeric && !lo.empty()) { try { lod = std::stod(lo); } catch (...) { numeric = false; } }
            if (numeric && !hi.empty()) { try { hid = std::stod(hi); } catch (...) { numeric = false; } }
            if (numeric) {
                if (op == ">") match = kd > lod;
                else if (op == ">=") match = kd >= lod;
                else if (op == "<") match = kd < hid;
                else if (op == "<=") match = kd <= hid;
                else if (op == "BETWEEN") match = kd >= lod && kd <= hid;
            } else {
                if (op == ">") match = k > lo;
                else if (op == ">=") match = k >= lo;
                else if (op == "<") match = k < hi;
                else if (op == "<=") match = k <= hi;
                else if (op == "BETWEEN") match = k >= lo && k <= hi;
            }
            if (match)
                for (size_t ri : node->rowIndices[i]) result.push_back(ri);
        }
        if (!node->isLeaf && n < static_cast<int>(node->children.size()))
            rangeCollect(node->children[n].get(), lo, hi, op, result);
    }

    ''' + anchor

if anchor in btree and 'void rangeCollect' not in btree:
    btree = btree.replace(anchor, helper)
    print("OK: BTree rangeCollect added")
elif 'void rangeCollect' in btree:
    print("SKIP: rangeCollect already exists")
else:
    print("FAIL: countKeys anchor not found")

with open(BTREE, 'w') as f:
    f.write(btree)

# Now add indexRangeSearch to Table in engine.hpp
ENGINE = '/opt/milansql/src/engine/engine.hpp'
with open(ENGINE, 'r') as f:
    engine = f.read()

if 'indexRangeSearch' not in engine:
    # Find indexSearch function end and insert after it
    anchor2 = '    bool hasIndex(const std::string& col) const {'
    if anchor2 in engine:
        range_method = '''    // Phase 173: Range search on indexed column
    std::vector<size_t> indexRangeSearch(const std::string& col, const std::string& lo,
                                          const std::string& hi, const std::string& op) const {
        for (const auto& [name, idx] : indexes_) {
            std::string leading = idx.colName;
            auto comma = leading.find(',');
            if (comma != std::string::npos) leading = leading.substr(0, comma);
            while (!leading.empty() && leading[0] == ' ') leading.erase(0, 1);
            while (!leading.empty() && leading.back() == ' ') leading.pop_back();
            if (leading == col) return idx.tree.rangeSearch(lo, hi, op);
        }
        return {};
    }

    ''' + anchor2
        engine = engine.replace(anchor2, range_method)
        print("OK: Table::indexRangeSearch added")
    else:
        print("FAIL: hasIndex anchor not found")
else:
    print("SKIP: indexRangeSearch already exists")

# Fix UNION dedup seen check — find the actual pattern
import re
m = re.search(r'(bool found = false;[^}]+?seen\.push_back)', engine, re.DOTALL)
if m:
    print(f"Found UNION seen pattern at offset {m.start()}")
else:
    # Look for the actual pattern more loosely
    lines = engine.split('\n')
    for i, line in enumerate(lines):
        if 'seen.push_back' in line and i > 0:
            print(f"  Line {i+1}: {line.strip()}")

with open(ENGINE, 'w') as f:
    f.write(engine)
print("DONE: BTree range + indexRangeSearch fixes applied")
