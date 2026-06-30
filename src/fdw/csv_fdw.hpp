#pragma once
// ============================================================
// csv_fdw.hpp — CSV Foreign Data Wrapper
// Phase 89: Foreign Data Wrapper
// ============================================================

#include "foreign_data_wrapper.hpp"
#include "../utils/csv_utils.hpp"
#include <fstream>

namespace milansql {

class CsvFdw : public ForeignDataWrapper {
public:
    std::vector<std::vector<std::string>> scan(
        const ForeignTableDef& tbl,
        const std::vector<std::string>& /*colNames*/) override
    {
        std::string file = tbl.options.count("file") ? tbl.options.at("file") : "";
        if (file.empty()) return {};
        // Prevent path traversal — block absolute paths and ..
        if (file.find("..") != std::string::npos || file[0] == '/' || file[0] == '\\') return {};

        char delim = ',';
        if (tbl.options.count("delimiter") && !tbl.options.at("delimiter").empty())
            delim = tbl.options.at("delimiter")[0];

        // Read all rows including header (skipHeader=false)
        std::vector<std::vector<std::string>> allRows;
        try {
            allRows = CsvUtils::readFile(file, delim, false);
        } catch (...) {
            return {};
        }
        if (allRows.empty()) return {};

        // First row is the header
        const auto& header = allRows[0];

        // Build column index mapping: FDW col name -> CSV column index
        std::vector<int> colIdx;
        for (const auto& cn : tbl.colNames) {
            int idx = -1;
            for (int i = 0; i < static_cast<int>(header.size()); ++i) {
                if (header[static_cast<size_t>(i)] == cn) { idx = i; break; }
            }
            colIdx.push_back(idx);
        }

        std::vector<std::vector<std::string>> result;
        for (size_t r = 1; r < allRows.size(); ++r) {
            const auto& vals = allRows[r];
            std::vector<std::string> row;
            for (int ci : colIdx) {
                if (ci >= 0 && static_cast<size_t>(ci) < vals.size())
                    row.push_back(vals[static_cast<size_t>(ci)]);
                else
                    row.push_back("NULL");
            }
            result.push_back(std::move(row));
        }
        return result;
    }
};

} // namespace milansql
