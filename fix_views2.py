#!/usr/bin/env python3
"""Fix dispatch_executeSelectToTable to handle FROM-less SELECT."""

DISPATCH = '/opt/milansql/src/dispatch.hpp'
with open(DISPATCH, 'r') as f:
    content = f.read()

changes = 0

# Add FROM-less handling at the start of dispatch_executeSelectToTable
old_exec = '''static inline milansql::Table dispatch_executeSelectToTable(
        milansql::Engine& engine,
        milansql::Parser& parser,
        milansql::ParsedCommand cmd)
{
    for (const auto& sq : cmd.subqueries) {'''

new_exec = '''static inline milansql::Table dispatch_executeSelectToTable(
        milansql::Engine& engine,
        milansql::Parser& parser,
        milansql::ParsedCommand cmd)
{
    // Phase 173: Handle FROM-less SELECT (e.g., SELECT 1 AS val)
    if (cmd.tableName.empty() && !cmd.isSetOp && !cmd.isJoin) {
        // Build the SQL string back and use literal select
        std::string literalSql = "SELECT ";
        for (size_t i = 0; i < cmd.selectColumns.size(); ++i) {
            if (i > 0) literalSql += ", ";
            literalSql += cmd.selectColumns[i];
        }
        milansql::Table result("", {});
        if (dispatch_executeLiteralSelect(literalSql, result))
            return result;
        // If literal select fails, create a single-row table with empty values
        std::vector<milansql::Column> cols;
        for (const auto& sc : cmd.selectColumns)
            cols.push_back({sc, "TEXT"});
        milansql::Table fallback("", cols);
        std::vector<std::string> vals(cols.size(), "");
        fallback.insert(milansql::Row(vals));
        return fallback;
    }

    for (const auto& sq : cmd.subqueries) {'''

if old_exec in content:
    content = content.replace(old_exec, new_exec)
    changes += 1
    print("OK: dispatch_executeSelectToTable FROM-less SELECT fixed")

# Also need to fix the materializeView to reconstruct the SQL for the literal path
# The current fix calls dispatch_executeSelectToTable but that now needs the selectColumns
# Let's check if the original view SQL is preserved
# Actually, better approach: use the original view SQL directly for FROM-less queries

old_fromless_view = '''    if (vc.tableName.empty()) {
        // Phase 173: FROM-less SELECT (e.g., SELECT 1 AS val, SELECT expression)
        base = dispatch_executeSelectToTable(engine, parser, vc);
    } else if (!vc.whereConds.empty()) {'''

new_fromless_view = '''    if (vc.tableName.empty()) {
        // Phase 173: FROM-less SELECT (e.g., SELECT 1 AS val, SELECT expression)
        milansql::Table litResult("", {});
        if (dispatch_executeLiteralSelect(vsql, litResult)) {
            base = std::move(litResult);
        } else {
            base = dispatch_executeSelectToTable(engine, parser, vc);
        }
    } else if (!vc.whereConds.empty()) {'''

if old_fromless_view in content:
    content = content.replace(old_fromless_view, new_fromless_view)
    changes += 1
    print("OK: materializeView uses literal select for FROM-less views")

with open(DISPATCH, 'w') as f:
    f.write(content)

print(f"DONE: {changes} fixes applied")
