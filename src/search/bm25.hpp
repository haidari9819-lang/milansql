#pragma once
#include <string>
#include <vector>
#include <sstream>
#include <cmath>
#include <algorithm>
#include <unordered_map>

class Bm25Scorer {
public:
    static constexpr double k1 = 1.5;
    static constexpr double b  = 0.75;

    // Compute BM25 score for a document
    // termFreqInDoc: how many times the query term appears in this doc
    // docLength: total term count of this doc
    // avgDocLength: average doc length across corpus
    // docsWithTerm: how many docs contain this term
    // totalDocs: total number of documents
    static double score(int termFreqInDoc, int docLength, double avgDocLength,
                        int docsWithTerm, int totalDocs) {
        if (docsWithTerm == 0 || totalDocs == 0) return 0.0;
        double idf = std::log((totalDocs - docsWithTerm + 0.5) / (docsWithTerm + 0.5) + 1.0);
        double tf = (termFreqInDoc * (k1 + 1.0)) /
                    (termFreqInDoc + k1 * (1.0 - b + b * docLength / avgDocLength));
        return idf * tf;
    }
};

// Extract a snippet around first occurrence of query terms
inline std::string extractSnippet(const std::string& text,
                                   const std::string& query,
                                   int maxLen = 150) {
    // lowercase text and query for search
    std::string lText = text, lQuery = query;
    std::transform(lText.begin(), lText.end(), lText.begin(), ::tolower);
    std::transform(lQuery.begin(), lQuery.end(), lQuery.begin(), ::tolower);

    // tokenize query
    std::istringstream iss(lQuery);
    std::string tok;
    size_t bestPos = std::string::npos;
    while (iss >> tok) {
        // strip boolean operators
        if (!tok.empty() && (tok[0] == '+' || tok[0] == '-')) tok = tok.substr(1);
        if (tok.empty()) continue;
        size_t pos = lText.find(tok);
        if (pos != std::string::npos) {
            if (bestPos == std::string::npos || pos < bestPos) bestPos = pos;
        }
    }
    if (bestPos == std::string::npos) bestPos = 0;

    size_t start = (bestPos > 30) ? bestPos - 30 : 0;
    size_t len   = std::min((size_t)maxLen, text.size() - start);
    std::string snippet = text.substr(start, len);
    if (start > 0) snippet = "..." + snippet;
    if (start + len < text.size()) snippet += "...";
    return snippet;
}
