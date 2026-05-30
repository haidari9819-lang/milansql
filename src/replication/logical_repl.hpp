#pragma once
// Phase 81: Logical Replication Log
// Append-only log: database.logical.log
// Format: one line per change: TABLENAME|OP|col1=val1,col2=val2,...
// OP: INSERT, UPDATE, DELETE

#include <string>
#include <vector>
#include <fstream>
#include <sstream>

namespace milansql {

struct LogChange {
    std::string table;
    std::string op;      // INSERT, UPDATE, DELETE
    std::vector<std::string> vals;   // column values
    std::vector<std::string> cols;   // column names
};

class LogicalReplLog {
public:
    explicit LogicalReplLog(const std::string& path = "database.logical.log")
        : path_(path) {}

    // Write a change to the log
    void writeChange(const std::string& table,
                     const std::string& op,
                     const std::vector<std::string>& cols,
                     const std::vector<std::string>& vals) {
        std::ofstream f(path_, std::ios::app);
        if (!f) return;
        f << table << "|" << op << "|";
        for (size_t i = 0; i < cols.size(); ++i) {
            if (i) f << ",";
            f << cols[i] << "=" << vals[i];
        }
        f << "\n";
    }

    // Read all changes
    std::vector<LogChange> readChanges() const {
        std::vector<LogChange> result;
        std::ifstream f(path_);
        std::string line;
        while (std::getline(f, line)) {
            auto p1 = line.find('|');
            auto p2 = line.find('|', p1 + 1);
            if (p1 == std::string::npos || p2 == std::string::npos) continue;
            LogChange ch;
            ch.table = line.substr(0, p1);
            ch.op    = line.substr(p1 + 1, p2 - p1 - 1);
            std::string kv = line.substr(p2 + 1);
            // parse col=val pairs
            std::istringstream ss(kv);
            std::string tok;
            while (std::getline(ss, tok, ',')) {
                auto eq = tok.find('=');
                if (eq != std::string::npos) {
                    ch.cols.push_back(tok.substr(0, eq));
                    ch.vals.push_back(tok.substr(eq + 1));
                }
            }
            result.push_back(std::move(ch));
        }
        return result;
    }

    size_t changeCount() const {
        size_t n = 0;
        std::ifstream f(path_);
        std::string line;
        while (std::getline(f, line)) ++n;
        return n;
    }

private:
    std::string path_;
};

} // namespace milansql
