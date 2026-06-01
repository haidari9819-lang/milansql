#pragma once
#include "compressor.hpp"
#include <map>
#include <algorithm>
#include <stdexcept>

// Dictionary Compressor
// Encoding:
//   [0x01][code_byte]            — dictionary hit (code 0x00–0xFF)
//   [0x00][len_lo][len_hi][bytes] — literal string (2-byte length)

namespace milansql {

class DictCompressor : public Compressor {
public:
    // Build dictionary from a list of string values (256 most frequent)
    void buildDictionary(const std::vector<std::string>& values) {
        std::map<std::string, size_t> freq;
        for (const auto& v : values)
            if (!v.empty() && v != "NULL")
                freq[v]++;

        // Sort by frequency descending, take top 256
        std::vector<std::pair<size_t, std::string>> ranked;
        ranked.reserve(freq.size());
        for (const auto& kv : freq)
            ranked.push_back({kv.second, kv.first});
        std::sort(ranked.begin(), ranked.end(),
                  [](const std::pair<size_t,std::string>& a,
                     const std::pair<size_t,std::string>& b) {
                      return a.first > b.first;
                  });

        dict_.clear();
        reverseDict_.clear();
        size_t limit = std::min(ranked.size(), size_t(256));
        for (size_t i = 0; i < limit; ++i) {
            uint8_t code = static_cast<uint8_t>(i);
            dict_[ranked[i].second] = code;
            reverseDict_[code] = ranked[i].second;
        }
    }

    std::vector<uint8_t> compress(const std::string& input) const override {
        // Try dictionary lookup for the whole string
        auto it = dict_.find(input);
        if (it != dict_.end()) {
            return {0x01, it->second};
        }
        // Literal encoding
        std::vector<uint8_t> out;
        size_t len = input.size();
        out.push_back(0x00);
        out.push_back(static_cast<uint8_t>(len & 0xFF));
        out.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
        for (unsigned char c : input)
            out.push_back(c);
        return out;
    }

    std::string decompress(const std::vector<uint8_t>& data) const override {
        if (data.empty())
            throw std::runtime_error("Dict decompress: empty data");
        if (data[0] == 0x01) {
            // Dictionary hit
            if (data.size() < 2)
                throw std::runtime_error("Dict decompress: truncated dict token");
            uint8_t code = data[1];
            auto it = reverseDict_.find(code);
            if (it == reverseDict_.end())
                throw std::runtime_error("Dict decompress: unknown code");
            return it->second;
        } else {
            // Literal
            if (data.size() < 3)
                throw std::runtime_error("Dict decompress: truncated literal");
            size_t len = static_cast<size_t>(data[1]) | (static_cast<size_t>(data[2]) << 8);
            if (data.size() < 3 + len)
                throw std::runtime_error("Dict decompress: truncated literal data");
            return std::string(reinterpret_cast<const char*>(data.data() + 3), len);
        }
    }

    bool hasDictionary() const { return !dict_.empty(); }

    // Compress a blob of all values concatenated (for stats)
    std::vector<uint8_t> compressAll(const std::vector<std::string>& values) const {
        std::vector<uint8_t> out;
        for (const auto& v : values) {
            auto tok = compress(v);
            out.insert(out.end(), tok.begin(), tok.end());
        }
        return out;
    }

private:
    std::map<std::string, uint8_t> dict_;          // string → code
    std::map<uint8_t, std::string> reverseDict_;   // code → string
};

} // namespace milansql
