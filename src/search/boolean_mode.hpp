#pragma once
#include <string>
#include <vector>
#include <sstream>
#include <algorithm>

struct BooleanTerm {
    std::string word;
    bool mustHave  = false;  // +word
    bool mustNot   = false;  // -word
    bool isPhrase  = false;  // "phrase"
    std::string phrase;
};

class BooleanModeParser {
public:
    static std::vector<BooleanTerm> parse(const std::string& query) {
        std::vector<BooleanTerm> terms;
        std::string q = query;
        size_t i = 0;
        while (i < q.size()) {
            // skip whitespace
            while (i < q.size() && std::isspace((unsigned char)q[i])) i++;
            if (i >= q.size()) break;

            BooleanTerm term;
            if (q[i] == '+') { term.mustHave = true; i++; }
            else if (q[i] == '-') { term.mustNot = true; i++; }

            if (i < q.size() && q[i] == '"') {
                // phrase
                term.isPhrase = true;
                i++; // skip opening "
                std::string phrase;
                while (i < q.size() && q[i] != '"') phrase += q[i++];
                if (i < q.size()) i++; // skip closing "
                term.phrase = phrase;
                std::transform(term.phrase.begin(), term.phrase.end(), term.phrase.begin(), ::tolower);
                terms.push_back(term);
            } else {
                // single word
                std::string word;
                while (i < q.size() && !std::isspace((unsigned char)q[i])) word += q[i++];
                if (!word.empty()) {
                    std::transform(word.begin(), word.end(), word.begin(), ::tolower);
                    term.word = word;
                    terms.push_back(term);
                }
            }
        }
        return terms;
    }

    static bool match(const std::string& docText, const std::vector<BooleanTerm>& terms) {
        std::string lDoc = docText;
        std::transform(lDoc.begin(), lDoc.end(), lDoc.begin(), ::tolower);

        // Check mustNot and mustHave constraints first
        for (auto& t : terms) {
            bool found = false;
            if (t.isPhrase) {
                found = (lDoc.find(t.phrase) != std::string::npos);
            } else {
                found = (lDoc.find(t.word) != std::string::npos);
            }
            if (t.mustNot && found) return false;
            if (t.mustHave && !found) return false;
        }

        // Check if at least one non-mustNot term matches
        for (auto& t : terms) {
            if (t.mustNot) continue;
            bool found = false;
            if (t.isPhrase) found = (lDoc.find(t.phrase) != std::string::npos);
            else found = (lDoc.find(t.word) != std::string::npos);
            if (found) return true;
        }
        return false;
    }
};
