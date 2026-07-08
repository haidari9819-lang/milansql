#!/usr/bin/env python3
"""Fix compilation errors: add indexRangeSearch + fix BETWEEN val2 reference."""

ENGINE = '/opt/milansql/src/engine/engine.hpp'
with open(ENGINE, 'r') as f:
    engine = f.read()

changes = 0

# 1. Add indexRangeSearch method after indexSearch
old_after_indexsearch = '''    std::vector<size_t> indexSearch(const std::string& colName,
                                    const std::string& val) const {
        for (const auto& [idxName, entry] : indices_)
            if (!entry.cols.empty() && entry.cols[0] == colName)
                return entry.tree->search(val);
        return {};
    }

    std::vector<IndexInfo> getIndexes() const {'''

new_after_indexsearch = '''    std::vector<size_t> indexSearch(const std::string& colName,
                                    const std::string& val) const {
        for (const auto& [idxName, entry] : indices_)
            if (!entry.cols.empty() && entry.cols[0] == colName)
                return entry.tree->search(val);
        return {};
    }

    // Phase 173: Range search on indexed column
    std::vector<size_t> indexRangeSearch(const std::string& col, const std::string& lo,
                                          const std::string& hi, const std::string& op) const {
        for (const auto& [idxName, entry] : indices_)
            if (!entry.cols.empty() && entry.cols[0] == col)
                return entry.tree->rangeSearch(lo, hi, op);
        return {};
    }

    std::vector<IndexInfo> getIndexes() const {'''

if old_after_indexsearch in engine:
    engine = engine.replace(old_after_indexsearch, new_after_indexsearch)
    changes += 1
    print("OK: indexRangeSearch added to Table class")
else:
    print("FAIL: indexSearch anchor not found")

# 2. Fix BETWEEN reference: val2 -> betweenLow/betweenHigh
old_between = '''            } else if (conds[0].op == "BETWEEN") {
                std::string bLo = milansql::dateutils::stripQuotes(conds[0].val);
                std::string bHi = milansql::dateutils::stripQuotes(conds[0].val2);
                idxResults = src.indexRangeSearch(conds[0].col, bLo, bHi, "BETWEEN");'''

new_between = '''            } else if (conds[0].op == "BETWEEN") {
                std::string bLo = milansql::dateutils::stripQuotes(conds[0].betweenLow);
                std::string bHi = milansql::dateutils::stripQuotes(conds[0].betweenHigh);
                idxResults = src.indexRangeSearch(conds[0].col, bLo, bHi, "BETWEEN");'''

if old_between in engine:
    engine = engine.replace(old_between, new_between)
    changes += 1
    print("OK: BETWEEN val2 -> betweenLow/betweenHigh")
else:
    print("FAIL: BETWEEN pattern not found")

# 3. Remove the old duplicate indexRangeSearch that was inserted in wrong place (if any)
# Check for the old version with indexes_ (wrong member name)
old_wrong = '''    // Phase 173: Range search on indexed column
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
    }'''

if old_wrong in engine:
    engine = engine.replace(old_wrong, '')
    changes += 1
    print("OK: Removed old broken indexRangeSearch")

with open(ENGINE, 'w') as f:
    f.write(engine)
print(f"DONE: {changes} compile fixes applied")
