#pragma once
// ============================================================
// csv_utils.hpp — CSV Reader / Writer for MilanSQL
// Phase 60: LOAD DATA INFILE + SELECT INTO OUTFILE
// ============================================================

#include <string>
#include <vector>
#include <fstream>
#include <stdexcept>
#include <algorithm>
#include <filesystem>
#include <cctype>

namespace milansql {

class CsvUtils {
public:
    // ── Parse a single CSV line into fields ──────────────────
    // Handles RFC-4180 quoting: "field", "fie""ld", plain field
    static std::vector<std::string> parseLine(const std::string& line,
                                              char sep = ',')
    {
        std::vector<std::string> fields;
        std::string field;
        bool inQuotes = false;

        for (size_t i = 0; i < line.size(); ++i) {
            char c = line[i];
            if (c == '"') {
                if (inQuotes && i + 1 < line.size() && line[i + 1] == '"') {
                    field += '"';   // escaped quote ""
                    ++i;
                } else {
                    inQuotes = !inQuotes;
                }
            } else if (c == sep && !inQuotes) {
                fields.push_back(std::move(field));
                field.clear();
            } else {
                field += c;
            }
        }
        fields.push_back(std::move(field));
        return fields;
    }

    // ── Read entire CSV file ──────────────────────────────────
    // Returns a vector of rows; each row is a vector of field strings.
    // skipHeader: skip first non-empty line.
    static std::vector<std::vector<std::string>> readFile(
        const std::string& path, char sep = ',', bool skipHeader = false)
    {
        std::ifstream f(path);
        if (!f) throw std::runtime_error("CSV-Datei nicht gefunden: " + path);

        std::vector<std::vector<std::string>> rows;
        std::string line;
        bool firstDone = false;

        while (std::getline(f, line)) {
            // strip Windows \r
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            if (!firstDone && skipHeader) { firstDone = true; continue; }
            firstDone = true;
            rows.push_back(parseLine(line, sep));
        }
        return rows;
    }

    // ── Read header row from CSV (first non-empty line) ───────
    static std::vector<std::string> readHeader(const std::string& path,
                                                char sep = ',')
    {
        std::ifstream f(path);
        if (!f) throw std::runtime_error("CSV-Datei nicht gefunden: " + path);
        std::string line;
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) return parseLine(line, sep);
        }
        return {};
    }

    // ── Write CSV file ────────────────────────────────────────
    static void writeFile(const std::string& path,
                          const std::vector<std::string>& headers,
                          const std::vector<std::vector<std::string>>& rows,
                          char sep = ',')
    {
        std::ofstream f(path);
        if (!f) throw std::runtime_error("CSV-Datei nicht schreibbar: " + path);

        // Header row
        for (size_t i = 0; i < headers.size(); ++i) {
            if (i > 0) f << sep;
            f << escapeField(headers[i], sep);
        }
        f << '\n';

        // Data rows
        for (const auto& row : rows) {
            for (size_t i = 0; i < row.size(); ++i) {
                if (i > 0) f << sep;
                f << escapeField(row[i], sep);
            }
            f << '\n';
        }
    }

    // ── Escape a single field ─────────────────────────────────
    // Wraps in double-quotes if field contains sep, quote, or newline.
    static std::string escapeField(const std::string& s, char sep = ',') {
        if (s.empty()) return "";
        bool needsQuotes = false;
        for (char c : s) {
            if (c == sep || c == '"' || c == '\n' || c == '\r') {
                needsQuotes = true;
                break;
            }
        }
        if (!needsQuotes) return s;
        std::string out = "\"";
        for (char c : s) {
            if (c == '"') out += "\"\"";
            else          out += c;
        }
        out += '"';
        return out;
    }

    // ── Auto-detect separator from first line ─────────────────
    static char detectSeparator(const std::string& firstLine) {
        int commas     = static_cast<int>(
            std::count(firstLine.begin(), firstLine.end(), ','));
        int semicolons = static_cast<int>(
            std::count(firstLine.begin(), firstLine.end(), ';'));
        int tabs       = static_cast<int>(
            std::count(firstLine.begin(), firstLine.end(), '\t'));
        if (tabs > commas && tabs > semicolons) return '\t';
        if (semicolons > commas) return ';';
        return ',';
    }

    // ── Convert user-visible separator string to char ─────────
    // Accepts "," ";" "\t" or a single character.
    static char parseSepChar(const std::string& s) {
        if (s == "\\t") return '\t';
        if (s.empty())  return ',';
        return s[0];
    }

    // ── List CSV / TSV files in current directory ─────────────
    static std::vector<std::string> listCsvFiles() {
        std::vector<std::string> files;
        try {
            for (auto& entry : std::filesystem::directory_iterator(".")) {
                if (!entry.is_regular_file()) continue;
                std::string ext = entry.path().extension().string();
                for (char& c : ext)
                    c = static_cast<char>(std::tolower(
                            static_cast<unsigned char>(c)));
                if (ext == ".csv" || ext == ".tsv")
                    files.push_back(entry.path().filename().string());
            }
        } catch (...) {}
        std::sort(files.begin(), files.end());
        return files;
    }
};

} // namespace milansql
