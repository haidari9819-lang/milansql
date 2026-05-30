#pragma once
// ============================================================
// column_store.hpp — Phase 80: Column Store Engine (OLAP)
// Columnar storage: map<colName, vector<string>>
// Fast aggregation: SUM/AVG/COUNT/MIN/MAX iterate one vector.
// ============================================================

// System headers are included by engine.hpp before this file is injected.
// (string, vector, map, algorithm, numeric, stdexcept, cmath, climits, limits, sstream, iomanip)

// NOTE: This file is included from within engine/engine.hpp, inside namespace milansql.
// Column, Row, Table, WhereCondition are already defined at the point of inclusion.
// Do NOT add a namespace milansql {} wrapper here — it would create milansql::milansql nesting.

// ── numeric helper ────────────────────────────────────────────
static inline double csToDouble(const std::string& v) {
    try { return std::stod(v); } catch (...) { return 0.0; }
}

static inline std::string csFormatDouble(double v) {
    // Round to 2 decimal places for display
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << v;
    std::string s = oss.str();
    // Remove trailing zeros after decimal point, keep at least one digit
    if (s.find('.') != std::string::npos) {
        s.erase(s.find_last_not_of('0') + 1);
        if (s.back() == '.') s.pop_back();
    }
    return s;
}

// ────────────────────────────────────────────────────────────
// ColumnTable — columnar storage
// ────────────────────────────────────────────────────────────
class ColumnTable {
public:
    ColumnTable() = default;
    explicit ColumnTable(const std::string& name,
                         const std::vector<Column>& cols)
        : name_(name), columns_(cols) {
        for (const auto& c : cols)
            data_[c.name] = {};
    }

    const std::string&              name()    const { return name_; }
    const std::vector<Column>&      columns() const { return columns_; }
    size_t                          rowCount()const { return rowCount_; }

    // Insert a row of values (must match column count)
    void insert(const std::vector<std::string>& vals) {
        if (vals.size() != columns_.size())
            throw std::invalid_argument(
                "ColumnTable insert: Zeilenbreite (" + std::to_string(vals.size()) +
                ") != Schema (" + std::to_string(columns_.size()) + ")");
        for (size_t i = 0; i < columns_.size(); ++i)
            data_[columns_[i].name].push_back(vals[i]);
        ++rowCount_;
    }

    // ── Aggregation ───────────────────────────────────────────

    std::string aggregateCount() const {
        return std::to_string(rowCount_);
    }

    std::string aggregateSum(const std::string& col) const {
        const auto& vec = getCol(col);
        double sum = 0.0;
        for (const auto& v : vec) sum += csToDouble(v);
        return csFormatDouble(sum);
    }

    std::string aggregateAvg(const std::string& col) const {
        if (rowCount_ == 0) return "NULL";
        const auto& vec = getCol(col);
        double sum = 0.0;
        for (const auto& v : vec) sum += csToDouble(v);
        return csFormatDouble(sum / static_cast<double>(rowCount_));
    }

    std::string aggregateMin(const std::string& col) const {
        const auto& vec = getCol(col);
        if (vec.empty()) return "NULL";
        double mn = csToDouble(vec[0]);
        for (size_t i = 1; i < vec.size(); ++i) mn = std::min(mn, csToDouble(vec[i]));
        return csFormatDouble(mn);
    }

    std::string aggregateMax(const std::string& col) const {
        const auto& vec = getCol(col);
        if (vec.empty()) return "NULL";
        double mx = csToDouble(vec[0]);
        for (size_t i = 1; i < vec.size(); ++i) mx = std::max(mx, csToDouble(vec[i]));
        return csFormatDouble(mx);
    }

    // ── Projection to a Row-Store Table ──────────────────────

    // Returns all rows (no filter)
    Table toRows() const {
        Table result(name_, columns_);
        for (size_t r = 0; r < rowCount_; ++r) {
            std::vector<std::string> vals;
            vals.reserve(columns_.size());
            for (const auto& c : columns_)
                vals.push_back(data_.at(c.name)[r]);
            result.mutableRows().push_back(Row(vals));
        }
        return result;
    }

    // Returns rows matching WhereConditions (simple col op val only)
    Table filterRows(const std::vector<WhereCondition>& conds,
                     const std::string& logic = "AND") const {
        Table result(name_, columns_);
        for (size_t r = 0; r < rowCount_; ++r) {
            bool match = (logic == "AND");
            for (const auto& wc : conds) {
                bool condMatch = evalCond(r, wc);
                if (logic == "AND") {
                    if (!condMatch) { match = false; break; }
                } else {
                    if (condMatch)  { match = true;  break; }
                }
            }
            if (match) {
                std::vector<std::string> vals;
                vals.reserve(columns_.size());
                for (const auto& c : columns_)
                    vals.push_back(data_.at(c.name)[r]);
                result.mutableRows().push_back(Row(vals));
            }
        }
        return result;
    }

    // Count matching rows
    size_t countWhere(const std::vector<WhereCondition>& conds,
                      const std::string& logic = "AND") const {
        if (conds.empty()) return rowCount_;
        size_t n = 0;
        for (size_t r = 0; r < rowCount_; ++r) {
            bool match = (logic == "AND");
            for (const auto& wc : conds) {
                bool condMatch = evalCond(r, wc);
                if (logic == "AND") {
                    if (!condMatch) { match = false; break; }
                } else {
                    if (condMatch)  { match = true;  break; }
                }
            }
            if (match) ++n;
        }
        return n;
    }

    // Aggregate with WHERE filter applied
    std::string aggregateSumWhere(const std::string& col,
                                  const std::vector<WhereCondition>& conds,
                                  const std::string& logic = "AND") const {
        if (conds.empty()) return aggregateSum(col);
        const auto& vec = getCol(col);
        double sum = 0.0;
        for (size_t r = 0; r < rowCount_; ++r)
            if (matchRow(r, conds, logic)) sum += csToDouble(vec[r]);
        return csFormatDouble(sum);
    }

    std::string aggregateAvgWhere(const std::string& col,
                                   const std::vector<WhereCondition>& conds,
                                   const std::string& logic = "AND") const {
        if (conds.empty()) return aggregateAvg(col);
        const auto& vec = getCol(col);
        double sum = 0.0; size_t cnt = 0;
        for (size_t r = 0; r < rowCount_; ++r)
            if (matchRow(r, conds, logic)) { sum += csToDouble(vec[r]); ++cnt; }
        return cnt > 0 ? csFormatDouble(sum / static_cast<double>(cnt)) : "NULL";
    }

    std::string aggregateMinWhere(const std::string& col,
                                   const std::vector<WhereCondition>& conds,
                                   const std::string& logic = "AND") const {
        if (conds.empty()) return aggregateMin(col);
        const auto& vec = getCol(col);
        bool first = true; double mn = 0.0;
        for (size_t r = 0; r < rowCount_; ++r)
            if (matchRow(r, conds, logic)) {
                double v = csToDouble(vec[r]);
                if (first || v < mn) { mn = v; first = false; }
            }
        return first ? "NULL" : csFormatDouble(mn);
    }

    std::string aggregateMaxWhere(const std::string& col,
                                   const std::vector<WhereCondition>& conds,
                                   const std::string& logic = "AND") const {
        if (conds.empty()) return aggregateMax(col);
        const auto& vec = getCol(col);
        bool first = true; double mx = 0.0;
        for (size_t r = 0; r < rowCount_; ++r)
            if (matchRow(r, conds, logic)) {
                double v = csToDouble(vec[r]);
                if (first || v > mx) { mx = v; first = false; }
            }
        return first ? "NULL" : csFormatDouble(mx);
    }

private:
    std::string                               name_;
    std::vector<Column>                       columns_;
    std::map<std::string, std::vector<std::string>> data_;
    size_t                                    rowCount_ = 0;

    const std::vector<std::string>& getCol(const std::string& col) const {
        auto it = data_.find(col);
        if (it == data_.end())
            throw std::runtime_error("Column Store: Spalte '" + col + "' nicht gefunden");
        return it->second;
    }

    int colIndex(const std::string& col) const {
        for (int i = 0; i < static_cast<int>(columns_.size()); ++i)
            if (columns_[i].name == col) return i;
        return -1;
    }

    bool evalCond(size_t row, const WhereCondition& wc) const {
        auto it = data_.find(wc.col);
        if (it == data_.end()) return false;
        const std::string& cell = it->second[row];
        const std::string& val  = wc.val;
        const std::string& op   = wc.op;
        if (op == "=")  return cell == val;
        if (op == "!=") return cell != val;
        if (op == "<>") return cell != val;
        // Numeric comparisons
        try {
            double cv = std::stod(cell);
            double vv = std::stod(val);
            if (op == "<")  return cv < vv;
            if (op == ">")  return cv > vv;
            if (op == "<=") return cv <= vv;
            if (op == ">=") return cv >= vv;
        } catch (...) {
            // Fall back to string comparison
            if (op == "<")  return cell < val;
            if (op == ">")  return cell > val;
            if (op == "<=") return cell <= val;
            if (op == ">=") return cell >= val;
        }
        return false;
    }

    bool matchRow(size_t row, const std::vector<WhereCondition>& conds,
                  const std::string& logic) const {
        bool match = (logic == "AND");
        for (const auto& wc : conds) {
            bool cm = evalCond(row, wc);
            if (logic == "AND") { if (!cm) return false; }
            else                { if (cm)  return true;  }
        }
        return match;
    }
};

// (no closing namespace — this file is injected inside namespace milansql of engine.hpp)
