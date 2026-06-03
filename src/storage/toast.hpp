#pragma once
#include <string>
#include <map>
#include <vector>
#include <cstdint>
#include <algorithm>
#include <sstream>

// ============================================================
// toast.hpp — Phase 131: TOAST Large Object Storage
// ============================================================

namespace milansql {

struct ToastEntry {
    uint64_t oid = 0;
    std::string data;      // original (possibly compressed) data
    bool compressed = false;
    size_t originalSize = 0;
};

class ToastManager {
public:
    static constexpr size_t TOAST_THRESHOLD = 2048;          // 2KB
    static constexpr size_t TOAST_EXTERNAL_THRESHOLD = 8192; // 8KB

    // Simple LZ4-like compression using run-length encoding for test purposes
    static std::string simpleCompress(const std::string& data) {
        std::string out;
        size_t i = 0;
        while (i < data.size()) {
            char c = data[i];
            size_t count = 1;
            while (i + count < data.size() && data[i+count] == c && count < 255) count++;
            if (count > 3) {
                out += '\x01'; // escape
                out += static_cast<char>(count);
                out += c;
            } else {
                for (size_t k = 0; k < count; k++) out += c;
            }
            i += count;
        }
        return out;
    }

    static std::string simpleDecompress(const std::string& data) {
        std::string out;
        for (size_t i = 0; i < data.size(); i++) {
            if (data[i] == '\x01' && i+2 < data.size()) {
                size_t count = static_cast<unsigned char>(data[i+1]);
                out += std::string(count, data[i+2]);
                i += 2;
            } else {
                out += data[i];
            }
        }
        return out;
    }

    // Base64 encode
    static std::string base64Encode(const std::string& data) {
        static const char* chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string result;
        int val=0, bits=-6;
        for (unsigned char c : data) {
            val = (val<<8) + c;
            bits += 8;
            while (bits >= 0) {
                result += chars[(val>>bits)&0x3F];
                bits -= 6;
            }
        }
        if (bits > -6) result += chars[((val<<8)>>(bits+8))&0x3F];
        while (result.size()%4) result += '=';
        return result;
    }

    bool shouldToast(const std::string& value) const {
        return value.size() > TOAST_THRESHOLD;
    }

    // Store value in TOAST table, return reference string "__toast:<oid>"
    std::string toastValue(const std::string& value) {
        uint64_t oid = nextOid_++;
        ToastEntry entry;
        entry.oid = oid;
        entry.originalSize = value.size();
        if (value.size() > TOAST_EXTERNAL_THRESHOLD) {
            // Store as-is (EXTERNAL)
            entry.data = value;
            entry.compressed = false;
        } else {
            // Compress (EXTENDED)
            entry.data = simpleCompress(value);
            entry.compressed = true;
        }
        toastTable_[oid] = entry;
        long long saved = static_cast<long long>(value.size()) - static_cast<long long>(entry.data.size());
        totalSavedBytes_ += saved;
        toastedCount_++;
        return "__toast:" + std::to_string(oid);
    }

    // Fetch original value from TOAST reference
    std::string fetchToast(const std::string& ref) const {
        if (ref.rfind("__toast:", 0) != 0) return ref; // not a toast ref
        uint64_t oid = std::stoull(ref.substr(8));
        auto it = toastTable_.find(oid);
        if (it == toastTable_.end()) return ref;
        const auto& entry = it->second;
        return entry.compressed ? simpleDecompress(entry.data) : entry.data;
    }

    bool isToastRef(const std::string& s) const {
        return s.rfind("__toast:", 0) == 0;
    }

    size_t toastedCount() const { return toastedCount_; }
    size_t totalSavedBytes() const { return totalSavedBytes_ > 0 ? static_cast<size_t>(totalSavedBytes_) : 0; }

    struct ToastStats {
        size_t toastedValues = 0;
        size_t savedBytes = 0;
        double compressionRatio = 0.0;
    };

    ToastStats stats() const {
        ToastStats s;
        s.toastedValues = toastedCount_;
        size_t original = 0, stored = 0;
        for (auto& [oid, e] : toastTable_) { original += e.originalSize; stored += e.data.size(); }
        s.savedBytes = (original > stored) ? original - stored : 0;
        s.compressionRatio = (original > 0) ? (1.0 - (double)stored/original) * 100.0 : 0.0;
        return s;
    }

private:
    std::map<uint64_t, ToastEntry> toastTable_;
    uint64_t nextOid_ = 1;
    size_t toastedCount_ = 0;
    long long totalSavedBytes_ = 0;
};

} // namespace milansql
