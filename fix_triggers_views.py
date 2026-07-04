#!/usr/bin/env python3
"""Fix Trigger name mismatch + Views/Procedures FROM-less query support."""

ENGINE = '/opt/milansql/src/engine/engine.hpp'
with open(ENGINE, 'r') as f:
    engine = f.read()

changes = 0

# ═══════════════════════════════════════════════════════════════
# FIX 1: Trigger tableName comparison — resolve both sides
# ═══════════════════════════════════════════════════════════════

# Fix in fireAllTriggers (line ~5488)
old_fire = '            if (trg.tableName != tableName)                    continue;'
new_fire = '            if (resolveTableName(trg.tableName) != resolvedTbl) continue;'
if old_fire in engine:
    engine = engine.replace(old_fire, new_fire)
    changes += 1
    print("OK: fireAllTriggers comparison fixed (resolveTableName)")

# Fix in showTriggers (line ~3934)
old_show = '            if (trg.tableName              != tableName)         continue;'
if old_show in engine:
    new_show = '            if (resolveTableName(trg.tableName) != resolveTableName(tableName)) continue;'
    engine = engine.replace(old_show, new_show)
    changes += 1
    print("OK: showTriggers comparison fixed")

# ═══════════════════════════════════════════════════════════════
# FIX 2: Views — handle FROM-less SELECT (e.g., SELECT 1 AS val)
# Find the view materialization code
# ═══════════════════════════════════════════════════════════════

# Find where views are executed — look for the view expansion logic
import re

# Look for the view execution in selectAll or similar
view_exec_patterns = [
    'views_',
    'viewQuery',
    'vc.tableName',
]

# Find the actual view materialization
lines = engine.split('\n')
for i, line in enumerate(lines):
    if 'vc.tableName' in line or ('views_' in line and 'sql' in line.lower()):
        print(f"  View ref line {i+1}: {line.strip()[:80]}")

# The view issue: when a view's SQL has no FROM, the parser sets tableName to ""
# and selectAll("") fails. We need to check if the view SQL should be executed
# as a full query rather than decomposed.
# Let's find how views are currently handled

for i, line in enumerate(lines):
    if 'views_.count' in line or 'views_.find' in line:
        print(f"  View check line {i+1}: {line.strip()[:80]}")
        # Print surrounding context
        for j in range(max(0,i-2), min(len(lines), i+8)):
            print(f"    {j+1}: {lines[j].rstrip()[:100]}")
        print()

with open(ENGINE, 'w') as f:
    f.write(engine)

print(f"DONE: {changes} trigger fixes applied")
print("NOTE: View context printed above — manual review needed for FROM-less fix")
