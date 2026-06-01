#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <cctype>

namespace milansql {

enum class CompressionType { NONE, LZ4, RLE, DICTIONARY, ZSTD };

inline CompressionType parseCompressionType(const std::string& s) {
    std::string lower = s;
    for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lower == "lz4")  return CompressionType::LZ4;
    if (lower == "rle")  return CompressionType::RLE;
    if (lower == "dictionary" || lower == "dict") return CompressionType::DICTIONARY;
    if (lower == "zstd") return CompressionType::ZSTD;
    return CompressionType::NONE;
}

inline std::string compressionTypeName(CompressionType t) {
    switch (t) {
        case CompressionType::LZ4:        return "lz4";
        case CompressionType::RLE:        return "rle";
        case CompressionType::DICTIONARY: return "dictionary";
        case CompressionType::ZSTD:       return "zstd";
        default:                          return "none";
    }
}

class Compressor {
public:
    virtual ~Compressor() = default;
    virtual std::vector<uint8_t> compress(const std::string& data) const = 0;
    virtual std::string decompress(const std::vector<uint8_t>& data) const = 0;
};

} // namespace milansql
