#pragma once
// ============================================================
// foreign_data_wrapper.hpp — Base FDW Interface
// Phase 89: Foreign Data Wrapper
// ============================================================

#include <string>
#include <vector>
#include <map>

namespace milansql {

struct ServerDef {
    std::string name;
    std::string wrapperType; // "csv" or "http_json"
};

struct ForeignTableDef {
    std::string name;
    std::string serverName;
    std::vector<std::string> colNames;
    std::vector<std::string> colTypes;
    std::map<std::string,std::string> options; // file, delimiter, url
};

// Base interface for Foreign Data Wrappers
class ForeignDataWrapper {
public:
    virtual ~ForeignDataWrapper() = default;
    virtual std::vector<std::vector<std::string>> scan(
        const ForeignTableDef& tbl,
        const std::vector<std::string>& colNames) = 0;
};

} // namespace milansql
