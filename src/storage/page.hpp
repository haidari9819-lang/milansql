#pragma once
// ============================================================
// page.hpp — Phase 84: 8 KB Page Format
// Included inside namespace milansql in engine.hpp (after Table
// class). Do NOT add a namespace milansql wrapper here.
//
// Page layout (PAGE_SIZE = 8192 bytes):
//   [0-7]   pageId       uint64_t
//   [8-35]  tableName    char[28]
//   [36-37] rowCount     uint16_t
//   [38-39] writePos     uint16_t  (next free byte in data area)
//   [40-47] nextPageId   uint64_t  (NO_NEXT = no next page)
//   [48-51] checksum     uint32_t
//   [52-63] reserved     12 bytes
//   [64-8191] data area  8128 bytes
//
// Row format inside data area:
//   [uint16_t numCols][uint16_t len + bytes] × numCols
// ============================================================

struct Page {
    // ── Constants ────────────────────────────────────────────
    static constexpr uint32_t PAGE_SIZE   = 8192;
    static constexpr uint32_t HEADER_SIZE = 64;
    static constexpr uint32_t DATA_SIZE   = PAGE_SIZE - HEADER_SIZE; // 8128
    static constexpr uint64_t NO_NEXT     = UINT64_MAX;

    // ── Header field byte offsets ─────────────────────────────
    static constexpr size_t OFF_PAGE_ID   =  0;  // uint64_t  (8 bytes)
    static constexpr size_t OFF_TBL_NAME  =  8;  // char[28]  (28 bytes)
    static constexpr size_t OFF_ROW_COUNT = 36;  // uint16_t  (2 bytes)
    static constexpr size_t OFF_WRITE_POS = 38;  // uint16_t  (2 bytes)
    static constexpr size_t OFF_NEXT_PAGE = 40;  // uint64_t  (8 bytes)
    static constexpr size_t OFF_CHECKSUM  = 48;  // uint32_t  (4 bytes)
    // [52-63] reserved

    uint8_t raw_[PAGE_SIZE];  // single flat byte array for the entire page

    // ── Initialise a new empty page ───────────────────────────
    void init(uint64_t pageId, const std::string& tblName) {
        std::memset(raw_, 0, PAGE_SIZE);
        setPageId(pageId);
        setTableName(tblName);
        setRowCount(0);
        setWritePos(0);
        setNextPageId(NO_NEXT);
        updateChecksum();
    }

    // ── Header accessors ─────────────────────────────────────

    uint64_t pageId() const {
        uint64_t v; std::memcpy(&v, raw_ + OFF_PAGE_ID, 8); return v;
    }
    void setPageId(uint64_t v) { std::memcpy(raw_ + OFF_PAGE_ID, &v, 8); }

    std::string tableName() const {
        // Find null terminator within the 28-byte field
        const char* p = reinterpret_cast<const char*>(raw_ + OFF_TBL_NAME);
        size_t len = 0;
        while (len < 28 && p[len]) ++len;
        return std::string(p, len);
    }
    void setTableName(const std::string& s) {
        std::memset(raw_ + OFF_TBL_NAME, 0, 28);
        size_t n = s.size() < 27 ? s.size() : 27;
        std::memcpy(raw_ + OFF_TBL_NAME, s.c_str(), n);
    }

    uint16_t rowCount() const {
        uint16_t v; std::memcpy(&v, raw_ + OFF_ROW_COUNT, 2); return v;
    }
    void setRowCount(uint16_t v) { std::memcpy(raw_ + OFF_ROW_COUNT, &v, 2); }

    uint16_t writePos() const {
        uint16_t v; std::memcpy(&v, raw_ + OFF_WRITE_POS, 2); return v;
    }
    void setWritePos(uint16_t v) { std::memcpy(raw_ + OFF_WRITE_POS, &v, 2); }

    uint64_t nextPageId() const {
        uint64_t v; std::memcpy(&v, raw_ + OFF_NEXT_PAGE, 8); return v;
    }
    void setNextPageId(uint64_t v) { std::memcpy(raw_ + OFF_NEXT_PAGE, &v, 8); }

    uint32_t storedChecksum() const {
        uint32_t v; std::memcpy(&v, raw_ + OFF_CHECKSUM, 4); return v;
    }
    void setChecksum(uint32_t v) { std::memcpy(raw_ + OFF_CHECKSUM, &v, 4); }

    // ── Checksum (XOR over 4-byte chunks of the data area) ───

    uint32_t computeChecksum() const {
        uint32_t cs = 0;
        const uint8_t* d = raw_ + HEADER_SIZE;
        for (uint32_t i = 0; i + 3 < DATA_SIZE; i += 4) {
            uint32_t chunk; std::memcpy(&chunk, d + i, 4);
            cs ^= chunk;
        }
        return cs;
    }
    void updateChecksum()        { setChecksum(computeChecksum()); }
    bool verifyChecksum()  const { return storedChecksum() == computeChecksum(); }

    // ── Row I/O ───────────────────────────────────────────────

    // Append a row to the data area.
    // Returns true on success, false if the page is full.
    bool addRow(const Row& row) {
        // Compute required bytes: [uint16 numCols] + ([uint16 len][bytes]×numCols)
        uint32_t needed = 2;
        for (const auto& v : row.values)
            needed += 2 + static_cast<uint32_t>(v.size());
        if (needed > DATA_SIZE) return false;  // row too large for any page

        uint16_t wp = writePos();
        if (static_cast<uint32_t>(wp) + needed > DATA_SIZE) return false;

        uint8_t* dst = raw_ + HEADER_SIZE + wp;
        uint16_t nc  = static_cast<uint16_t>(row.values.size());
        std::memcpy(dst, &nc, 2); dst += 2;
        for (const auto& v : row.values) {
            uint16_t len = static_cast<uint16_t>(v.size());
            std::memcpy(dst, &len, 2); dst += 2;
            if (len) std::memcpy(dst, v.data(), len);
            dst += len;
        }
        setWritePos(static_cast<uint16_t>(wp + needed));
        setRowCount(rowCount() + 1);
        updateChecksum();
        return true;
    }

    // Read all rows from the data area.
    std::vector<Row> getRows() const {
        std::vector<Row> result;
        const uint8_t* src = raw_ + HEADER_SIZE;
        uint32_t pos   = 0;
        uint16_t wp    = writePos();
        uint16_t count = rowCount();

        while (pos < static_cast<uint32_t>(wp) && result.size() < count) {
            if (pos + 2 > DATA_SIZE) break;
            uint16_t nc; std::memcpy(&nc, src + pos, 2); pos += 2;

            std::vector<std::string> vals;
            vals.reserve(nc);
            bool ok = true;
            for (uint16_t c = 0; c < nc; ++c) {
                if (pos + 2 > DATA_SIZE) { ok = false; break; }
                uint16_t vlen; std::memcpy(&vlen, src + pos, 2); pos += 2;
                if (pos + vlen > DATA_SIZE) { ok = false; break; }
                vals.emplace_back(reinterpret_cast<const char*>(src + pos), vlen);
                pos += vlen;
            }
            if (!ok) break;
            result.emplace_back(std::move(vals));
        }
        return result;
    }
};
