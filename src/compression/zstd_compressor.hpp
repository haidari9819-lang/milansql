#pragma once
#include "compressor.hpp"
#include "rle_compressor.hpp"
#include "lz4_compressor.hpp"
#include <stdexcept>

// Simplified ZSTD-like compressor: two-pass RLE then LZ4
// Header byte: 0xAB marks ZSTD-encoded data

namespace milansql {

class ZstdCompressor : public Compressor {
public:
    std::vector<uint8_t> compress(const std::string& input) const override {
        // Pass 1: RLE
        RleCompressor rle;
        auto rleOut = rle.compress(input);

        // Pass 2: LZ4 on RLE output
        Lz4Compressor lz4;
        std::string rleStr(reinterpret_cast<const char*>(rleOut.data()), rleOut.size());
        auto lz4Out = lz4.compress(rleStr);

        // Prepend header: 0xAB + original_size (4 bytes LE) + rle_size (4 bytes LE)
        std::vector<uint8_t> out;
        out.reserve(9 + lz4Out.size());
        out.push_back(0xAB);
        uint32_t origSize = static_cast<uint32_t>(input.size());
        uint32_t rleSize  = static_cast<uint32_t>(rleOut.size());
        out.push_back(static_cast<uint8_t>(origSize & 0xFF));
        out.push_back(static_cast<uint8_t>((origSize >> 8)  & 0xFF));
        out.push_back(static_cast<uint8_t>((origSize >> 16) & 0xFF));
        out.push_back(static_cast<uint8_t>((origSize >> 24) & 0xFF));
        out.push_back(static_cast<uint8_t>(rleSize & 0xFF));
        out.push_back(static_cast<uint8_t>((rleSize >> 8)  & 0xFF));
        out.push_back(static_cast<uint8_t>((rleSize >> 16) & 0xFF));
        out.push_back(static_cast<uint8_t>((rleSize >> 24) & 0xFF));
        out.insert(out.end(), lz4Out.begin(), lz4Out.end());
        return out;
    }

    std::string decompress(const std::vector<uint8_t>& data) const override {
        if (data.size() < 9 || data[0] != 0xAB)
            throw std::runtime_error("ZSTD decompress: invalid header");
        // Read rle_size
        uint32_t rleSize =
            static_cast<uint32_t>(data[5]) |
            (static_cast<uint32_t>(data[6]) << 8)  |
            (static_cast<uint32_t>(data[7]) << 16) |
            (static_cast<uint32_t>(data[8]) << 24);

        // Decompress LZ4
        std::vector<uint8_t> lz4Data(data.begin() + 9, data.end());
        Lz4Compressor lz4;
        std::string rleStr = lz4.decompress(lz4Data);

        // Verify rle size
        if (rleStr.size() != static_cast<size_t>(rleSize))
            throw std::runtime_error("ZSTD decompress: RLE size mismatch");

        // Decompress RLE
        std::vector<uint8_t> rleVec(reinterpret_cast<const uint8_t*>(rleStr.data()),
                                     reinterpret_cast<const uint8_t*>(rleStr.data()) + rleStr.size());
        RleCompressor rle;
        return rle.decompress(rleVec);
    }
};

} // namespace milansql
