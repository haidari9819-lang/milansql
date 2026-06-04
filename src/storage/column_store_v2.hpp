#pragma once
// ============================================================
// column_store_v2.hpp — Phase 142: Column Store V2 + OLAP
// ============================================================
#include <vector>
#include <string>
#include <unordered_map>
#include <map>
#include <cstdint>
#include <algorithm>
#include <numeric>
#include <stdexcept>
#include <limits>

namespace milansql {

// A chunk of up to 1024 column values
struct ColumnChunk {
    static constexpr size_t CHUNK_SIZE = 1024;
    std::vector<std::string> rawValues;
    // Numeric values (for INT columns)
    std::vector<int64_t> intValues;
    bool isNumeric = false;
    // Delta encoding state
    int64_t deltaBase = 0;
    std::vector<int64_t> deltas;
    // Dictionary encoding (for TEXT columns)
    std::vector<std::string> dict;
    std::map<std::string, int> dictIndex;
    std::vector<int> dictCodes;
    bool isDictEncoded = false;

    void build(const std::vector<std::string>& vals) {
        rawValues = vals;
        // Try numeric
        isNumeric = true;
        intValues.clear();
        for (const auto& v : vals) {
            if (v == "NULL" || v.empty()) { isNumeric = false; break; }
            try { intValues.push_back(static_cast<int64_t>(std::stoll(v))); }
            catch (...) { isNumeric = false; break; }
        }
        if (isNumeric && !intValues.empty()) {
            // Delta encode
            deltaBase = intValues[0];
            deltas.resize(intValues.size());
            for (size_t i = 0; i < intValues.size(); ++i)
                deltas[i] = intValues[i] - deltaBase;
        } else {
            // Dictionary encode text
            intValues.clear();
            dictIndex.clear();
            dict.clear();
            dictCodes.clear();
            isDictEncoded = true;
            for (const auto& v : vals) {
                auto it = dictIndex.find(v);
                if (it == dictIndex.end()) {
                    int code = static_cast<int>(dict.size());
                    dictIndex[v] = code;
                    dict.push_back(v);
                    dictCodes.push_back(code);
                } else {
                    dictCodes.push_back(it->second);
                }
            }
        }
    }

    int64_t vectorizedSum() const {
        if (!isNumeric) return 0;
        int64_t s = 0;
        for (auto v : intValues) s += v;
        return s;
    }
    int64_t vectorizedCount() const {
        return static_cast<int64_t>(rawValues.size());
    }
    int64_t vectorizedMin() const {
        if (!isNumeric || intValues.empty()) return 0;
        return *std::min_element(intValues.begin(), intValues.end());
    }
    int64_t vectorizedMax() const {
        if (!isNumeric || intValues.empty()) return 0;
        return *std::max_element(intValues.begin(), intValues.end());
    }
};

// Column store for a table — stores all rows for selected columns
struct ColumnStoreIndex {
    std::string tableName;
    std::string indexName;
    std::vector<std::string> columns;  // indexed column names
    // col name -> list of chunks
    std::map<std::string, std::vector<ColumnChunk>> colChunks;

    void build(const std::string& tbl, const std::string& idxName,
               const std::vector<std::string>& cols,
               const std::vector<std::string>& /*colNames*/,
               const std::vector<std::vector<std::string>>& colData) {
        tableName = tbl;
        indexName = idxName;
        columns   = cols;
        colChunks.clear();
        for (size_t ci = 0; ci < cols.size(); ++ci) {
            if (ci >= colData.size()) break;
            const auto& data = colData[ci];
            // split into chunks of CHUNK_SIZE
            std::vector<ColumnChunk> chunks;
            for (size_t offset = 0; offset < data.size(); offset += ColumnChunk::CHUNK_SIZE) {
                ColumnChunk chunk;
                size_t end = std::min(offset + ColumnChunk::CHUNK_SIZE, data.size());
                chunk.build(std::vector<std::string>(data.begin() + static_cast<ptrdiff_t>(offset),
                                                      data.begin() + static_cast<ptrdiff_t>(end)));
                chunks.push_back(std::move(chunk));
            }
            colChunks[cols[ci]] = std::move(chunks);
        }
    }

    bool hasColumn(const std::string& col) const {
        return colChunks.count(col) > 0;
    }

    int64_t vectorizedSum(const std::string& col) const {
        auto it = colChunks.find(col);
        if (it == colChunks.end()) return 0;
        int64_t total = 0;
        for (const auto& chunk : it->second) total += chunk.vectorizedSum();
        return total;
    }
    int64_t vectorizedCount(const std::string& col) const {
        auto it = colChunks.find(col);
        if (it == colChunks.end()) return 0;
        int64_t total = 0;
        for (const auto& chunk : it->second) total += chunk.vectorizedCount();
        return total;
    }
    int64_t vectorizedMin(const std::string& col) const {
        auto it = colChunks.find(col);
        if (it == colChunks.end()) return 0;
        int64_t m = std::numeric_limits<int64_t>::max();
        bool found = false;
        for (const auto& chunk : it->second) {
            if (!chunk.isNumeric || chunk.intValues.empty()) continue;
            int64_t v = chunk.vectorizedMin();
            if (!found || v < m) { m = v; found = true; }
        }
        return found ? m : 0;
    }
    int64_t vectorizedMax(const std::string& col) const {
        auto it = colChunks.find(col);
        if (it == colChunks.end()) return 0;
        int64_t m = std::numeric_limits<int64_t>::lowest();
        bool found = false;
        for (const auto& chunk : it->second) {
            if (!chunk.isNumeric || chunk.intValues.empty()) continue;
            int64_t v = chunk.vectorizedMax();
            if (!found || v > m) { m = v; found = true; }
        }
        return found ? m : 0;
    }
};

} // namespace milansql
