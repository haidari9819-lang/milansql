#!/usr/bin/env python3
"""Fix RLS v2 compilation: replace Parser dependency with inline tokenizer+parser."""

ENGINE = "/opt/milansql/src/engine/engine.hpp"

with open(ENGINE, 'r') as f:
    content = f.read()

# Replace the parseRlsExpr_ and evaluateRlsExpr_ with self-contained versions
OLD = '''    // Phase 170: Parse RLS expression string into WhereConditions using the existing parser
    std::pair<std::vector<WhereCondition>, std::string>
    parseRlsExpr_(const std::string& rawExpr) const {
        std::string expr = rlsSubstituteVars_(rawExpr);
        // Tokenize the expression as "WHERE <expr>" so parseWhere can handle it
        std::string fakeSql = "SELECT * FROM _rls WHERE " + expr;
        auto tokens = milansql::Parser::tokenize(fakeSql);
        // Find WHERE token position
        size_t wherePos = 0;
        for (size_t i = 0; i < tokens.size(); ++i) {
            std::string u = tokens[i];
            for (auto& c : u) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            if (u == "WHERE") { wherePos = i; break; }
        }
        milansql::ParsedCommand tmpCmd;
        milansql::Parser::parseWhere(tokens, wherePos, tmpCmd);
        return {tmpCmd.whereConds, tmpCmd.whereLogic};
    }

    // Phase 170: Evaluate RLS expression using the full WHERE engine
    bool evaluateRlsExpr_(const std::string& expr, const Row& row,
                          const std::vector<std::string>& colNames) const {
        if (expr.empty()) return true;
        if (expr == "1 = 1" || expr == "1=1") return true;
        if (expr == "1 = 0" || expr == "1=0") return false;

        try {
            auto [conds, logic] = parseRlsExpr_(expr);
            if (conds.empty()) return true;

            // Build a temporary Table shell for rowMatches
            Table tmpTbl("_rls_eval", {});
            // We need columns — build from colNames
            std::vector<Column> cols;
            for (const auto& cn : colNames) {
                Column c;
                c.name = cn;
                c.type = "TEXT";
                cols.push_back(c);
            }
            // Use a stack-local table with the right schema
            Table evalTbl("_rls_eval", cols);

            return const_cast<Engine*>(this)->rowMatches(evalTbl, row, conds, logic);
        } catch (...) {
            // Fallback: deny if expression can\'t be parsed
            return false;
        }
    }'''

NEW = '''    // Phase 170: Inline tokenizer for RLS expressions (no Parser dependency)
    static std::vector<std::string> rlsTokenize_(const std::string& s) {
        std::vector<std::string> tokens;
        std::string cur;
        bool inStr = false;
        char strChar = 0;
        for (size_t i = 0; i < s.size(); ++i) {
            char c = s[i];
            if (inStr) {
                cur += c;
                if (c == strChar) { inStr = false; tokens.push_back(cur); cur.clear(); }
                continue;
            }
            if (c == '\\'' || c == '"') {
                if (!cur.empty()) { tokens.push_back(cur); cur.clear(); }
                inStr = true; strChar = c; cur += c;
                continue;
            }
            if (c == '(' || c == ')' || c == ',') {
                if (!cur.empty()) { tokens.push_back(cur); cur.clear(); }
                tokens.push_back(std::string(1, c));
                continue;
            }
            // Handle multi-char operators
            if (c == '!' && i + 1 < s.size() && s[i+1] == '=') {
                if (!cur.empty()) { tokens.push_back(cur); cur.clear(); }
                tokens.push_back("!="); ++i; continue;
            }
            if (c == '>' && i + 1 < s.size() && s[i+1] == '=') {
                if (!cur.empty()) { tokens.push_back(cur); cur.clear(); }
                tokens.push_back(">="); ++i; continue;
            }
            if (c == '<' && i + 1 < s.size() && s[i+1] == '=') {
                if (!cur.empty()) { tokens.push_back(cur); cur.clear(); }
                tokens.push_back("<="); ++i; continue;
            }
            if (c == '<' && i + 1 < s.size() && s[i+1] == '>') {
                if (!cur.empty()) { tokens.push_back(cur); cur.clear(); }
                tokens.push_back("!="); ++i; continue;
            }
            if (c == '=' || c == '<' || c == '>') {
                if (!cur.empty()) { tokens.push_back(cur); cur.clear(); }
                tokens.push_back(std::string(1, c));
                continue;
            }
            if (c == ' ' || c == '\\t' || c == '\\n') {
                if (!cur.empty()) { tokens.push_back(cur); cur.clear(); }
                continue;
            }
            cur += c;
        }
        if (!cur.empty()) tokens.push_back(cur);
        return tokens;
    }

    // Phase 170: Parse tokenized RLS expression into WhereConditions
    static std::pair<std::vector<WhereCondition>, std::string>
    parseRlsTokens_(const std::vector<std::string>& tokens) {
        std::vector<WhereCondition> conds;
        std::string logic = "AND";
        size_t i = 0;

        auto toUp = [](const std::string& s) {
            std::string r = s;
            for (auto& c : r) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            return r;
        };

        while (i < tokens.size()) {
            std::string u = toUp(tokens[i]);
            if (u == "AND") { logic = "AND"; ++i; continue; }
            if (u == "OR")  { logic = "OR";  ++i; continue; }

            WhereCondition cond;
            bool parsed = false;

            // IS NULL / IS NOT NULL
            if (i + 2 < tokens.size() && toUp(tokens[i+1]) == "IS" && toUp(tokens[i+2]) == "NULL") {
                cond.col = tokens[i]; cond.op = "IS NULL"; i += 3; parsed = true;
            } else if (i + 3 < tokens.size() && toUp(tokens[i+1]) == "IS" &&
                       toUp(tokens[i+2]) == "NOT" && toUp(tokens[i+3]) == "NULL") {
                cond.col = tokens[i]; cond.op = "IS NOT NULL"; i += 4; parsed = true;

            // NOT IN (...)
            } else if (i + 2 < tokens.size() && toUp(tokens[i+1]) == "NOT" && toUp(tokens[i+2]) == "IN") {
                cond.col = tokens[i]; cond.op = "NOT IN"; i += 3;
                if (i < tokens.size() && tokens[i] == "(") {
                    ++i;
                    while (i < tokens.size() && tokens[i] != ")") {
                        if (tokens[i] != ",") {
                            std::string v = tokens[i];
                            if (v.size() >= 2 && v.front() == '\\'' && v.back() == '\\'')
                                v = v.substr(1, v.size() - 2);
                            cond.inList.push_back(v);
                        }
                        ++i;
                    }
                    if (i < tokens.size()) ++i;  // skip )
                }
                parsed = true;

            // IN (...)
            } else if (i + 1 < tokens.size() && toUp(tokens[i+1]) == "IN") {
                cond.col = tokens[i]; cond.op = "IN"; i += 2;
                if (i < tokens.size() && tokens[i] == "(") {
                    ++i;
                    while (i < tokens.size() && tokens[i] != ")") {
                        if (tokens[i] != ",") {
                            std::string v = tokens[i];
                            if (v.size() >= 2 && v.front() == '\\'' && v.back() == '\\'')
                                v = v.substr(1, v.size() - 2);
                            cond.inList.push_back(v);
                        }
                        ++i;
                    }
                    if (i < tokens.size()) ++i;  // skip )
                }
                parsed = true;

            // BETWEEN
            } else if (i + 4 < tokens.size() && toUp(tokens[i+1]) == "BETWEEN") {
                cond.col = tokens[i]; cond.op = "BETWEEN";
                cond.betweenLow = tokens[i+2];
                // skip AND
                cond.betweenHigh = tokens[i+4];
                i += 5; parsed = true;

            // NOT BETWEEN
            } else if (i + 5 < tokens.size() && toUp(tokens[i+1]) == "NOT" && toUp(tokens[i+2]) == "BETWEEN") {
                cond.col = tokens[i]; cond.op = "NOT BETWEEN";
                cond.betweenLow = tokens[i+3];
                // skip AND
                cond.betweenHigh = tokens[i+5];
                i += 6; parsed = true;

            // LIKE
            } else if (i + 2 < tokens.size() && toUp(tokens[i+1]) == "LIKE") {
                cond.col = tokens[i]; cond.op = "LIKE"; cond.val = tokens[i+2];
                i += 3; parsed = true;

            // Standard operator: col op val
            } else if (i + 2 < tokens.size()) {
                std::string opStr = tokens[i+1];
                if (opStr == "=" || opStr == "!=" || opStr == "<" || opStr == ">" ||
                    opStr == "<=" || opStr == ">=") {
                    cond.col = tokens[i]; cond.op = opStr; cond.val = tokens[i+2];
                    // Strip quotes from value
                    if (cond.val.size() >= 2 && cond.val.front() == '\\'' && cond.val.back() == '\\'')
                        cond.val = cond.val.substr(1, cond.val.size() - 2);
                    i += 3; parsed = true;
                }
            }

            if (parsed) {
                conds.push_back(cond);
            } else {
                ++i;  // skip unparseable token
            }
        }
        return {conds, logic};
    }

    // Phase 170: Parse RLS expression string into WhereConditions
    std::pair<std::vector<WhereCondition>, std::string>
    parseRlsExpr_(const std::string& rawExpr) const {
        std::string expr = rlsSubstituteVars_(rawExpr);
        auto tokens = rlsTokenize_(expr);
        return parseRlsTokens_(tokens);
    }

    // Phase 170: Evaluate RLS expression using the full WHERE engine
    bool evaluateRlsExpr_(const std::string& expr, const Row& row,
                          const std::vector<std::string>& colNames) const {
        if (expr.empty()) return true;
        if (expr == "1 = 1" || expr == "1=1") return true;
        if (expr == "1 = 0" || expr == "1=0") return false;

        try {
            auto [conds, logic] = parseRlsExpr_(expr);
            if (conds.empty()) return true;

            // Build a temporary Table for rowMatches
            std::vector<Column> cols;
            for (const auto& cn : colNames)
                cols.push_back(Column(cn, "TEXT"));
            Table evalTbl("_rls_eval", cols);

            return const_cast<Engine*>(this)->rowMatches(evalTbl, row, conds, logic);
        } catch (...) {
            // Fallback: deny if expression can\\'t be parsed
            return false;
        }
    }'''

if OLD in content:
    content = content.replace(OLD, NEW, 1)
    print("OK: replaced parseRlsExpr_/evaluateRlsExpr_ with self-contained version")
else:
    print("FAIL: pattern not found")
    # Debug: find the function
    if 'parseRlsExpr_' in content:
        print("  parseRlsExpr_ exists but pattern doesn't match exactly")
    else:
        print("  parseRlsExpr_ not found at all")

with open(ENGINE, 'w') as f:
    f.write(content)
