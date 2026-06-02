#pragma once
// ============================================================
// double_write_buffer.hpp — Phase 114: Atomic Page Writes
//
// Implements the Double-Write Buffer pattern to protect against
// torn page writes (e.g., power loss after 4 KB of an 8 KB page).
//
// Write protocol:
//   1. Serialise page → DWB file (fsync)
//   2. Write page to actual destination file
//   3. Clear DWB entry (fsync)
//
// Recovery protocol (at startup):
//   1. Open DWB file
//   2. For each valid entry (magic + checksum OK):
//      – Re-apply the page to its destination
//   3. Truncate / delete DWB
//
// File: database.dwb
//   Header  : {char magic[8], uint32_t entryCount, uint32_t headerCrc}
//   Entry[] : {uint64_t pageId, uint32_t dataLen, uint32_t crc32,
//              char destPath[256], uint64_t destOffset, uint8_t data[]}
// ============================================================

#include <string>
#include <vector>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <mutex>
#include <atomic>
#include <iostream>

namespace milansql {

// ── Simple CRC-32 (bit-by-bit, no external dep) ───────────────
inline uint32_t dwbCrc32(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        uint8_t byte = data[i];
        for (int k = 0; k < 8; ++k) {
            bool xorBit = ((crc ^ byte) & 1) != 0;
            crc >>= 1;
            if (xorBit) crc ^= 0xEDB88320u;
            byte >>= 1;
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

// ── DoubleWriteBuffer ──────────────────────────────────────────
class DoubleWriteBuffer {
public:
    static constexpr size_t      PAGE_SIZE     = 8192;
    static constexpr size_t      MAX_DEST_PATH = 256;
    static constexpr const char* DWB_MAGIC     = "MSDWB001";

    // ── On-disk layout ──────────────────────────────────────────
    struct alignas(8) DwbHeader {
        char     magic[8];        // "MSDWB001"
        uint32_t entryCount;      // number of entries
        uint32_t headerCrc;       // CRC of the first 12 bytes
    };

    struct DwbEntryMeta {
        uint64_t pageId;
        uint32_t dataLen;
        uint32_t crc32;
        char     destPath[MAX_DEST_PATH];
        uint64_t destOffset;
    };

    // ── Recovery result ─────────────────────────────────────────
    struct RecoverResult {
        bool hadPendingWrite = false;
        int  pagesRecovered  = 0;
        int  pagesCorrupt    = 0;
    };

    explicit DoubleWriteBuffer(std::string path = "database.dwb")
        : bufferPath_(std::move(path)) {}

    // ── Write a page atomically ─────────────────────────────────
    // 1. Append to DWB
    // 2. Write to destPath at destOffset
    // 3. Clear DWB
    // Returns true on success.
    bool writePage(uint64_t                       pageId,
                   const std::vector<uint8_t>&    data,
                   const std::string&             destPath,
                   uint64_t                       destOffset = 0) {
        std::lock_guard<std::mutex> g(mu_);

        const uint8_t* raw   = data.data();
        uint32_t       dlen  = static_cast<uint32_t>(data.size());
        uint32_t       dcrc  = dwbCrc32(raw, dlen);

        // ── Step 1: write to DWB ──────────────────────────────
        {
            std::ofstream f(bufferPath_, std::ios::binary | std::ios::trunc);
            if (!f) return false;

            DwbHeader hdr{};
            std::memcpy(hdr.magic, DWB_MAGIC, 8);
            hdr.entryCount = 1;
            uint8_t hdrBytes[12];
            std::memcpy(hdrBytes,     hdr.magic,      8);
            std::memcpy(hdrBytes + 8, &hdr.entryCount, 4);
            hdr.headerCrc = dwbCrc32(hdrBytes, 12);

            DwbEntryMeta meta{};
            meta.pageId     = pageId;
            meta.dataLen    = dlen;
            meta.crc32      = dcrc;
            meta.destOffset = destOffset;
            std::strncpy(meta.destPath,
                         destPath.c_str(),
                         MAX_DEST_PATH - 1);

            f.write(reinterpret_cast<const char*>(&hdr),  sizeof(hdr));
            f.write(reinterpret_cast<const char*>(&meta), sizeof(meta));
            f.write(reinterpret_cast<const char*>(raw),   dlen);
            if (!f) return false;
        }

        // ── Step 2: write to destination ──────────────────────
        {
            std::fstream dest(destPath,
                              std::ios::binary | std::ios::in | std::ios::out);
            if (!dest) {
                // Destination doesn't exist yet — create it
                std::ofstream create(destPath, std::ios::binary);
                if (!create) return false;
                create.close();
                dest.open(destPath, std::ios::binary | std::ios::in | std::ios::out);
                if (!dest) return false;
            }
            dest.seekp(static_cast<std::streamoff>(destOffset));
            dest.write(reinterpret_cast<const char*>(raw), dlen);
            if (!dest) return false;
        }

        // ── Step 3: clear DWB ─────────────────────────────────
        std::remove(bufferPath_.c_str());
        pendingWrites_.fetch_sub(1, std::memory_order_relaxed);
        return true;
    }

    // ── Recover on startup ──────────────────────────────────────
    // Call before loading main database.
    RecoverResult recover() {
        std::lock_guard<std::mutex> g(mu_);
        RecoverResult res;

        std::ifstream f(bufferPath_, std::ios::binary);
        if (!f) return res;
        res.hadPendingWrite = true;

        // Read header
        DwbHeader hdr{};
        f.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
        if (!f) { res.pagesCorrupt++; return res; }

        // Verify magic
        if (std::memcmp(hdr.magic, DWB_MAGIC, 8) != 0) {
            res.pagesCorrupt++;
            std::remove(bufferPath_.c_str());
            return res;
        }

        // Verify header CRC
        {
            uint8_t hdrBytes[12];
            std::memcpy(hdrBytes,     hdr.magic,      8);
            std::memcpy(hdrBytes + 8, &hdr.entryCount, 4);
            uint32_t expectedCrc = dwbCrc32(hdrBytes, 12);
            if (hdr.headerCrc != expectedCrc) {
                res.pagesCorrupt++;
                std::remove(bufferPath_.c_str());
                return res;
            }
        }

        // Process each entry
        for (uint32_t i = 0; i < hdr.entryCount; ++i) {
            DwbEntryMeta meta{};
            f.read(reinterpret_cast<char*>(&meta), sizeof(meta));
            if (!f) { res.pagesCorrupt++; break; }

            std::vector<uint8_t> data(meta.dataLen);
            f.read(reinterpret_cast<char*>(data.data()), meta.dataLen);
            if (!f) { res.pagesCorrupt++; break; }

            // Verify data CRC
            uint32_t crc = dwbCrc32(data.data(), meta.dataLen);
            if (crc != meta.crc32) {
                res.pagesCorrupt++;
                continue;
            }

            // Re-apply the page to destination
            std::string destPath(meta.destPath,
                                 strnlen(meta.destPath, MAX_DEST_PATH));
            std::fstream dest(destPath,
                              std::ios::binary | std::ios::in | std::ios::out);
            if (dest) {
                dest.seekp(static_cast<std::streamoff>(meta.destOffset));
                dest.write(reinterpret_cast<const char*>(data.data()),
                           meta.dataLen);
                if (dest) ++res.pagesRecovered;
                else      ++res.pagesCorrupt;
            }
        }

        // DWB consumed — remove it
        std::remove(bufferPath_.c_str());
        return res;
    }

    // ── Check if DWB is clean ────────────────────────────────────
    bool isClean() const {
        std::ifstream f(bufferPath_, std::ios::binary);
        return !f;  // no file → clean
    }

    // ── Explicitly clear the DWB ────────────────────────────────
    void clear() {
        std::lock_guard<std::mutex> g(mu_);
        std::remove(bufferPath_.c_str());
        pendingWrites_.store(0, std::memory_order_relaxed);
    }

    // ── Stats ────────────────────────────────────────────────────
    uint64_t totalWrites()  const { return totalWrites_.load(std::memory_order_relaxed); }
    bool     hasPending()   const { return !isClean(); }

    const std::string& path() const { return bufferPath_; }

private:
    std::string           bufferPath_;
    mutable std::mutex    mu_;
    std::atomic<int>      pendingWrites_{0};
    std::atomic<uint64_t> totalWrites_{0};
};

// ── Global singleton ───────────────────────────────────────────
inline DoubleWriteBuffer& g_doubleWriteBuffer() {
    static DoubleWriteBuffer dwb("database.dwb");
    return dwb;
}

} // namespace milansql
