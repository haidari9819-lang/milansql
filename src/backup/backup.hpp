#pragma once
// Phase 57: Backup / Restore für MilanSQL
// src/backup/backup.hpp

#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <functional>
#include <set>
#include <map>
#include <algorithm>
#include <filesystem>

#include "../engine/engine.hpp"
#include "../utils/date_utils.hpp"

namespace milansql {

class MilanBackup {
public:
    // ── Dateinamen-Generierung ────────────────────────────────────────────────

    // Erzeugt timestamped Dateinamen: milansql_YYYYMMDD_HHMMSS.sql
    static std::string generateFilename() {
        auto t = dateutils::currentTm();
        char buf[32];
        std::snprintf(buf, sizeof(buf),
                      "milansql_%04d%02d%02d_%02d%02d%02d.sql",
                      t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                      t.tm_hour, t.tm_min, t.tm_sec);
        return std::string(buf);
    }

    // ── SQL-Wert escapen ──────────────────────────────────────────────────────

    // NULL bleibt, Zahlen bleiben, Strings werden 'single-quoted'
    static std::string escapeValue(const std::string& s) {
        if (s == "NULL") return "NULL";
        // Bereits single-quoted?
        if (s.size() >= 2 && s.front() == '\'' && s.back() == '\'') return s;
        // Numerisch?
        if (!s.empty()) {
            try {
                size_t pos = 0;
                std::stod(s, &pos);
                if (pos == s.size()) return s;
            } catch (...) {}
        }
        // String → 'text' mit escaped '
        std::string r = "'";
        for (char c : s) {
            if (c == '\'') r += "''";
            else r += c;
        }
        r += "'";
        return r;
    }

    // ── .sql Dateien auflisten ────────────────────────────────────────────────

    static std::vector<std::string> listBackups() {
        std::vector<std::string> result;
        try {
            namespace fs = std::filesystem;
            for (const auto& entry : fs::directory_iterator(".")) {
                if (entry.is_regular_file()) {
                    auto p = entry.path();
                    if (p.extension() == ".sql")
                        result.push_back(p.filename().string());
                }
            }
        } catch (...) {}
        std::sort(result.begin(), result.end());
        return result;
    }

    // ── Restore ───────────────────────────────────────────────────────────────

    // Liest SQL-Dump und führt jede Zeile über executeSQL aus
    // Gibt Zusammenfassung zurück
    static std::string restoreDatabase(const std::string& filepath,
                                       std::function<void(const std::string&)> executeSQL) {
        std::ifstream in(filepath);
        if (!in.is_open())
            return "FEHLER: Datei '" + filepath + "' nicht gefunden.";

        int stmtCount = 0;
        int errCount  = 0;
        std::string line;

        while (std::getline(in, line)) {
            // Whitespace trimmen
            const size_t a = line.find_first_not_of(" \t\r\n");
            if (a == std::string::npos) continue;
            const size_t b = line.find_last_not_of(" \t\r\n");
            line = line.substr(a, b - a + 1);

            // Kommentare und Leerzeilen überspringen
            if (line.size() >= 2 && line[0] == '-' && line[1] == '-') continue;
            if (line.empty()) continue;

            // Abschließendes Semikolon entfernen
            if (!line.empty() && line.back() == ';')
                line.pop_back();
            if (line.empty()) continue;

            try {
                executeSQL(line);
                ++stmtCount;
            } catch (...) {
                ++errCount;
            }
        }

        std::string msg = std::to_string(stmtCount) + " Statement(s) ausgefuehrt";
        if (errCount > 0)
            msg += ", " + std::to_string(errCount) + " Warnung(en)";
        msg += ".";
        return msg;
    }

    // ── Vollständiges Datenbank-Dump ──────────────────────────────────────────

    static std::string dumpDatabase(Engine& engine, std::string filepath) {
        if (filepath.empty()) filepath = generateFilename();

        std::ofstream out(filepath);
        if (!out.is_open())
            return "FEHLER: Kann Datei '" + filepath + "' nicht erstellen.";

        // ── Header ──
        out << "-- MilanSQL Backup v1.0.0\n";
        out << "-- Erstellt: " << dateutils::currentDatetimeStr() << "\n";
        {
            auto schemas = engine.showSchemas();
            out << "-- Schemas: ";
            for (size_t i = 0; i < schemas.size(); ++i) {
                if (i > 0) out << ", ";
                out << schemas[i];
            }
            out << "\n";
        }
        out << "\n";

        // ── Schemas & Tabellen ──
        auto allInternal = engine.getAllTableNamesInternal();

        // Nach Schema gruppieren
        std::map<std::string, std::vector<std::string>> bySchema;
        for (const auto& iname : allInternal) {
            auto dot = iname.find('.');
            std::string schema = (dot != std::string::npos) ? iname.substr(0, dot) : "public";
            bySchema[schema].push_back(iname);
        }

        // Reihenfolge: public zuerst, dann alphabetisch
        auto schemas = engine.showSchemas();
        std::sort(schemas.begin(), schemas.end());
        {
            auto it = std::find(schemas.begin(), schemas.end(), "public");
            if (it != schemas.end()) {
                schemas.erase(it);
                schemas.insert(schemas.begin(), "public");
            }
        }

        out << "-- ═══════════════════════════════════════\n";
        out << "-- Tabellen\n";
        out << "-- ═══════════════════════════════════════\n\n";

        for (const auto& schema : schemas) {
            auto sit = bySchema.find(schema);
            if (sit == bySchema.end() || sit->second.empty()) continue;

            auto tables = topoSort(engine, sit->second);

            if (schema != "public")
                out << "CREATE SCHEMA " << schema << ";\n";
            out << "USE " << schema << ";\n\n";

            for (const auto& iname : tables)
                writeTableDump(out, engine, iname);
        }

        // ── Indizes ──
        out << "-- ═══════════════════════════════════════\n";
        out << "-- Indizes\n";
        out << "-- ═══════════════════════════════════════\n\n";

        bool hasIdxs = false;
        for (const auto& schema : schemas) {
            auto sit = bySchema.find(schema);
            if (sit == bySchema.end()) continue;
            for (const auto& iname : sit->second) {
                auto dot = iname.find('.');
                std::string bareName = (dot != std::string::npos) ? iname.substr(dot + 1) : iname;
                if (writeIndexes(out, engine, iname, bareName)) hasIdxs = true;
            }
        }
        if (!hasIdxs) out << "-- (keine benutzerdefinierten Indizes)\n";
        out << "\n";

        // ── Views ──
        writeViews(out, engine);

        // ── Trigger ──
        writeTriggers(out, engine);

        // ── Procedures ──
        writeProcedures(out, engine);

        out << "-- Ende des Backups\n";
        out.close();

        return "Backup erstellt: '" + filepath + "'";
    }

    // ── Einzel-Tabellen-Dump ──────────────────────────────────────────────────

    static std::string dumpTable(Engine& engine, const std::string& tableName,
                                 std::string filepath) {
        if (filepath.empty()) filepath = tableName + "_backup.sql";

        std::ofstream out(filepath);
        if (!out.is_open())
            return "FEHLER: Kann Datei '" + filepath + "' nicht erstellen.";

        out << "-- MilanSQL Tabellen-Backup\n";
        out << "-- Tabelle: " << tableName << "\n";
        out << "-- Erstellt: " << dateutils::currentDatetimeStr() << "\n\n";

        std::string iname = engine.resolveTableName(tableName);
        auto dot = iname.find('.');
        std::string bareName = (dot != std::string::npos) ? iname.substr(dot + 1) : iname;

        writeTableDump(out, engine, iname);
        writeIndexes(out, engine, iname, bareName);

        out.close();
        return "Tabellen-Backup erstellt: '" + filepath + "'";
    }

private:
    // ── Topologische Sortierung ───────────────────────────────────────────────
    // Eltern-Tabellen kommen vor Kind-Tabellen (wichtig für FK-INSERTs)

    static std::vector<std::string> topoSort(Engine& engine,
                                              const std::vector<std::string>& tables) {
        std::set<std::string> tableSet(tables.begin(), tables.end());

        // deps[t] = Menge der Tabellen von denen t direkt abhängt
        std::map<std::string, std::set<std::string>> deps;
        for (const auto& t : tables) {
            deps[t] = {};
            try {
                const auto& fks = engine.selectAll(t).getForeignKeys();
                for (const auto& fk : fks) {
                    std::string ref = engine.resolveTableName(fk.refTable);
                    if (tableSet.count(ref) && ref != t)
                        deps[t].insert(ref);
                }
            } catch (...) {}
        }

        // Kahn's Algorithmus
        std::map<std::string, int> inDeg;
        for (const auto& t : tables)
            inDeg[t] = static_cast<int>(deps[t].size());

        std::vector<std::string> queue;
        for (const auto& t : tables)
            if (inDeg[t] == 0) queue.push_back(t);

        std::vector<std::string> result;
        while (!queue.empty()) {
            std::string t = queue.back();
            queue.pop_back();
            result.push_back(t);
            for (const auto& [other, depSet] : deps) {
                if (depSet.count(t)) {
                    if (--inDeg[other] == 0)
                        queue.push_back(other);
                }
            }
        }

        // Zyklen: verbleibende Tabellen anhängen
        for (const auto& t : tables)
            if (std::find(result.begin(), result.end(), t) == result.end())
                result.push_back(t);

        return result;
    }

    // ── CREATE TABLE SQL aufbauen ─────────────────────────────────────────────

    static std::string buildCreateTableSql(const Table& tbl, const std::string& bareName) {
        const auto& cols = tbl.columns();
        const auto& fks  = tbl.getForeignKeys();

        std::string sql = "CREATE TABLE " + bareName + " (";
        for (size_t i = 0; i < cols.size(); ++i) {
            if (i > 0) sql += ", ";
            const auto& c = cols[i];
            sql += c.name + " " + c.type;
            if (c.isPrimaryKey)                    sql += " PRIMARY KEY";
            if (c.autoIncrement)                   sql += " AUTO_INCREMENT";
            if (c.notNull && !c.isPrimaryKey)      sql += " NOT NULL";
            if (c.isUnique && !c.isPrimaryKey)     sql += " UNIQUE";
            if (c.hasDefault)                      sql += " DEFAULT " + c.defaultValue;
            for (const auto& cc : c.checks)
                sql += " CHECK (" + c.name + " " + cc.op + " " + cc.val + ")";
        }
        for (const auto& fk : fks) {
            sql += ", FOREIGN KEY (" + fk.fromCol + ") REFERENCES "
                 + fk.refTable + "(" + fk.refCol + ")";
            if (fk.onDelete != "RESTRICT")
                sql += " ON DELETE " + fk.onDelete;
        }
        sql += ")";
        return sql;
    }

    // ── Tabellen-Block schreiben ──────────────────────────────────────────────
    // DROP TABLE IF EXISTS + CREATE TABLE + INSERT-Statements

    static void writeTableDump(std::ostream& out, Engine& engine, const std::string& iname) {
        auto dot = iname.find('.');
        std::string bareName = (dot != std::string::npos) ? iname.substr(dot + 1) : iname;

        try {
            const Table& tbl = engine.selectAll(iname);
            const auto& rows = tbl.rows();

            out << "DROP TABLE IF EXISTS " << bareName << ";\n";
            out << buildCreateTableSql(tbl, bareName) << ";\n";

            if (!rows.empty()) {
                out << "\n";
                for (const auto& row : rows) {
                    out << "INSERT INTO " << bareName << " VALUES (";
                    for (size_t i = 0; i < row.values.size(); ++i) {
                        if (i > 0) out << ", ";
                        out << escapeValue(row.values[i]);
                    }
                    out << ");\n";
                }
            }
            out << "\n";
        } catch (...) {}
    }

    // ── Indizes schreiben ─────────────────────────────────────────────────────
    // Gibt true zurück, wenn mindestens ein Index geschrieben wurde

    static bool writeIndexes(std::ostream& out, Engine& engine,
                             const std::string& iname, const std::string& bareName) {
        bool wrote = false;
        try {
            auto idxs = engine.getIndexes(iname);
            for (const auto& idx : idxs) {
                if (idx.indexName == "PRIMARY") continue;  // auto-erstellt, überspringen
                out << "CREATE INDEX " << idx.indexName << " ON " << bareName
                    << " (" << idx.colName << ");\n";
                wrote = true;
            }
        } catch (...) {}
        return wrote;
    }

    // ── Views schreiben ───────────────────────────────────────────────────────

    static void writeViews(std::ostream& out, Engine& engine) {
        auto viewNames = engine.getAllViewNames();
        if (viewNames.empty()) return;

        out << "-- ═══════════════════════════════════════\n";
        out << "-- Views\n";
        out << "-- ═══════════════════════════════════════\n\n";

        std::sort(viewNames.begin(), viewNames.end());
        for (const auto& vname : viewNames) {
            try {
                const std::string& vsql = engine.getViewSql(vname);
                out << "CREATE VIEW " << vname << " AS " << vsql << ";\n";
            } catch (...) {}
        }
        out << "\n";
    }

    // ── Trigger schreiben ─────────────────────────────────────────────────────

    static void writeTriggers(std::ostream& out, Engine& engine) {
        const auto& triggers = engine.getAllTriggers();
        if (triggers.empty()) return;

        out << "-- ═══════════════════════════════════════\n";
        out << "-- Trigger\n";
        out << "-- ═══════════════════════════════════════\n\n";

        for (const auto& [name, td] : triggers) {
            // Body auf eine Zeile normalisieren
            std::string body = td.body;
            for (char& c : body)
                if (c == '\n' || c == '\r') c = ' ';

            out << "CREATE TRIGGER " << td.name
                << " " << td.timing
                << " " << td.event
                << " ON " << td.tableName
                << " FOR EACH ROW BEGIN " << body << " END\n";
        }
        out << "\n";
    }

    // ── Procedures schreiben ──────────────────────────────────────────────────

    static void writeProcedures(std::ostream& out, Engine& engine) {
        const auto& procs = engine.getAllProcedures();
        if (procs.empty()) return;

        out << "-- ═══════════════════════════════════════\n";
        out << "-- Procedures\n";
        out << "-- ═══════════════════════════════════════\n\n";

        for (const auto& [name, pd] : procs) {
            // Body auf eine Zeile normalisieren
            std::string body = pd.body;
            for (char& c : body)
                if (c == '\n' || c == '\r') c = ' ';

            out << "CREATE PROCEDURE " << pd.name << "(";
            for (size_t i = 0; i < pd.params.size(); ++i) {
                if (i > 0) out << ", ";
                out << pd.params[i].first << " " << pd.params[i].second;
            }
            out << ") BEGIN " << body << " END\n";
        }
        out << "\n";
    }
};

} // namespace milansql
