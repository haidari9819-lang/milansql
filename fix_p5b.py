#!/usr/bin/env python3
"""P5b: Add result row limits at selectWhere return + JOIN executeJoins."""

ENGINE = '/opt/milansql/src/engine/engine.hpp'
with open(ENGINE, 'r') as f:
    engine = f.read()

changes = 0

# 1. Add limit check before the final return in selectWhere (line 1701)
old_return = '        return {std::move(result), usedIndex};'
# There are two of these — we want to add the check before both
# Actually we should truncate at the end, just before the LAST return
# Find the last occurrence
last_pos = engine.rfind(old_return)
first_pos = engine.find(old_return)

# Add the check before each return
limit_check = '''        // Phase 173 P5: Enforce result row limit
        if (result.rows().size() > MAX_RESULT_ROWS) {
            auto& rs = result.rows_ref();
            rs.resize(MAX_RESULT_ROWS);
        }
        '''

# But wait, rows_ is private. Let's use a different approach — truncate in the Table class
# Actually, let's add a truncate method to Table

# Find Table class and add truncateRows method
old_table_clear = '    void clear() { rows_.clear(); }'
if old_table_clear in engine:
    new_table_clear = '''    void clear() { rows_.clear(); }
    // Phase 173 P5: Truncate rows to limit
    void truncateRows(size_t maxRows) {
        if (rows_.size() > maxRows) rows_.resize(maxRows);
    }'''
    engine = engine.replace(old_table_clear, new_table_clear)
    changes += 1
    print("OK: Table::truncateRows added")

# Now add truncation before selectWhere returns
new_return = '''        result.truncateRows(MAX_RESULT_ROWS);
        return {std::move(result), usedIndex};'''

engine = engine.replace(old_return, new_return)
changes += 1
print(f"OK: selectWhere result limit added at {engine.count('truncateRows')} points")

# 2. Add limit in executeJoins — find the main join loop result insertion
# Look for the probe phase result insert in executeJoins
import re

# Find executeJoins and add a limit after the main result building loop
# The function returns "return result;" at the end
# Let's add truncation before all "return result;" inside JOIN functions
old_join_return = '''        // Phase 113: DP-Join-Optimierer'''
if old_join_return in engine:
    # This is before the DP optimizer. Let's find the simpler approach:
    # Add limit check in the hash join probe phase
    pass

# Find the hash join build/probe section
hash_join_probe = engine.find('// Probe phase:')
if hash_join_probe > 0:
    print(f"Found hash join probe at offset {hash_join_probe}")
    # Find the result.insert after the probe
    probe_insert = engine.find('result.insert(', hash_join_probe)
    if probe_insert > 0:
        # Find the end of this line
        eol = engine.find('\n', probe_insert)
        line = engine[probe_insert:eol]
        print(f"  Probe insert: {line.strip()}")

# Simpler approach: add truncation at the end of executeJoins
# Find "return result;" that belongs to executeJoins
exec_join_start = engine.find('Table executeJoins(')
if exec_join_start > 0:
    # Find the return result; within this function (the last one before next function)
    # Look for the closing of this function
    search_from = exec_join_start
    last_return_result = -1
    while True:
        pos = engine.find('return result;', search_from)
        if pos < 0 or pos > exec_join_start + 50000:
            break
        last_return_result = pos
        search_from = pos + 1

    if last_return_result > 0:
        old_jr = 'return result;'
        # We need to be specific — only replace this exact position
        new_jr = 'result.truncateRows(MAX_RESULT_ROWS);\n        return result;'
        engine = engine[:last_return_result] + new_jr + engine[last_return_result + len(old_jr):]
        changes += 1
        print(f"OK: executeJoins result limit added at offset {last_return_result}")

# 3. Add the request body size check in http_server.hpp
HTTP = '/opt/milansql/src/server/http_server.hpp'
with open(HTTP, 'r') as f:
    http = f.read()

# Find the parseRequest call and add body size check before it
old_parse = '        auto req = parseRequest(buffer.data(), received);'
if old_parse in http and 'MAX_REQUEST_BODY' not in http:
    new_parse = '''        // Phase 173 P5: Reject oversized request bodies (16 MB)
        constexpr size_t MAX_REQUEST_BODY = 16 * 1024 * 1024;
        if (received > MAX_REQUEST_BODY) {
            std::string resp = buildHttpResponse(413, R"({\"success\":false,\"error\":\"Request body too large\"})");
            sendResponse(clientSock, resp);
            continue;
        }
        auto req = parseRequest(buffer.data(), received);'''
    pos = http.find(old_parse)
    http = http[:pos] + new_parse + http[pos + len(old_parse):]
    changes += 1
    print("OK: Request body size limit added")

with open(HTTP, 'w') as f:
    http_content = http

with open(ENGINE, 'w') as f:
    f.write(engine)

with open(HTTP, 'w') as f:
    f.write(http_content)

print(f"DONE P5b: {changes} changes applied")
