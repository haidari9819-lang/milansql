#pragma once
// ============================================================
// copy_manager.hpp — COPY FROM/TO Bulk Import/Export
// Phase 92: MilanSQL
// ============================================================

#include <string>
#include <vector>
#include <chrono>
#include <fstream>
#include <cstring>
#include <stdexcept>

#include "../engine/engine.hpp"
#include "../utils/csv_utils.hpp"

namespace milansql {

struct CopyStats {
    std::string lastOperation;  // "FROM" or "TO"
    std::string tableName;
    std::string fileName;
    int rowsProcessed = 0;
    int errors = 0;
    double durationMs = 0.0;
};

class CopyManager {
public:
    // Copy FROM file into table
    std::string copyFrom(Engine& engine, const std::string& tableName,
                         const std::string& fileName,
                         const std::string& format,   // "CSV" or "BINARY"
                         char delimiter,
                         bool hasHeader)
    {
        if (format == "BINARY") {
            return doBinaryFrom(engine, tableName, fileName);
        } else {
            // Read file into lines
            std::ifstream f(fileName);
            if (!f) throw std::runtime_error("COPY: Datei nicht gefunden: " + fileName);
            std::vector<std::string> lines;
            std::string line;
            while (std::getline(f, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                lines.push_back(std::move(line));
            }
            return doCsvFrom(engine, tableName, lines, delimiter, hasHeader);
        }
    }

    // Copy FROM STDIN (reads lines from stdinLines until "\." found)
    std::string copyFromStdin(Engine& engine, const std::string& tableName,
                              const std::vector<std::string>& stdinLines,
                              char delimiter)
    {
        return doCsvFrom(engine, tableName, stdinLines, delimiter, false);
    }

    // Copy TO file from table
    std::string copyTo(Engine& engine, const std::string& tableName,
                       const std::string& fileName,
                       const std::string& format,
                       char delimiter,
                       bool hasHeader)
    {
        if (format == "BINARY") {
            return doBinaryTo(engine, tableName, fileName);
        } else {
            return doCsvTo(engine, tableName, fileName, delimiter, hasHeader);
        }
    }

    // Copy query result TO file
    std::string copyQueryTo(const std::vector<std::string>& colNames,
                            const std::vector<std::vector<std::string>>& rows,
                            const std::string& fileName,
                            char delimiter,
                            bool hasHeader)
    {
        auto start = std::chrono::steady_clock::now();

        std::ofstream out(fileName);
        if (!out) throw std::runtime_error("COPY: Datei nicht schreibbar: " + fileName);

        if (hasHeader && !colNames.empty()) {
            for (size_t i = 0; i < colNames.size(); ++i) {
                if (i > 0) out << delimiter;
                out << CsvUtils::escapeField(colNames[i], delimiter);
            }
            out << '\n';
        }

        for (const auto& row : rows) {
            for (size_t i = 0; i < row.size(); ++i) {
                if (i > 0) out << delimiter;
                out << CsvUtils::escapeField(row[i], delimiter);
            }
            out << '\n';
        }

        auto end = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(end - start).count();

        int n = static_cast<int>(rows.size());
        lastStats_ = {"TO", "(query)", fileName, n, 0, ms};
        return "COPY " + std::to_string(n);
    }

    std::string showStats() const {
        if (lastStats_.lastOperation.empty()) {
            return "No COPY operation has been performed yet.";
        }
        std::string out;
        out += "Last COPY Operation:\n";
        out += "  Direction: " + lastStats_.lastOperation + "\n";
        out += "  Table:     " + lastStats_.tableName + "\n";
        out += "  File:      " + lastStats_.fileName + "\n";
        out += "  Rows:      " + std::to_string(lastStats_.rowsProcessed) + "\n";
        out += "  Errors:    " + std::to_string(lastStats_.errors) + "\n";
        // Format duration with 2 decimal places
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.2f", lastStats_.durationMs);
        out += "  Duration:  ";
        out += buf;
        out += " ms";
        return out;
    }

private:
    CopyStats lastStats_;

    // Binary format: magic bytes + column info + row count + rows
    static const uint32_t BINARY_MAGIC = 0x4D4C4342U; // "MLCB"

    static void writeU32(std::ofstream& f, uint32_t v) {
        f.write(reinterpret_cast<const char*>(&v), sizeof(v));
    }
    static void writeStr(std::ofstream& f, const std::string& s) {
        uint32_t len = static_cast<uint32_t>(s.size());
        writeU32(f, len);
        if (len > 0) f.write(s.data(), static_cast<std::streamsize>(len));
    }

    static bool readU32(std::ifstream& f, uint32_t& v) {
        return static_cast<bool>(f.read(reinterpret_cast<char*>(&v), sizeof(v)));
    }
    static bool readStr(std::ifstream& f, std::string& s) {
        uint32_t len = 0;
        if (!readU32(f, len)) return false;
        if (len == 0xFFFFFFFFU) { s.clear(); return true; } // NULL sentinel
        s.resize(len);
        if (len > 0 && !f.read(&s[0], static_cast<std::streamsize>(len))) return false;
        return true;
    }

    std::string doCsvFrom(Engine& engine, const std::string& tableName,
                          const std::vector<std::string>& lines,
                          char delimiter, bool hasHeader)
    {
        auto start = std::chrono::steady_clock::now();

        auto& tbl = engine.getMutableTable(tableName);

        size_t startLine = hasHeader ? 1 : 0;
        int imported = 0;

        for (size_t i = startLine; i < lines.size(); i++) {
            if (lines[i].empty()) continue;
            auto vals = CsvUtils::parseLine(lines[i], delimiter);
            tbl.mutableRows().push_back(Row(std::move(vals)));
            imported++;
        }

        auto end = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(end - start).count();

        lastStats_ = {"FROM", tableName, "(lines)", imported, 0, ms};
        return "COPY " + std::to_string(imported);
    }

    std::string doBinaryFrom(Engine& engine, const std::string& tableName,
                             const std::string& fileName)
    {
        auto start = std::chrono::steady_clock::now();

        std::ifstream f(fileName, std::ios::binary);
        if (!f) throw std::runtime_error("COPY BINARY: Datei nicht gefunden: " + fileName);

        // Read magic
        uint32_t magic = 0;
        if (!readU32(f, magic) || magic != BINARY_MAGIC)
            throw std::runtime_error("COPY BINARY: Ungueltige Magic-Bytes in: " + fileName);

        // Read column count
        uint32_t numCols = 0;
        if (!readU32(f, numCols))
            throw std::runtime_error("COPY BINARY: Fehler beim Lesen der Spaltenanzahl.");

        // Read column names/types (informational — we trust the table schema)
        for (uint32_t c = 0; c < numCols; ++c) {
            std::string colName, colType;
            if (!readStr(f, colName) || !readStr(f, colType))
                throw std::runtime_error("COPY BINARY: Fehler beim Lesen der Spalten-Metadaten.");
        }

        // Read row count
        uint32_t numRows = 0;
        if (!readU32(f, numRows))
            throw std::runtime_error("COPY BINARY: Fehler beim Lesen der Zeilenanzahl.");

        auto& tbl = engine.getMutableTable(tableName);

        int imported = 0;
        for (uint32_t r = 0; r < numRows; ++r) {
            std::vector<std::string> vals;
            for (uint32_t c = 0; c < numCols; ++c) {
                std::string val;
                if (!readStr(f, val))
                    throw std::runtime_error("COPY BINARY: Fehler beim Lesen einer Zeile.");
                vals.push_back(std::move(val));
            }
            tbl.mutableRows().push_back(Row(std::move(vals)));
            imported++;
        }

        auto end = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(end - start).count();

        lastStats_ = {"FROM", tableName, fileName, imported, 0, ms};
        return "COPY " + std::to_string(imported);
    }

    std::string doCsvTo(Engine& engine, const std::string& tableName,
                        const std::string& fileName, char delimiter, bool hasHeader)
    {
        auto start = std::chrono::steady_clock::now();

        const auto& tbl = engine.selectAll(tableName);
        const auto& cols = tbl.columns();
        const auto& rows = tbl.rows();

        std::ofstream out(fileName);
        if (!out) throw std::runtime_error("COPY: Datei nicht schreibbar: " + fileName);

        if (hasHeader && !cols.empty()) {
            for (size_t i = 0; i < cols.size(); ++i) {
                if (i > 0) out << delimiter;
                out << CsvUtils::escapeField(cols[i].name, delimiter);
            }
            out << '\n';
        }

        for (const auto& row : rows) {
            for (size_t i = 0; i < row.values.size(); ++i) {
                if (i > 0) out << delimiter;
                out << CsvUtils::escapeField(row.values[i], delimiter);
            }
            out << '\n';
        }

        auto end = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(end - start).count();

        int n = static_cast<int>(rows.size());
        lastStats_ = {"TO", tableName, fileName, n, 0, ms};
        return "COPY " + std::to_string(n);
    }

    std::string doBinaryTo(Engine& engine, const std::string& tableName,
                           const std::string& fileName)
    {
        auto start = std::chrono::steady_clock::now();

        const auto& tbl = engine.selectAll(tableName);
        const auto& cols = tbl.columns();
        const auto& rows = tbl.rows();

        std::ofstream out(fileName, std::ios::binary);
        if (!out) throw std::runtime_error("COPY BINARY: Datei nicht schreibbar: " + fileName);

        // Magic
        writeU32(out, BINARY_MAGIC);

        // Column count
        uint32_t numCols = static_cast<uint32_t>(cols.size());
        writeU32(out, numCols);

        // Column metadata
        for (const auto& col : cols) {
            writeStr(out, col.name);
            writeStr(out, col.type);
        }

        // Row count
        writeU32(out, static_cast<uint32_t>(rows.size()));

        // Rows
        for (const auto& row : rows) {
            for (size_t c = 0; c < cols.size(); ++c) {
                if (c < row.values.size()) {
                    writeStr(out, row.values[c]);
                } else {
                    // NULL: write empty string
                    writeStr(out, "");
                }
            }
        }

        auto end = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(end - start).count();

        int n = static_cast<int>(rows.size());
        lastStats_ = {"TO", tableName, fileName, n, 0, ms};
        return "COPY " + std::to_string(n);
    }
};

} // namespace milansql
