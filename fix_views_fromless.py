#!/usr/bin/env python3
"""Fix Views and Procedures to handle FROM-less SELECT queries."""

DISPATCH = '/opt/milansql/src/dispatch.hpp'
with open(DISPATCH, 'r') as f:
    content = f.read()

changes = 0

# Fix materializeView — handle empty tableName (FROM-less SELECT)
old_materialize = '''    milansql::Table base;
    if (!vc.whereConds.empty()) {
        auto qr = engine.selectWhere(vc.tableName, vc.whereConds, vc.whereLogic);
        base = std::move(qr.table);
    } else {
        base = engine.selectAll(vc.tableName).clone();
    }'''

new_materialize = '''    milansql::Table base;
    if (vc.tableName.empty()) {
        // Phase 173: FROM-less SELECT (e.g., SELECT 1 AS val, SELECT expression)
        base = dispatch_executeSelectToTable(engine, parser, vc);
    } else if (!vc.whereConds.empty()) {
        auto qr = engine.selectWhere(vc.tableName, vc.whereConds, vc.whereLogic);
        base = std::move(qr.table);
    } else {
        base = engine.selectAll(vc.tableName).clone();
    }'''

if old_materialize in content:
    content = content.replace(old_materialize, new_materialize)
    changes += 1
    print("OK: materializeView FROM-less SELECT fixed")
else:
    print("WARN: materializeView pattern not found")

with open(DISPATCH, 'w') as f:
    f.write(content)

print(f"DONE: {changes} view fixes applied")
