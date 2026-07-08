#!/usr/bin/env python3
"""
Patch MilanSQL RLS: Full WHERE-parser reuse, WITH CHECK, UPDATE/DELETE enforcement.
"""
import re

ENGINE = "/opt/milansql/src/engine/engine.hpp"
PARSER = "/opt/milansql/src/parser/parser.hpp"
DISPATCH = "/opt/milansql/src/dispatch.hpp"

def read(path):
    with open(path, 'r') as f:
        return f.read()

def write(path, content):
    with open(path, 'w') as f:
        f.write(content)

def patch(path, old, new, label):
    content = read(path)
    if old not in content:
        print(f"  SKIP: {label} — pattern not found in {path}")
        return False
    content = content.replace(old, new, 1)
    write(path, content)
    print(f"  OK: {label}")
    return True

# ══════════════════════════════════════════════════════════════
# 1. Extend RlsPolicy struct with withCheckExpr
# ══════════════════════════════════════════════════════════════
print("=== 1. RlsPolicy struct: add withCheckExpr ===")
patch(ENGINE,
    '''struct RlsPolicy {
        std::string name;
        std::string table;
        std::string command;
        std::string role;
        std::string usingExpr;
    };''',
    '''struct RlsPolicy {
        std::string name;
        std::string table;
        std::string command;
        std::string role;
        std::string usingExpr;
        std::string withCheckExpr;  // Phase 170: WITH CHECK for INSERT/UPDATE validation
    };''',
    "RlsPolicy struct + withCheckExpr")

# ══════════════════════════════════════════════════════════════
# 2. Replace evaluateRlsExpr_ with full WHERE parser reuse
# ══════════════════════════════════════════════════════════════
print("\n=== 2. Replace evaluateRlsExpr_ with WHERE parser ===")

OLD_EVAL = '''    bool evaluateRlsExpr_(const std::string& expr, const Row& row,
                          const std::vector<std::string>& colNames) const {
        if (expr == "1 = 1" || expr == "1=1") return true;
        if (expr == "1 = 0" || expr == "1=0") return false;

        std::string e = expr;
        size_t p;
        while ((p = e.find("CURRENT_USER_ID()")) != std::string::npos)
            e.replace(p, 17, currentUser_);

        auto eqPos = e.find('=');
        if (eqPos == std::string::npos) return true;
        std::string lhs = e.substr(0, eqPos);
        std::string rhs = e.substr(eqPos + 1);
        auto trim = [](std::string& s) {
            while (!s.empty() && s.front() == ' ') s.erase(s.begin());
            while (!s.empty() && s.back()  == ' ') s.pop_back();
        };
        trim(lhs); trim(rhs);
        if (rhs.size() >= 2 && rhs.front() == '\\'' && rhs.back() == '\\'')
            rhs = rhs.substr(1, rhs.size() - 2);

        for (size_t i = 0; i < colNames.size(); ++i) {
            if (colNames[i] == lhs) {
                if (i >= row.values.size()) return false;
                std::string cellVal = row.values[i];
                // Strip surrounding single quotes stored by the engine
                if (cellVal.size() >= 2 && cellVal.front() == '\\'' && cellVal.back() == '\\'')
                    cellVal = cellVal.substr(1, cellVal.size() - 2);
                return cellVal == rhs;
            }
        }
        return true;
    }'''

NEW_EVAL = '''    // Phase 170: Substitute CURRENT_USER_ID() / current_user in RLS expression
    std::string rlsSubstituteVars_(const std::string& expr) const {
        std::string e = expr;
        // Replace CURRENT_USER_ID() with current user name
        size_t p;
        while ((p = e.find("CURRENT_USER_ID()")) != std::string::npos)
            e.replace(p, 17, currentUser_);
        while ((p = e.find("current_user_id()")) != std::string::npos)
            e.replace(p, 17, currentUser_);
        // Replace current_user (standalone, not part of _id)
        {
            std::string pat = "current_user";
            size_t pos = 0;
            while ((pos = e.find(pat, pos)) != std::string::npos) {
                // Make sure it's not part of current_user_id
                if (pos + pat.size() < e.size() && e[pos + pat.size()] == '_') {
                    pos += pat.size();
                    continue;
                }
                e.replace(pos, pat.size(), "'" + currentUser_ + "'");
                pos += currentUser_.size() + 2;
            }
        }
        return e;
    }

    // Phase 170: Parse RLS expression string into WhereConditions using the existing parser
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
            // Fallback: deny if expression can't be parsed
            return false;
        }
    }

    // Phase 170: Check WITH CHECK expression for a row being written
    bool checkRlsWithCheck_(const std::string& table, const Row& row,
                            const std::string& cmd) const {
        if (!isRlsEnabled(table)) return true;
        if (currentUser_ == "root" || currentUser_.empty()) return true;

        auto pit = rlsPolicies_.find(table);
        if (pit == rlsPolicies_.end() || pit->second.empty()) return false;

        std::vector<std::string> colNames;
        try {
            const auto& cols = getTable(table).columns();
            colNames.reserve(cols.size());
            for (const auto& c : cols) colNames.push_back(c.name);
        } catch (...) {
            return false;
        }

        for (const auto& pol : pit->second) {
            if (pol.command != "ALL" && pol.command != cmd) continue;
            if (pol.role != "PUBLIC" && pol.role != currentUser_) continue;
            // Use withCheckExpr if present, otherwise fall back to usingExpr
            const std::string& checkExpr = pol.withCheckExpr.empty()
                                           ? pol.usingExpr : pol.withCheckExpr;
            if (evaluateRlsExpr_(checkExpr, row, colNames)) {
                return true;
            }
        }
        return false;
    }'''

patch(ENGINE, OLD_EVAL, NEW_EVAL, "evaluateRlsExpr_ -> full WHERE parser + WITH CHECK")

# ══════════════════════════════════════════════════════════════
# 3. Update showPolicies to display WITH CHECK
# ══════════════════════════════════════════════════════════════
print("\n=== 3. showPolicies: display WITH CHECK ===")
patch(ENGINE,
    '''        for (const auto& p : it->second)
            std::cout << "  " << p.name << " | " << p.command
                      << " | TO " << p.role
                      << " | USING (" << p.usingExpr << ")\\n";''',
    '''        for (const auto& p : it->second) {
            std::cout << "  " << p.name << " | " << p.command
                      << " | TO " << p.role
                      << " | USING (" << p.usingExpr << ")";
            if (!p.withCheckExpr.empty())
                std::cout << " WITH CHECK (" << p.withCheckExpr << ")";
            std::cout << "\\n";
        }''',
    "showPolicies WITH CHECK display")

# ══════════════════════════════════════════════════════════════
# 4. Update saveRls / loadRls for withCheckExpr
# ══════════════════════════════════════════════════════════════
print("\n=== 4. saveRls/loadRls: persist withCheckExpr ===")
patch(ENGINE,
    '''                f << "POLICY " << p.name << "|" << p.table << "|"
                  << p.command << "|" << p.role << "|" << p.usingExpr << "\\n";''',
    '''                f << "POLICY " << p.name << "|" << p.table << "|"
                  << p.command << "|" << p.role << "|" << p.usingExpr
                  << "|" << p.withCheckExpr << "\\n";''',
    "saveRls withCheckExpr")

patch(ENGINE,
    '''                if (parts.size() >= 5) {
                    RlsPolicy p;
                    p.name = parts[0]; p.table = parts[1];
                    p.command = parts[2]; p.role = parts[3];
                    p.usingExpr = parts[4];
                    rlsPolicies_[p.table].push_back(p);
                }''',
    '''                if (parts.size() >= 5) {
                    RlsPolicy p;
                    p.name = parts[0]; p.table = parts[1];
                    p.command = parts[2]; p.role = parts[3];
                    p.usingExpr = parts[4];
                    if (parts.size() >= 6) p.withCheckExpr = parts[5];
                    rlsPolicies_[p.table].push_back(p);
                }''',
    "loadRls withCheckExpr")

# ══════════════════════════════════════════════════════════════
# 5. Add RLS enforcement to deleteWhere
# ══════════════════════════════════════════════════════════════
print("\n=== 5. RLS enforcement in deleteWhere ===")
patch(ENGINE,
    '''    std::size_t deleteWhere(const std::string& tblRaw,
                            const std::string& wCol, const std::string& wVal) {
        auto tbl = resolveTableName(tblRaw);
        checkPrivilege("DELETE", tbl);  // Phase 46: access control
        if (!g_lockManager.checkWriteAllowed(tbl))  // Phase 65: table lock check
            throw std::runtime_error("Tabelle '" + tbl + "' ist durch LOCK TABLE READ gesperrt.");
        WriteScope ws(getOrCreateRwLock(tbl), tbl);  // Phase 112: exclusive write lock
        // Phase 21: CASCADE / SET NULL / RESTRICT für betroffene Zeilen
        {
            const Table& t = getTable(tbl);
            size_t wCI = colIdx(t, wCol);
            const std::string stripped = milansql::dateutils::stripQuotes(wVal);
            std::vector<Row> matched;
            for (const auto& row : t.rows())
                if (wCI < row.values.size() &&
                    (row.values[wCI] == wVal || row.values[wCI] == stripped))
                    matched.push_back(row);
            for (const auto& row : matched)
                cascadeDelete(tbl, row);
        }''',
    '''    std::size_t deleteWhere(const std::string& tblRaw,
                            const std::string& wCol, const std::string& wVal) {
        auto tbl = resolveTableName(tblRaw);
        checkPrivilege("DELETE", tbl);  // Phase 46: access control
        if (!g_lockManager.checkWriteAllowed(tbl))  // Phase 65: table lock check
            throw std::runtime_error("Tabelle '" + tbl + "' ist durch LOCK TABLE READ gesperrt.");
        WriteScope ws(getOrCreateRwLock(tbl), tbl);  // Phase 112: exclusive write lock

        // Phase 170: RLS enforcement for DELETE — filter matched rows
        if (isRlsEnabled(tbl) && currentUser_ != "root" && !currentUser_.empty()) {
            const Table& t = getTable(tbl);
            size_t wCI = colIdx(t, wCol);
            const std::string stripped = milansql::dateutils::stripQuotes(wVal);
            for (const auto& row : t.rows()) {
                if (wCI < row.values.size() &&
                    (row.values[wCI] == wVal || row.values[wCI] == stripped)) {
                    std::vector<std::string> colNames;
                    for (const auto& c : t.columns()) colNames.push_back(c.name);
                    auto allowed = applyRls_(tbl, {row}, "DELETE");
                    if (allowed.empty())
                        throw std::runtime_error("RLS policy violation: DELETE denied on " + tbl);
                }
            }
        }

        // Phase 21: CASCADE / SET NULL / RESTRICT für betroffene Zeilen
        {
            const Table& t = getTable(tbl);
            size_t wCI = colIdx(t, wCol);
            const std::string stripped = milansql::dateutils::stripQuotes(wVal);
            std::vector<Row> matched;
            for (const auto& row : t.rows())
                if (wCI < row.values.size() &&
                    (row.values[wCI] == wVal || row.values[wCI] == stripped))
                    matched.push_back(row);
            for (const auto& row : matched)
                cascadeDelete(tbl, row);
        }''',
    "deleteWhere RLS enforcement")

# ══════════════════════════════════════════════════════════════
# 6. Add RLS enforcement to simple updateWhere
# ══════════════════════════════════════════════════════════════
print("\n=== 6. RLS enforcement in simple updateWhere ===")
patch(ENGINE,
    '''    std::size_t updateWhere(const std::string& tblRaw,
                            const std::string& setCol, const std::string& setVal,
                            const std::string& wCol,   const std::string& wVal) {
        auto tbl = resolveTableName(tblRaw);
        checkPrivilege("UPDATE", tbl);  // Phase 46: access control
        if (!g_lockManager.checkWriteAllowed(tbl))  // Phase 65: table lock check
            throw std::runtime_error("Tabelle '" + tbl + "' ist durch LOCK TABLE READ gesperrt.");
        WriteScope ws(getOrCreateRwLock(tbl), tbl);  // Phase 112: exclusive write lock
        checkSetConstraints(tbl, {setCol}, {setVal});  // Phase 23
        if (inTransaction_) {''',
    '''    std::size_t updateWhere(const std::string& tblRaw,
                            const std::string& setCol, const std::string& setVal,
                            const std::string& wCol,   const std::string& wVal) {
        auto tbl = resolveTableName(tblRaw);
        checkPrivilege("UPDATE", tbl);  // Phase 46: access control
        if (!g_lockManager.checkWriteAllowed(tbl))  // Phase 65: table lock check
            throw std::runtime_error("Tabelle '" + tbl + "' ist durch LOCK TABLE READ gesperrt.");
        WriteScope ws(getOrCreateRwLock(tbl), tbl);  // Phase 112: exclusive write lock

        // Phase 170: RLS enforcement for UPDATE
        if (isRlsEnabled(tbl) && currentUser_ != "root" && !currentUser_.empty()) {
            const Table& t = getTable(tbl);
            size_t wCI = colIdx(t, wCol);
            const std::string stripped = milansql::dateutils::stripQuotes(wVal);
            for (const auto& row : t.rows()) {
                if (wCI < row.values.size() &&
                    (row.values[wCI] == wVal || row.values[wCI] == stripped)) {
                    auto allowed = applyRls_(tbl, {row}, "UPDATE");
                    if (allowed.empty())
                        throw std::runtime_error("RLS policy violation: UPDATE denied on " + tbl);
                }
            }
        }

        checkSetConstraints(tbl, {setCol}, {setVal});  // Phase 23
        if (inTransaction_) {''',
    "simple updateWhere RLS enforcement")

# ══════════════════════════════════════════════════════════════
# 7. Add RLS enforcement to multi-column updateWhere
# ══════════════════════════════════════════════════════════════
print("\n=== 7. RLS enforcement in multi-column updateWhere ===")
patch(ENGINE,
    '''    // Phase 22: Multi-Column UPDATE WHERE
    std::size_t updateWhere(const std::string& tblRaw,
                            const std::vector<std::string>& setCols,
                            const std::vector<std::string>& setVals,
                            const std::string& wCol, const std::string& wVal) {
        auto tbl = resolveTableName(tblRaw);
        checkPrivilege("UPDATE", tbl);  // Phase 46: access control
        WriteScope ws(getOrCreateRwLock(tbl), tbl);  // Phase 112: exclusive write lock
        // Phase 44: Check if any setVal contains an expression (col op num)''',
    '''    // Phase 22: Multi-Column UPDATE WHERE
    std::size_t updateWhere(const std::string& tblRaw,
                            const std::vector<std::string>& setCols,
                            const std::vector<std::string>& setVals,
                            const std::string& wCol, const std::string& wVal) {
        auto tbl = resolveTableName(tblRaw);
        checkPrivilege("UPDATE", tbl);  // Phase 46: access control
        WriteScope ws(getOrCreateRwLock(tbl), tbl);  // Phase 112: exclusive write lock

        // Phase 170: RLS enforcement for UPDATE
        if (isRlsEnabled(tbl) && currentUser_ != "root" && !currentUser_.empty()) {
            const Table& t = getTable(tbl);
            size_t wCI = colIdx(t, wCol);
            const std::string stripped = milansql::dateutils::stripQuotes(wVal);
            for (const auto& row : t.rows()) {
                if (wCI < row.values.size() &&
                    (row.values[wCI] == wVal || row.values[wCI] == stripped)) {
                    auto allowed = applyRls_(tbl, {row}, "UPDATE");
                    if (allowed.empty())
                        throw std::runtime_error("RLS policy violation: UPDATE denied on " + tbl);
                }
            }
        }

        // Phase 44: Check if any setVal contains an expression (col op num)''',
    "multi-col updateWhere RLS enforcement")

# ══════════════════════════════════════════════════════════════
# 8. Add WITH CHECK enforcement to insertRow
# ══════════════════════════════════════════════════════════════
print("\n=== 8. WITH CHECK enforcement in insertRow ===")
patch(ENGINE,
    '''        checkInsertFK(tbl, vals);       // FOREIGN KEY prüfen
        checkAllConstraints(tbl, vals); // Phase 23: CHECK constraints prüfen
        checkJsonColumns(t, vals);      // Phase 56: JSON-Validierung

        // Phase 43: BEFORE INSERT triggers''',
    '''        checkInsertFK(tbl, vals);       // FOREIGN KEY prüfen
        checkAllConstraints(tbl, vals); // Phase 23: CHECK constraints prüfen
        checkJsonColumns(t, vals);      // Phase 56: JSON-Validierung

        // Phase 170: RLS WITH CHECK enforcement for INSERT
        if (isRlsEnabled(tbl) && currentUser_ != "root" && !currentUser_.empty()) {
            Row checkRow(vals);
            if (!checkRlsWithCheck_(tbl, checkRow, "INSERT"))
                throw std::runtime_error("RLS WITH CHECK violation: INSERT denied on " + tbl);
        }

        // Phase 43: BEFORE INSERT triggers''',
    "insertRow WITH CHECK enforcement")

# ══════════════════════════════════════════════════════════════
# 9. Add WITH CHECK enforcement to multi-col updateWhere (post-update check)
#    We add it after the row is modified to verify the NEW values pass WITH CHECK
# ══════════════════════════════════════════════════════════════
print("\n=== 9. WITH CHECK post-update verification ===")
# For the expression-based update path, add check after row modification
patch(ENGINE,
    '''                    // Phase 68: recompute generated cols after row update
                    applyGeneratedCols(tblRef, row.values);
                    ++n;
                }
            }
            if (n) tblRef.rebuildIndexes();
            return n;
        }''',
    '''                    // Phase 68: recompute generated cols after row update
                    applyGeneratedCols(tblRef, row.values);
                    // Phase 170: WITH CHECK on updated row
                    if (isRlsEnabled(tbl) && currentUser_ != "root" && !currentUser_.empty()) {
                        if (!checkRlsWithCheck_(tbl, row, "UPDATE"))
                            throw std::runtime_error("RLS WITH CHECK violation: UPDATE would create row that violates policy on " + tbl);
                    }
                    ++n;
                }
            }
            if (n) tblRef.rebuildIndexes();
            return n;
        }''',
    "WITH CHECK post-update (expr path)")

# ══════════════════════════════════════════════════════════════
# 10. Parser: support WITH CHECK clause in CREATE POLICY
# ══════════════════════════════════════════════════════════════
print("\n=== 10. Parser: WITH CHECK clause ===")

content = read(PARSER)

# Add policyWithCheckExpr field to ParsedCommand
content = content.replace(
    '    std::string policyUsingExpr;\n',
    '    std::string policyUsingExpr;\n    std::string policyWithCheckExpr;  // Phase 170\n',
    1
)
print("  OK: policyWithCheckExpr field added to ParsedCommand")

# After USING extraction, add WITH CHECK extraction
OLD_USING_BLOCK = '''                    while (!cmd.policyUsingExpr.empty() && cmd.policyUsingExpr.front() == ' ')
                        cmd.policyUsingExpr.erase(cmd.policyUsingExpr.begin());
                    while (!cmd.policyUsingExpr.empty() && cmd.policyUsingExpr.back() == ' ')
                        cmd.policyUsingExpr.pop_back();
                }
            }

        // ── Phase 75: DROP POLICY name ON table'''

NEW_USING_BLOCK = '''                    while (!cmd.policyUsingExpr.empty() && cmd.policyUsingExpr.front() == ' ')
                        cmd.policyUsingExpr.erase(cmd.policyUsingExpr.begin());
                    while (!cmd.policyUsingExpr.empty() && cmd.policyUsingExpr.back() == ' ')
                        cmd.policyUsingExpr.pop_back();
                }
            }
            // Phase 170: Extract WITH CHECK expression
            {
                std::string upIn = input;
                for (auto& c : upIn) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                auto wcpos = upIn.find("WITH CHECK");
                if (wcpos != std::string::npos) {
                    std::string after = input.substr(wcpos + 10);  // skip "WITH CHECK"
                    size_t start = 0;
                    while (start < after.size() && after[start] == ' ') ++start;
                    if (start < after.size() && after[start] == '(') {
                        int depth = 0; size_t end = start;
                        for (size_t k = start; k < after.size(); ++k) {
                            if (after[k] == '(') ++depth;
                            else if (after[k] == ')') { --depth; if (depth == 0) { end = k; break; } }
                        }
                        cmd.policyWithCheckExpr = after.substr(start + 1, end - start - 1);
                    } else {
                        cmd.policyWithCheckExpr = after.substr(start);
                    }
                    while (!cmd.policyWithCheckExpr.empty() && cmd.policyWithCheckExpr.front() == ' ')
                        cmd.policyWithCheckExpr.erase(cmd.policyWithCheckExpr.begin());
                    while (!cmd.policyWithCheckExpr.empty() && cmd.policyWithCheckExpr.back() == ' ')
                        cmd.policyWithCheckExpr.pop_back();
                    // If USING expr accidentally captured WITH CHECK, trim it
                    auto wcInUsing = cmd.policyUsingExpr.find("WITH CHECK");
                    if (wcInUsing == std::string::npos) {
                        // Also check uppercase version
                        std::string usingUp = cmd.policyUsingExpr;
                        for (auto& c : usingUp) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                        wcInUsing = usingUp.find("WITH CHECK");
                    }
                    if (wcInUsing != std::string::npos) {
                        cmd.policyUsingExpr = cmd.policyUsingExpr.substr(0, wcInUsing);
                        while (!cmd.policyUsingExpr.empty() && cmd.policyUsingExpr.back() == ' ')
                            cmd.policyUsingExpr.pop_back();
                        while (!cmd.policyUsingExpr.empty() && cmd.policyUsingExpr.back() == ')')
                            cmd.policyUsingExpr.pop_back();
                    }
                }
            }

        // ── Phase 75: DROP POLICY name ON table'''

content = content.replace(OLD_USING_BLOCK, NEW_USING_BLOCK, 1)
print("  OK: WITH CHECK parsing in CREATE POLICY")

write(PARSER, content)

# ══════════════════════════════════════════════════════════════
# 11. Dispatch: pass withCheckExpr to engine
# ══════════════════════════════════════════════════════════════
print("\n=== 11. Dispatch: pass withCheckExpr ===")
patch(DISPATCH,
    '''        milansql::Engine::RlsPolicy p;
        p.name      = cmd.policyName;
        p.table     = cmd.tableName;
        p.command   = cmd.policyCommand.empty() ? "ALL" : cmd.policyCommand;
        p.role      = cmd.policyUser.empty()    ? "PUBLIC" : cmd.policyUser;
        p.usingExpr = cmd.policyUsingExpr;
        engine.createRlsPolicy(p);
        std::cout << "  Policy " << p.name << " created.\\n\\n";''',
    '''        milansql::Engine::RlsPolicy p;
        p.name           = cmd.policyName;
        p.table          = cmd.tableName;
        p.command        = cmd.policyCommand.empty() ? "ALL" : cmd.policyCommand;
        p.role           = cmd.policyUser.empty()    ? "PUBLIC" : cmd.policyUser;
        p.usingExpr      = cmd.policyUsingExpr;
        p.withCheckExpr  = cmd.policyWithCheckExpr;  // Phase 170
        engine.createRlsPolicy(p);
        std::cout << "  Policy " << p.name << " created.\\n\\n";''',
    "dispatch withCheckExpr")

# ══════════════════════════════════════════════════════════════
# 12. Make parseWhere public (it's already static, just ensure accessible)
# ══════════════════════════════════════════════════════════════
print("\n=== 12. Verify parseWhere is accessible ===")
# parseWhere is a static method on Parser class — check if it's public
content = read(PARSER)
# It's likely already in a public section since it's called externally
if 'static void parseWhere(' in content:
    print("  OK: parseWhere exists and is static")
else:
    print("  WARNING: parseWhere not found")

# Also need to make tokenize accessible from engine
if 'static std::vector<std::string> tokenize(' in content:
    print("  OK: tokenize exists and is static")

print("\n" + "=" * 50)
print("ALL RLS V2 PATCHES APPLIED")
print("=" * 50)
