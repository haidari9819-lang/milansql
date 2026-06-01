#pragma once
#include "compressor.hpp"
#include <stdexcept>

// Simplified LZ77-style compressor (LZ4-like)
// Token format:
//   [0xFF][offset_lo][offset_hi][len]  — back-reference match (min 4 bytes)
//   [0x00][byte]                       — literal byte

namespace milansql {

class Lz4Compressor : public Compressor {
public:
    std::vector<uint8_t> compress(const std::string& input) const override {
        const auto* src = reinterpret_cast<const uint8_t*>(input.data());
        size_t srcLen = input.size();
        std::vector<uint8_t> out;
        out.reserve(srcLen);

        size_t pos = 0;
        while (pos < srcLen) {
            // Find longest match in last 4096 bytes
            size_t bestOffset = 0, bestLen = 0;
            size_t windowStart = (pos > 4096) ? pos - 4096 : 0;

            for (size_t back = windowStart; back < pos; ++back) {
                size_t matchLen = 0;
                while (pos + matchLen < srcLen &&
                       matchLen < 255 &&
                       src[back + matchLen] == src[pos + matchLen]) {
                    ++matchLen;
                }
                if (matchLen >= 4 && matchLen > bestLen) {
                    bestLen   = matchLen;
                    bestOffset = pos - back;
                }
            }

            if (bestLen >= 4) {
                // Emit match: [0xFF][offset_lo][offset_hi][len]
                out.push_back(0xFF);
                out.push_back(static_cast<uint8_t>(bestOffset & 0xFF));
                out.push_back(static_cast<uint8_t>((bestOffset >> 8) & 0xFF));
                out.push_back(static_cast<uint8_t>(bestLen));
                pos += bestLen;
            } else {
                // Emit literal: [0x00][byte]
                out.push_back(0x00);
                out.push_back(src[pos]);
                ++pos;
            }
        }
        return out;
    }

    std::string decompress(const std::vector<uint8_t>& data) const override {
        std::string out;
        out.reserve(data.size() * 2);
        size_t i = 0;
        while (i < data.size()) {
            uint8_t tag = data[i++];
            if (tag == 0xFF) {
                // Match token: [offset_lo][offset_hi][len]
                if (i + 2 >= data.size())
                    throw std::runtime_error("LZ4 decompress: truncated match token");
                uint8_t offLo  = data[i++];
                uint8_t offHi  = data[i++];
                uint8_t mlen   = data[i++];
                size_t  offset = static_cast<size_t>(offLo) | (static_cast<size_t>(offHi) << 8);
                if (offset == 0 || offset > out.size())
                    throw std::runtime_error("LZ4 decompress: invalid offset");
                size_t copyFrom = out.size() - offset;
                for (uint8_t k = 0; k < mlen; ++k)
                    out.push_back(out[copyFrom + k]);
            } else {
                // Literal token: [0x00][byte]
                if (i >= data.size())
                    throw std::runtime_error("LZ4 decompress: truncated literal token");
                out.push_back(static_cast<char>(data[i++]));
            }
        }
        return out;
    }
};

} // namespace milansql
