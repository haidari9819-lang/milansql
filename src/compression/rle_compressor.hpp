#pragma once
#include "compressor.hpp"
#include <stdexcept>

// RLE Compressor
// Format: [0x01][count][byte] for runs of 3+ repeated bytes
//         [0x00][length][bytes...] for literal runs (< 3 repeated)
// count and length are 1-byte values (1-255)

namespace milansql {

class RleCompressor : public Compressor {
public:
    std::vector<uint8_t> compress(const std::string& input) const override {
        const auto* src = reinterpret_cast<const uint8_t*>(input.data());
        size_t srcLen = input.size();
        std::vector<uint8_t> out;
        out.reserve(srcLen);

        size_t i = 0;
        while (i < srcLen) {
            // Count run length
            size_t runLen = 1;
            while (i + runLen < srcLen && runLen < 255 && src[i + runLen] == src[i])
                ++runLen;

            if (runLen >= 3) {
                // Emit run: marker=0x01, count, byte
                out.push_back(0x01);
                out.push_back(static_cast<uint8_t>(runLen));
                out.push_back(src[i]);
                i += runLen;
            } else {
                // Collect literal run (until next run of 3+)
                size_t litStart = i;
                size_t litLen = 0;
                while (i < srcLen && litLen < 255) {
                    // Peek ahead for run
                    size_t peek = 1;
                    while (i + litLen + peek < srcLen &&
                           peek < 3 &&
                           src[i + litLen + peek] == src[i + litLen])
                        ++peek;
                    if (peek >= 3) break;
                    ++litLen;
                    ++i;
                }
                if (litLen == 0) {
                    // Fallback: emit single literal
                    out.push_back(0x00);
                    out.push_back(1);
                    out.push_back(src[i]);
                    ++i;
                } else {
                    out.push_back(0x00);
                    out.push_back(static_cast<uint8_t>(litLen));
                    for (size_t k = 0; k < litLen; ++k)
                        out.push_back(src[litStart + k]);
                }
            }
        }
        return out;
    }

    std::string decompress(const std::vector<uint8_t>& data) const override {
        std::string out;
        out.reserve(data.size() * 2);
        size_t i = 0;
        while (i < data.size()) {
            if (i + 1 >= data.size())
                throw std::runtime_error("RLE decompress: truncated data");
            uint8_t marker = data[i++];
            uint8_t count  = data[i++];
            if (marker == 0x01) {
                // Run
                if (i >= data.size())
                    throw std::runtime_error("RLE decompress: missing run byte");
                uint8_t byte = data[i++];
                for (uint8_t k = 0; k < count; ++k)
                    out.push_back(static_cast<char>(byte));
            } else {
                // Literals
                if (i + count > data.size())
                    throw std::runtime_error("RLE decompress: truncated literals");
                for (uint8_t k = 0; k < count; ++k)
                    out.push_back(static_cast<char>(data[i++]));
            }
        }
        return out;
    }
};

} // namespace milansql
