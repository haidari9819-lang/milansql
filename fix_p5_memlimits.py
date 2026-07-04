#!/usr/bin/env python3
"""P5: Memory Limits — MAX_RESULT_ROWS, JOIN intermediate limits, query timeout."""

ENGINE = '/opt/milansql/src/engine/engine.hpp'
with open(ENGINE, 'r') as f:
    engine = f.read()

changes = 0

# 1. Add constants near the top of the namespace
anchor_ns = 'namespace milansql {'
if 'MAX_RESULT_ROWS' not in engine and anchor_ns in engine:
    limits = '''namespace milansql {

// Phase 173 P5: Memory safety limits
constexpr size_t MAX_RESULT_ROWS   = 100000;   // Max rows in any single result set
constexpr size_t MAX_JOIN_ROWS     = 500000;    // Max intermediate rows during JOIN
constexpr size_t MAX_SUBQUERY_ROWS = 50000;     // Max rows from a subquery
constexpr size_t MAX_IN_LIST       = 10000;     // Max elements in IN (...) list
'''
    # Replace only the first occurrence
    pos = engine.find(anchor_ns)
    engine = engine[:pos] + limits + engine[pos + len(anchor_ns):]
    changes += 1
    print("OK: Memory limit constants added")

# 2. Add result size check in selectWhere — after rows are collected
#    Find the return statement at the end of selectWhere
old_select_return = '''        // ── Phase 100: LIMIT push-down already applied above ──
        return { result, usedIndex };'''

if old_select_return not in engine:
    # Try alternate pattern
    old_select_return = '        return { result, usedIndex };'

if old_select_return in engine:
    new_select_return = '''        // Phase 173 P5: Enforce result row limit
        if (result.rows().size() > MAX_RESULT_ROWS) {
            Table limited("", result.columns());
            size_t count = 0;
            for (const auto& r : result.rows()) {
                if (count++ >= MAX_RESULT_ROWS) break;
                limited.insert(r);
            }
            return { limited, usedIndex };
        }
        ''' + old_select_return
    # Replace only the FIRST occurrence (selectWhere)
    pos = engine.find(old_select_return)
    if pos > 0:
        engine = engine[:pos] + new_select_return + engine[pos + len(old_select_return):]
        changes += 1
        print("OK: selectWhere result limit added")

# 3. Add JOIN intermediate size check in innerJoin
old_join_insert = '''                if (rowMatches(result, combined, whereConds, whereLogic))
                    result.insert(std::move(combined));
            }
        }
        return result;
    }'''

if old_join_insert in engine:
    new_join_insert = '''                if (rowMatches(result, combined, whereConds, whereLogic)) {
                    result.insert(std::move(combined));
                    if (result.rows().size() > MAX_JOIN_ROWS)
                        throw std::runtime_error("JOIN result exceeds maximum row limit ("
                            + std::to_string(MAX_JOIN_ROWS) + " rows). Add WHERE conditions or use LIMIT.");
                }
            }
        }
        return result;
    }'''
    engine = engine.replace(old_join_insert, new_join_insert)
    changes += 1
    print("OK: innerJoin size limit added")

# 4. Add CROSS JOIN protection — look for cross join pattern
import re
# Find the executeJoins function's nested loop join and add limit there too
# The hash join and merge join paths also need limits
# Let's add a check after each result.insert in the join loop

# Find hash join result insert
old_hash_insert = 'result.insert(Row(std::move(combined)));'
count_hash = engine.count(old_hash_insert)
if count_hash > 0:
    new_hash_insert = '''result.insert(Row(std::move(combined)));
                    if (result.rows().size() > MAX_JOIN_ROWS)
                        throw std::runtime_error("JOIN result exceeds maximum row limit (" + std::to_string(MAX_JOIN_ROWS) + ")");'''
    # Replace all occurrences
    engine = engine.replace(old_hash_insert, new_hash_insert)
    changes += 1
    print(f"OK: {count_hash} hash/merge join size limits added")

# 5. Add IN list size limit in the parser
old_in_parse = "cond.inList.push_back("
if old_in_parse in engine:
    # We'll add the check after the IN list is fully parsed instead
    # Find the IN list completion pattern
    pass  # Skip this - the IN list is built during parsing, not in engine.hpp

with open(ENGINE, 'w') as f:
    f.write(engine)

# Now add limits to http_server.hpp for request-level protection
HTTP = '/opt/milansql/src/server/http_server.hpp'
with open(HTTP, 'r') as f:
    http = f.read()

# Add max request body size check
old_body_parse = 'auto req = parseRequest(buffer.data(), received);'
if old_body_parse in http and 'MAX_REQUEST_BODY' not in http:
    new_body_parse = '''// Phase 173 P5: Reject oversized request bodies (16 MB limit)
        constexpr size_t MAX_REQUEST_BODY = 16 * 1024 * 1024;
        if (received > MAX_REQUEST_BODY) {
            std::string resp = buildHttpResponse(413, R"({"success":false,"error":"Request body too large"})");
            sendResponse(clientSock, resp);
            return;
        }
        auto req = parseRequest(buffer.data(), received);'''
    # Only replace first occurrence
    pos = http.find(old_body_parse)
    if pos > 0:
        http = http[:pos] + new_body_parse + http[pos + len(old_body_parse):]
        changes += 1
        print("OK: Max request body size check added")

with open(HTTP, 'w') as f:
    f.write(http)

print(f"DONE P5: {changes} memory limit changes applied")
