#pragma once
// ============================================================
// migration_manager.hpp — Online Schema Migrations (Phase 109)
// CREATE MIGRATION name AS sql
// APPLY MIGRATION name
// ROLLBACK MIGRATION name
// SHOW MIGRATIONS  /  SHOW MIGRATION STATUS
// Persisted in database.migrations
// ============================================================

#include <string>
#include <vector>
#include <map>
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <ctime>
#include "../utils/date_utils.hpp"

namespace milansql {

struct MigrationDef {
    std::string name;
    std::string sql;           // forward migration SQL
    std::string rollbackSql;   // auto-derived or user-supplied
    std::string appliedAt;     // empty = pending, timestamp = applied
};

class MigrationManager {
public:
    MigrationManager() { load(); }

    // ── CREATE MIGRATION name AS sql_statement ────────────────
    std::string createMigration(const std::string& name, const std::string& sql) {
        if (migrations_.count(name))
            return "  ERROR: Migration '" + name + "' already exists.\n\n";
        MigrationDef m;
        m.name        = name;
        m.sql         = sql;
        m.rollbackSql = deriveRollback(sql);
        m.appliedAt   = "";
        order_.push_back(name);
        migrations_[name] = std::move(m);
        save();
        return "  Migration '" + name + "' created (pending).\n\n";
    }

    // ── APPLY MIGRATION name ──────────────────────────────────
    // Returns the SQL to execute; caller executes it against engine
    std::string getMigrationSql(const std::string& name) const {
        auto it = migrations_.find(name);
        if (it == migrations_.end()) return "";
        return it->second.sql;
    }

    // Mark migration as applied
    std::string markApplied(const std::string& name) {
        auto it = migrations_.find(name);
        if (it == migrations_.end())
            return "  ERROR: Migration '" + name + "' not found.\n\n";
        if (!it->second.appliedAt.empty())
            return "  INFO: Migration '" + name + "' already applied at " + it->second.appliedAt + ".\n\n";
        it->second.appliedAt = currentTimestamp();
        save();
        return "  Migration '" + name + "' applied successfully.\n\n";
    }

    // ── ROLLBACK MIGRATION name ───────────────────────────────
    std::string getRollbackSql(const std::string& name) const {
        auto it = migrations_.find(name);
        if (it == migrations_.end()) return "";
        return it->second.rollbackSql;
    }

    std::string markRolledBack(const std::string& name) {
        auto it = migrations_.find(name);
        if (it == migrations_.end())
            return "  ERROR: Migration '" + name + "' not found.\n\n";
        if (it->second.appliedAt.empty())
            return "  INFO: Migration '" + name + "' is not applied (pending).\n\n";
        it->second.appliedAt = "";
        save();
        return "  Migration '" + name + "' rolled back.\n\n";
    }

    bool exists(const std::string& name) const { return migrations_.count(name) > 0; }
    bool isApplied(const std::string& name) const {
        auto it = migrations_.find(name);
        return it != migrations_.end() && !it->second.appliedAt.empty();
    }
    bool isPending(const std::string& name) const {
        auto it = migrations_.find(name);
        return it != migrations_.end() && it->second.appliedAt.empty();
    }

    // ── SHOW MIGRATIONS ───────────────────────────────────────
    std::string showMigrations() const {
        if (migrations_.empty())
            return "  No migrations defined. Use: CREATE MIGRATION name AS sql\n\n";
        std::ostringstream oss;
        oss << "\n+------------------------+----------+---------------------+-------------------------------------------+\n";
        oss << "| Name                   | Status   | Applied At          | SQL                                       |\n";
        oss << "+------------------------+----------+---------------------+-------------------------------------------+\n";
        for (const auto& name : order_) {
            auto it = migrations_.find(name);
            if (it == migrations_.end()) continue;
            const auto& m = it->second;
            std::string status   = m.appliedAt.empty() ? "pending" : "applied";
            std::string applied  = m.appliedAt.empty() ? "(not applied)" : m.appliedAt;
            std::string sqlSnip  = m.sql.size() > 41 ? m.sql.substr(0, 38) + "..." : m.sql;
            std::string np = m.name;   np.resize(22, ' ');
            std::string sp = status;   sp.resize(8,  ' ');
            std::string ap = applied;  ap.resize(19, ' ');
            std::string qp = sqlSnip;  qp.resize(41, ' ');
            oss << "| " << np << " | " << sp << " | " << ap << " | " << qp << " |\n";
        }
        oss << "+------------------------+----------+---------------------+-------------------------------------------+\n";
        long long applied = 0, pending = 0;
        for (const auto& [k, m] : migrations_)
            if (m.appliedAt.empty()) ++pending; else ++applied;
        oss << "  " << migrations_.size() << " migrations  (" << applied << " applied, " << pending << " pending)\n\n";
        return oss.str();
    }

    std::string showStatus() const { return showMigrations(); }

private:
    static constexpr const char* filePath_ = "database.migrations";

    std::map<std::string, MigrationDef> migrations_;
    std::vector<std::string>            order_;      // insertion order

    // ── Persistence ───────────────────────────────────────────
    void save() const {
        std::ofstream f(filePath_);
        if (!f.is_open()) return;
        for (const auto& name : order_) {
            auto it = migrations_.find(name);
            if (it == migrations_.end()) continue;
            const auto& m = it->second;
            // Format: NAME\tSQL\tROLLBACK_SQL\tAPPLIED_AT
            f << m.name << "\t"
              << escapeTab(m.sql) << "\t"
              << escapeTab(m.rollbackSql) << "\t"
              << m.appliedAt << "\n";
        }
    }

    void load() {
        std::ifstream f(filePath_);
        if (!f.is_open()) return;
        std::string line;
        while (std::getline(f, line)) {
            if (line.empty()) continue;
            auto fields = splitTab(line);
            if (fields.size() < 4) continue;
            MigrationDef m;
            m.name        = fields[0];
            m.sql         = unescapeTab(fields[1]);
            m.rollbackSql = unescapeTab(fields[2]);
            m.appliedAt   = fields[3];
            if (!migrations_.count(m.name)) order_.push_back(m.name);
            migrations_[m.name] = std::move(m);
        }
    }

    // ── Derive rollback SQL from forward migration ────────────
    static std::string deriveRollback(const std::string& sql) {
        // Tokenize to detect ALTER TABLE x ADD COLUMN y → DROP COLUMN y
        std::string upper = sql;
        for (auto& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (upper.find("ALTER TABLE") != std::string::npos &&
            upper.find("ADD COLUMN")  != std::string::npos) {
            // Extract: ALTER TABLE tbl ADD COLUMN col ...
            // Derive:  ALTER TABLE tbl DROP COLUMN col
            std::istringstream iss(sql);
            std::vector<std::string> tokens;
            std::string tok;
            while (iss >> tok) tokens.push_back(tok);
            // tokens: ALTER TABLE <tbl> ADD COLUMN <col> ...
            if (tokens.size() >= 6) {
                std::string tbl = tokens[2];
                std::string col = tokens[5];
                // strip trailing semicolon from tbl/col
                if (!tbl.empty() && tbl.back() == ';') tbl.pop_back();
                if (!col.empty() && col.back() == ';') col.pop_back();
                return "ALTER TABLE " + tbl + " DROP COLUMN " + col;
            }
        }
        if (upper.find("CREATE TABLE") != std::string::npos) {
            // Derive: DROP TABLE name
            std::istringstream iss(sql);
            std::vector<std::string> tokens;
            std::string tok;
            while (iss >> tok) tokens.push_back(tok);
            if (tokens.size() >= 3) {
                std::string tbl = tokens[2];
                if (!tbl.empty() && tbl.back() == '(') tbl.pop_back();
                if (!tbl.empty() && tbl.back() == ';') tbl.pop_back();
                return "DROP TABLE IF EXISTS " + tbl;
            }
        }
        return "";  // no automatic rollback derivable
    }

    // ── Helpers ───────────────────────────────────────────────
    static std::string currentTimestamp() {
        std::time_t t = std::time(nullptr);
        char buf[32];
        std::tm ltm = milansql::safe_localtime(&t);
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &ltm);
        return buf;
    }

    static std::string escapeTab(const std::string& s) {
        std::string r;
        for (char c : s) {
            if (c == '\t') r += "\\t";
            else if (c == '\n') r += "\\n";
            else r += c;
        }
        return r;
    }

    static std::string unescapeTab(const std::string& s) {
        std::string r;
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '\\' && i + 1 < s.size()) {
                if (s[i+1] == 't') { r += '\t'; ++i; }
                else if (s[i+1] == 'n') { r += '\n'; ++i; }
                else r += s[i];
            } else {
                r += s[i];
            }
        }
        return r;
    }

    static std::vector<std::string> splitTab(const std::string& line) {
        std::vector<std::string> out;
        std::string cur;
        for (char c : line) {
            if (c == '\t') { out.push_back(cur); cur.clear(); }
            else cur += c;
        }
        out.push_back(cur);
        return out;
    }
};

// ── Global singleton ──────────────────────────────────────────
inline MigrationManager& g_migrationManager() {
    static MigrationManager mm;
    return mm;
}

} // namespace milansql
