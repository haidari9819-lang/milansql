#pragma once

#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <cstdint>
#include <stdexcept>

#include "engine/engine.hpp"

// ============================================================
// storage.hpp — Persistenz-Schicht für MilanSQL
// Architektur ähnlich MariaDB: abstrakte StorageEngine,
// konkrete Implementierung: MilanStorage (JSON-Datei)
//
// Dateiformat: database.milan (JSON, UTF-8)
// {
//   "version": "1.0",
//   "tables": {
//     "trainer": {
//       "columns": [{"name":"id","type":"INT"}, ...],
//       "rows":    [["1","Glurak","Feuer"], ...]
//     }
//   }
// }
// ============================================================

namespace milansql {

// ------------------------------------------------------------
// StorageEngine: abstrakte Basisklasse
// Ermöglicht später weitere Backends (BinaryStorage, etc.)
// ------------------------------------------------------------
class StorageEngine {
public:
    virtual ~StorageEngine() = default;
    virtual void        save(const Engine& engine) = 0;
    virtual void        load(Engine& engine)        = 0;
    virtual std::string name() const                = 0;
};

// ------------------------------------------------------------
// MilanStorage: JSON-basierte Datei-Speicherung
// Schreibt/liest "database.milan" im Arbeitsverzeichnis
// ------------------------------------------------------------
class MilanStorage : public StorageEngine {
public:
    explicit MilanStorage(std::string path = "database.milan")
        : filepath_(std::move(path)) {}

    std::string name() const override { return "MilanStorage"; }

    // ── Alle Tabellen in JSON-Datei schreiben ─────────────────
    void save(const Engine& engine) override {
        std::ofstream f(filepath_);
        if (!f) throw std::runtime_error("Kann nicht schreiben: " + filepath_);
        f << toJson(engine);
        f.flush();
    }

    // ── JSON-Datei lesen und Engine befüllen ──────────────────
    // Gibt Anzahl geladener Tabellen zurück (0 = keine Datei)
    std::size_t loadWithCount(Engine& engine) {
        std::ifstream f(filepath_);
        if (!f) return 0;  // Datei existiert nicht → leere DB

        std::string content((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());
        return fromJson(content, engine);
    }

    void load(Engine& engine) override { loadWithCount(engine); }

private:
    std::string filepath_;

    // ============================================================
    // JSON SERIALIZER (Engine → String)
    // ============================================================
    std::string toJson(const Engine& engine) const {
        std::ostringstream o;
        o << "{\n";
        o << "  \"version\": \"1.0\",\n";
        o << "  \"tables\": {\n";

        const auto names = engine.getAllTableNamesInternal();
        for (std::size_t ti = 0; ti < names.size(); ++ti) {
            const Table& tbl = engine.selectAll(names[ti]);

            o << "    \"" << esc(tbl.name()) << "\": {\n";

            // Spalten
            o << "      \"columns\": [\n";
            const auto& cols = tbl.columns();
            for (std::size_t ci = 0; ci < cols.size(); ++ci) {
                o << "        {"
                  << "\"name\": \"" << esc(cols[ci].name) << "\", "
                  << "\"type\": \"" << esc(cols[ci].type) << "\"}";
                if (ci + 1 < cols.size()) o << ",";
                o << "\n";
            }
            o << "      ],\n";

            // Zeilen
            o << "      \"rows\": [\n";
            const auto& rows = tbl.rows();
            for (std::size_t ri = 0; ri < rows.size(); ++ri) {
                o << "        [";
                for (std::size_t vi = 0; vi < rows[ri].values.size(); ++vi) {
                    o << "\"" << esc(rows[ri].values[vi]) << "\"";
                    if (vi + 1 < rows[ri].values.size()) o << ", ";
                }
                o << "]";
                if (ri + 1 < rows.size()) o << ",";
                o << "\n";
            }
            o << "      ]\n";

            o << "    }";
            if (ti + 1 < names.size()) o << ",";
            o << "\n";
        }

        o << "  }\n}\n";
        return o.str();
    }

    // JSON-Sonderzeichen in Strings escapen
    static std::string esc(const std::string& s) {
        std::string r;
        r.reserve(s.size());
        for (unsigned char c : s) {
            switch (c) {
                case '"':  r += "\\\""; break;
                case '\\': r += "\\\\"; break;
                case '\n': r += "\\n";  break;
                case '\r': r += "\\r";  break;
                case '\t': r += "\\t";  break;
                default:   r += static_cast<char>(c);
            }
        }
        return r;
    }

    // ============================================================
    // JSON PARSER (String → Engine)
    // Einfacher rekursiver Abstiegs-Parser für unser festes Schema
    // ============================================================
    std::size_t fromJson(const std::string& j, Engine& engine) {
        std::size_t pos = 0;
        std::size_t loaded = 0;

        skip(j, pos);
        eat(j, pos, '{');

        while (pos < j.size() && j[pos] != '}') {
            skip(j, pos);
            std::string key = parseStr(j, pos);
            skip(j, pos);
            eat(j, pos, ':');
            skip(j, pos);

            if (key == "tables") {
                loaded = parseTables(j, pos, engine);
            } else {
                skipVal(j, pos);  // "version" und unbekannte Keys überspringen
            }

            skip(j, pos);
            if (pos < j.size() && j[pos] == ',') ++pos;
            skip(j, pos);
        }

        return loaded;
    }

    // Parst den "tables"-Block und befüllt die Engine
    std::size_t parseTables(const std::string& j, std::size_t& pos, Engine& engine) {
        eat(j, pos, '{');
        skip(j, pos);
        std::size_t count = 0;

        while (pos < j.size() && j[pos] != '}') {
            skip(j, pos);
            std::string tableName = parseStr(j, pos);
            skip(j, pos);
            eat(j, pos, ':');
            skip(j, pos);

            parseTableDef(j, pos, tableName, engine);
            ++count;

            skip(j, pos);
            if (pos < j.size() && j[pos] == ',') ++pos;
            skip(j, pos);
        }
        eat(j, pos, '}');
        return count;
    }

    // Parst eine einzelne Tabellen-Definition und fügt sie in die Engine ein
    void parseTableDef(const std::string& j, std::size_t& pos,
                       const std::string& tableName, Engine& engine) {
        eat(j, pos, '{');
        skip(j, pos);

        std::vector<Column>                  columns;
        std::vector<std::vector<std::string>> rows;

        while (pos < j.size() && j[pos] != '}') {
            skip(j, pos);
            std::string key = parseStr(j, pos);
            skip(j, pos);
            eat(j, pos, ':');
            skip(j, pos);

            if      (key == "columns") columns = parseCols(j, pos);
            else if (key == "rows")    rows    = parseRows(j, pos);
            else                        skipVal(j, pos);

            skip(j, pos);
            if (pos < j.size() && j[pos] == ',') ++pos;
            skip(j, pos);
        }
        eat(j, pos, '}');

        // Tabelle anlegen und Zeilen einfügen
        engine.createTable(tableName, columns);
        for (auto& rv : rows)
            engine.insertRow(tableName, rv);
    }

    // Parst das "columns"-Array: [{"name":"...","type":"..."}, ...]
    std::vector<Column> parseCols(const std::string& j, std::size_t& pos) {
        std::vector<Column> cols;
        eat(j, pos, '[');
        skip(j, pos);

        while (pos < j.size() && j[pos] != ']') {
            eat(j, pos, '{');
            skip(j, pos);
            std::string cname, ctype;

            while (pos < j.size() && j[pos] != '}') {
                skip(j, pos);
                std::string k = parseStr(j, pos);
                skip(j, pos); eat(j, pos, ':'); skip(j, pos);
                std::string v = parseStr(j, pos);
                if (k == "name") cname = v;
                if (k == "type") ctype = v;
                skip(j, pos);
                if (pos < j.size() && j[pos] == ',') ++pos;
                skip(j, pos);
            }
            eat(j, pos, '}');
            cols.emplace_back(cname, ctype);

            skip(j, pos);
            if (pos < j.size() && j[pos] == ',') ++pos;
            skip(j, pos);
        }
        eat(j, pos, ']');
        return cols;
    }

    // Parst das "rows"-Array: [["v1","v2"], ...]
    std::vector<std::vector<std::string>> parseRows(const std::string& j, std::size_t& pos) {
        std::vector<std::vector<std::string>> rows;
        eat(j, pos, '[');
        skip(j, pos);

        while (pos < j.size() && j[pos] != ']') {
            eat(j, pos, '[');
            skip(j, pos);
            std::vector<std::string> row;

            while (pos < j.size() && j[pos] != ']') {
                skip(j, pos);
                row.push_back(parseStr(j, pos));
                skip(j, pos);
                if (pos < j.size() && j[pos] == ',') ++pos;
                skip(j, pos);
            }
            eat(j, pos, ']');
            rows.push_back(std::move(row));

            skip(j, pos);
            if (pos < j.size() && j[pos] == ',') ++pos;
            skip(j, pos);
        }
        eat(j, pos, ']');
        return rows;
    }

    // Liest einen JSON-String "..." mit Escape-Unterstützung
    std::string parseStr(const std::string& j, std::size_t& pos) {
        skip(j, pos);
        if (pos >= j.size() || j[pos] != '"')
            throw std::runtime_error("JSON: '\"' erwartet bei pos=" + std::to_string(pos));
        ++pos;
        std::string r;
        while (pos < j.size() && j[pos] != '"') {
            if (j[pos] == '\\') {
                ++pos;
                if (pos >= j.size()) break;
                switch (j[pos]) {
                    case '"':  r += '"';  break;
                    case '\\': r += '\\'; break;
                    case 'n':  r += '\n'; break;
                    case 'r':  r += '\r'; break;
                    case 't':  r += '\t'; break;
                    default:   r += j[pos]; break;
                }
            } else {
                r += j[pos];
            }
            ++pos;
        }
        if (pos < j.size()) ++pos;  // schließendes "
        return r;
    }

    // Whitespace überspringen
    static void skip(const std::string& j, std::size_t& pos) {
        while (pos < j.size() &&
               (j[pos]==' ' || j[pos]=='\t' || j[pos]=='\n' || j[pos]=='\r'))
            ++pos;
    }

    // Erwartet ein bestimmtes Zeichen und konsumiert es
    static void eat(const std::string& j, std::size_t& pos, char c) {
        skip(j, pos);
        if (pos >= j.size() || j[pos] != c)
            throw std::runtime_error(
                std::string("JSON: '") + c + "' erwartet bei pos=" +
                std::to_string(pos) +
                (pos < j.size() ? (", gefunden '" + std::string(1, j[pos]) + "'") : " (EOF)"));
        ++pos;
    }

    // Überspringt einen beliebigen JSON-Wert (für unbekannte Keys)
    void skipVal(const std::string& j, std::size_t& pos) {
        skip(j, pos);
        if (pos >= j.size()) return;

        if (j[pos] == '"') { parseStr(j, pos); return; }

        if (j[pos] == '{' || j[pos] == '[') {
            char open  = j[pos];
            char close = (open == '{') ? '}' : ']';
            ++pos;
            int depth = 1;
            while (pos < j.size() && depth > 0) {
                if      (j[pos] == '"')   { --pos; parseStr(j, pos); }
                else if (j[pos] == open)  ++depth;
                else if (j[pos] == close) --depth;
                if (depth > 0) ++pos;
            }
            if (pos < j.size()) ++pos;
            return;
        }

        // Zahlen oder true/false/null
        while (pos < j.size() &&
               j[pos] != ',' && j[pos] != '}' && j[pos] != ']' &&
               j[pos] != '\n')
            ++pos;
    }
};

// ============================================================
// MilanBinaryStorage — Echtes Binärformat, inspiriert von InnoDB
//
// FILE LAYOUT (Byte-genaue Beschreibung):
//   Bytes  0.. 7  Magic "MILANDB1"  (8 Bytes)
//   Bytes  8.. 9  Version           uint16_t LE
//   Bytes 10..13  Page Count        uint32_t LE
//   Bytes 14..15  Checksum          uint16_t LE  (XOR über Data-Section)
//   Bytes 16..    DATA SECTION (TABLE DIRECTORY PAGE):
//     uint16_t  Anzahl Tabellen
//     Je Tabelle:
//       uint8_t  len + char[len]     Tabellenname
//       uint8_t  Anzahl Spalten
//       Je Spalte:
//         uint8_t  len + char[len]   Spaltenname
//         uint8_t  Typ (0=INT, 1=TEXT)
//       uint32_t  Anzahl Zeilen
//       Je Zeile, je Wert:
//         uint16_t len + char[len]   Wert (uint16 für lange Strings)
// ============================================================
class MilanBinaryStorage : public StorageEngine {
public:
    // Magic Bytes und Versionskonstanten
    static constexpr uint16_t FORMAT_VERSION = 8;  // Phase 68: Generated Columns
    static constexpr const char* MAGIC = "MILANDB1";  // 8 Bytes

    explicit MilanBinaryStorage(std::string path = "database.milan")
        : filepath_(std::move(path)) {}

    std::string name() const override { return "MilanBinaryStorage v1"; }

    // ── Schreiben ─────────────────────────────────────────────
    void save(const Engine& engine) override {
        // 1. Data-Section in Puffer serialisieren
        std::ostringstream buf;
        serializeData(buf, engine);
        std::string data = buf.str();

        // 2. Checksumme über Data-Section berechnen
        uint16_t cs = checksum(data);

        // 3. Datei schreiben (binär)
        std::ofstream f(filepath_, std::ios::binary | std::ios::trunc);
        if (!f) throw std::runtime_error("Kann nicht schreiben: " + filepath_);

        // File Header (genau 16 Bytes)
        f.write(MAGIC, 8);                           // Magic
        writeU16(f, FORMAT_VERSION);                 // Version
        writeU32(f, 1u);                             // Page Count = 1
        writeU16(f, cs);                             // Checksum

        // Data Section
        f.write(data.data(), static_cast<std::streamsize>(data.size()));

        if (!f) throw std::runtime_error("Schreibfehler: " + filepath_);
    }

    // ── Lesen ─────────────────────────────────────────────────
    // Gibt Anzahl geladener Tabellen zurück (0 = keine Datei)
    std::size_t loadWithCount(Engine& engine) {
        std::ifstream f(filepath_, std::ios::binary);
        if (!f) return 0;   // Datei nicht vorhanden → leere DB

        // Gesamte Datei in einen String-Puffer einlesen
        std::string buf((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());

        if (buf.size() < 16)
            throw std::runtime_error("Datei zu kurz fuer gueltigen MilanSQL-Header");

        // ── FILE HEADER aus Puffer parsen (Bytes 0..15) ──────
        std::size_t pos = 0;

        // Magic (8 Bytes)
        std::string magic(buf.data(), 8);
        pos = 8;
        if (magic != MAGIC)
            throw std::runtime_error("Keine gueltige MilanSQL-Datei (falscher Magic)");

        uint16_t version  = rU16(buf, pos);   // Bytes  8..9
        rU32(buf, pos);                        // Bytes 10..13  (Page Count, reserviert)
        uint16_t storedCs = rU16(buf, pos);   // Bytes 14..15

        if (version != FORMAT_VERSION && version != 7)
            throw std::runtime_error(
                "Inkompatible Version: gespeichert=" + std::to_string(version) +
                ", erwartet=" + std::to_string(FORMAT_VERSION));

        // ── Data-Section: Bytes 16.. ──────────────────────────
        std::string data = buf.substr(16);

        if (checksum(data) != storedCs)
            std::cerr << "  WARNUNG: Checksumme fehlerhaft — Datei moeglicherweise korrupt!\n";

        // ── Deserialisieren ───────────────────────────────────
        std::size_t dataPos = 0;
        return deserializeData(data, dataPos, engine, version);
    }

    void load(Engine& engine) override { loadWithCount(engine); }

private:
    std::string filepath_;

    // ============================================================
    // SERIALIZER (Engine → Binär-Puffer)
    // ============================================================
    static void serializeData(std::ostream& o, const Engine& engine) {
        const auto names = engine.getAllTableNamesInternal();
        writeU16(o, static_cast<uint16_t>(names.size()));

        for (const auto& tname : names) {
            const Table& tbl = engine.selectAll(tname);

            writeStrN(o, tbl.name());                           // Tabellenname

            const auto& cols = tbl.columns();
            writeU8(o, static_cast<uint8_t>(cols.size()));     // Spaltenanzahl
            for (const auto& col : cols) {
                writeStrN(o, col.name);                         // Spaltenname
                writeU8(o, encodeType(col.type));               // Typ-Byte
                // Constraint-Flags:
                //   bit0=NOT NULL  bit1=UNIQUE  bit2=has DEFAULT
                //   bit3=PRIMARY KEY  bit4=AUTO_INCREMENT
                uint8_t flags = 0;
                if (col.notNull)       flags |= 0x01;
                if (col.isUnique)      flags |= 0x02;
                if (col.hasDefault)    flags |= 0x04;
                if (col.isPrimaryKey)  flags |= 0x08;
                if (col.autoIncrement) flags |= 0x10;
                writeU8(o, flags);
                if (col.hasDefault)    writeStrV(o, col.defaultValue);
                if (col.autoIncrement) writeU64(o, tbl.peekAutoInc(col.name));
                // Phase 23: CHECK constraints
                writeU8(o, static_cast<uint8_t>(col.checks.size()));
                for (const auto& cc : col.checks) {
                    writeStrN(o, cc.op);
                    writeStrN(o, cc.val);
                }
                // Phase 68: Generated Columns
                writeU8(o, col.isGenerated ? 1 : 0);
                if (col.isGenerated) {
                    writeStrV(o, col.generatedExpr);
                    writeU8(o, col.isStored ? 1 : 0);
                }
            }

            const auto& rows = tbl.rows();
            writeU32(o, static_cast<uint32_t>(rows.size()));   // Zeilenanzahl
            for (const auto& row : rows) {
                for (const auto& val : row.values) {
                    writeStrV(o, val);                          // Wert (uint16 Länge)
                }
            }

            // Phase 20/21: FOREIGN KEY Definitionen (inkl. ON DELETE)
            const auto& fks = tbl.getForeignKeys();
            writeU8(o, static_cast<uint8_t>(fks.size()));
            for (const auto& fk : fks) {
                writeStrN(o, fk.fromCol);
                writeStrN(o, fk.refTable);
                writeStrN(o, fk.refCol);
                writeU8(o, encodeOnDelete(fk.onDelete));  // Phase 21
            }
        }

        // Phase 24: Views serialisieren
        const auto viewNames = engine.getAllViewNames();
        writeU16(o, static_cast<uint16_t>(viewNames.size()));
        for (const auto& vname : viewNames) {
            writeStrN(o, vname);
            writeStrV(o, engine.getViewSql(vname));
        }
    }

    // ============================================================
    // DESERIALIZER (Binär-Puffer → Engine)
    // ============================================================
    static std::size_t deserializeData(const std::string& d,
                                       std::size_t& pos, Engine& engine,
                                       uint16_t version = FORMAT_VERSION) {
        uint16_t tableCount = rU16(d, pos);

        for (uint16_t ti = 0; ti < tableCount; ++ti) {
            std::string tname  = rStrN(d, pos);
            uint8_t    colCnt  = rU8(d, pos);

            std::vector<Column>   cols;
            std::vector<uint64_t> autoInc;
            cols.reserve(colCnt);
            autoInc.reserve(colCnt);
            for (uint8_t ci = 0; ci < colCnt; ++ci) {
                std::string cname = rStrN(d, pos);
                std::string ctype = decodeType(rU8(d, pos));
                Column col(cname, ctype);
                // Constraint-Flags lesen (Phase 19, Format v3)
                uint8_t flags      = rU8(d, pos);
                col.notNull        = (flags & 0x01) != 0;
                col.isUnique       = (flags & 0x02) != 0;
                col.hasDefault     = (flags & 0x04) != 0;
                col.isPrimaryKey   = (flags & 0x08) != 0;
                col.autoIncrement  = (flags & 0x10) != 0;
                if (col.hasDefault)    col.defaultValue = rStrV(d, pos);
                autoInc.push_back(col.autoIncrement ? rU64(d, pos) : 0);
                // Phase 23: CHECK constraints
                uint8_t checkCnt = rU8(d, pos);
                for (uint8_t chi = 0; chi < checkCnt; ++chi) {
                    CheckConstraint cc;
                    cc.op  = rStrN(d, pos);
                    cc.val = rStrN(d, pos);
                    col.checks.push_back(cc);
                }
                // Phase 68: Generated Columns (version >= 8)
                if (version >= 8) {
                    col.isGenerated = (rU8(d, pos) != 0);
                    if (col.isGenerated) {
                        col.generatedExpr = rStrV(d, pos);
                        col.isStored      = (rU8(d, pos) != 0);
                    }
                }
                cols.push_back(std::move(col));
            }

            uint32_t rowCnt = rU32(d, pos);
            engine.createTable(tname, cols);

            // AUTO_INCREMENT-Zähler wiederherstellen
            for (size_t ci = 0; ci < cols.size(); ++ci)
                if (autoInc[ci] > 0)
                    engine.setTableAutoInc(tname, cols[ci].name, autoInc[ci]);

            for (uint32_t ri = 0; ri < rowCnt; ++ri) {
                std::vector<std::string> vals;
                vals.reserve(cols.size());
                for (std::size_t ci = 0; ci < cols.size(); ++ci) {
                    vals.push_back(rStrV(d, pos));
                }
                engine.insertRow(tname, vals);
            }

            // Phase 20/21: FOREIGN KEY Definitionen laden (inkl. ON DELETE)
            uint8_t fkCnt = rU8(d, pos);
            for (uint8_t fi = 0; fi < fkCnt; ++fi) {
                ForeignKeyDef fk;
                fk.fromCol  = rStrN(d, pos);
                fk.refTable = rStrN(d, pos);
                fk.refCol   = rStrN(d, pos);
                fk.onDelete = decodeOnDelete(rU8(d, pos));  // Phase 21
                engine.addForeignKey(tname, fk);
            }
        }

        // Phase 24: Views laden
        uint16_t viewCount = rU16(d, pos);
        for (uint16_t vi = 0; vi < viewCount; ++vi) {
            std::string vname = rStrN(d, pos);
            std::string vsql  = rStrV(d, pos);
            engine.createView(vname, vsql);
        }

        return tableCount;
    }

    // ============================================================
    // WRITE HELPERS — alle Little-Endian
    // ============================================================
    static void writeU8(std::ostream& o, uint8_t v) {
        o.put(static_cast<char>(v));
    }

    static void writeU16(std::ostream& o, uint16_t v) {
        o.put(static_cast<char>(v & 0xFF));
        o.put(static_cast<char>((v >> 8) & 0xFF));
    }

    static void writeU32(std::ostream& o, uint32_t v) {
        o.put(static_cast<char>(v & 0xFF));
        o.put(static_cast<char>((v >>  8) & 0xFF));
        o.put(static_cast<char>((v >> 16) & 0xFF));
        o.put(static_cast<char>((v >> 24) & 0xFF));
    }

    // Name-String: uint8_t Länge + Daten (max 255 Zeichen)
    static void writeStrN(std::ostream& o, const std::string& s) {
        if (s.size() > 255)
            throw std::runtime_error("Name zu lang (max 255): " + s);
        writeU8(o, static_cast<uint8_t>(s.size()));
        o.write(s.data(), static_cast<std::streamsize>(s.size()));
    }

    static void writeU64(std::ostream& o, uint64_t v) {
        for (int i = 0; i < 8; ++i)
            o.put(static_cast<char>((v >> (i * 8)) & 0xFF));
    }

    // Value-String: uint16_t Länge + Daten (max 65535 Zeichen)
    static void writeStrV(std::ostream& o, const std::string& s) {
        if (s.size() > 65535)
            throw std::runtime_error("Wert zu lang (max 65535)");
        writeU16(o, static_cast<uint16_t>(s.size()));
        o.write(s.data(), static_cast<std::streamsize>(s.size()));
    }

    // ============================================================
    // READ HELPERS — aus std::string-Puffer mit Positions-Cursor
    // ============================================================
    static void checkBounds(const std::string& d, std::size_t pos, std::size_t need) {
        if (pos + need > d.size())
            throw std::runtime_error(
                "Binary: unerwartetes EOF bei pos=" + std::to_string(pos));
    }

    static uint8_t rU8(const std::string& d, std::size_t& p) {
        checkBounds(d, p, 1);
        return static_cast<uint8_t>(d[p++]);
    }

    static uint16_t rU16(const std::string& d, std::size_t& p) {
        checkBounds(d, p, 2);
        uint16_t v = static_cast<uint8_t>(d[p])
                   | (static_cast<uint8_t>(d[p+1]) << 8);
        p += 2;
        return v;
    }

    static uint32_t rU32(const std::string& d, std::size_t& p) {
        checkBounds(d, p, 4);
        uint32_t v = static_cast<uint8_t>(d[p])
                   | (static_cast<uint32_t>(static_cast<uint8_t>(d[p+1])) <<  8)
                   | (static_cast<uint32_t>(static_cast<uint8_t>(d[p+2])) << 16)
                   | (static_cast<uint32_t>(static_cast<uint8_t>(d[p+3])) << 24);
        p += 4;
        return v;
    }

    // uint8_t Länge lesen, dann String
    static std::string rStrN(const std::string& d, std::size_t& p) {
        uint8_t len = rU8(d, p);
        checkBounds(d, p, len);
        std::string s(d.data() + p, len);
        p += len;
        return s;
    }

    static uint64_t rU64(const std::string& d, std::size_t& p) {
        checkBounds(d, p, 8);
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i)
            v |= static_cast<uint64_t>(static_cast<uint8_t>(d[p + i])) << (i * 8);
        p += 8;
        return v;
    }

    // uint16_t Länge lesen, dann String
    static std::string rStrV(const std::string& d, std::size_t& p) {
        uint16_t len = rU16(d, p);
        checkBounds(d, p, len);
        std::string s(d.data() + p, len);
        p += len;
        return s;
    }

    // ============================================================
    // ON DELETE-KODIERUNG  (Phase 21)
    // ============================================================
    static uint8_t encodeOnDelete(const std::string& action) {
        if (action == "CASCADE")  return 1;
        if (action == "SET NULL") return 2;
        return 0;  // RESTRICT (default)
    }

    static std::string decodeOnDelete(uint8_t v) {
        switch (v) {
            case 1: return "CASCADE";
            case 2: return "SET NULL";
            default: return "RESTRICT";
        }
    }

    // ============================================================
    // TYP-KODIERUNG
    // ============================================================
    static uint8_t encodeType(const std::string& t) {
        if (t == "INT")  return 0;
        if (t == "TEXT") return 1;
        return 1;   // Fallback: TEXT
    }

    static std::string decodeType(uint8_t t) {
        switch (t) {
            case 0: return "INT";
            case 1: return "TEXT";
            default: return "TEXT";
        }
    }

    // ============================================================
    // CHECKSUMME — 16-Bit XOR (alternierend Low/High-Byte)
    // Erkennt Einzelbit-Fehler zuverlässig
    // ============================================================
    static uint16_t checksum(const std::string& data) {
        uint16_t cs = 0;
        for (std::size_t i = 0; i < data.size(); ++i) {
            // Gerade Bytes ins Low-Byte, ungerade ins High-Byte XOR-en
            if (i & 1)
                cs ^= static_cast<uint16_t>(static_cast<uint8_t>(data[i]) << 8);
            else
                cs ^= static_cast<uint8_t>(data[i]);
        }
        return cs;
    }
};

} // namespace milansql
