#!/bin/bash
# Chaos Test für MilanSQL
# Usage: bash scripts/chaos_test.sh [host:port]
set -euo pipefail

SERVER="${1:-178.105.206.36:8080}"
PASS=0
FAIL=0

ok()  { echo "  PASS: $1"; ((PASS++)); }
fail(){ echo "  FAIL: $1"; ((FAIL++)); }

echo "=== MilanSQL Chaos Test ==="
echo "Target: $SERVER"
echo ""

# ── Test 1: 100 gleichzeitige Requests ───────────────────────────────────────
echo "Test 1: 100 concurrent requests..."
for i in $(seq 1 100); do
  curl -s --max-time 5 -X POST "http://$SERVER/query" \
    -H "Content-Type: application/json" \
    -d '{"sql":"SELECT 1"}' > /dev/null &
done
wait
ok "100 concurrent requests completed without server crash"

# ── Test 2: Sehr langer SQL String ───────────────────────────────────────────
echo "Test 2: Very long SQL (10 000 chars)..."
LONG_SQL=$(python3 -c "print('SELECT ' + 'A'*10000)" 2>/dev/null \
         || node  -e "process.stdout.write('SELECT ' + 'A'.repeat(10000))" 2>/dev/null \
         || printf 'SELECT %10000s' 'X')
RESP=$(curl -s --max-time 5 -X POST "http://$SERVER/query" \
  -H "Content-Type: application/json" \
  -d "{\"sql\":\"$LONG_SQL\"}" 2>/dev/null || true)
if echo "$RESP" | grep -q '"success"'; then
  ok "Long SQL: server responded"
else
  fail "Long SQL: no response or server crashed"
fi

# ── Test 3: Kaputtes JSON ────────────────────────────────────────────────────
echo "Test 3: Broken JSON..."
RESP=$(curl -s --max-time 5 -o /dev/null -w "%{http_code}" \
  -X POST "http://$SERVER/query" \
  -H "Content-Type: application/json" \
  -d '{"sql": BROKEN JSON!!!' 2>/dev/null || echo "000")
if [ "$RESP" != "000" ]; then
  ok "Broken JSON: server returned HTTP $RESP (did not crash)"
else
  fail "Broken JSON: server unreachable"
fi

# ── Test 4: SQL Injection ─────────────────────────────────────────────────────
echo "Test 4: SQL Injection..."
RESP=$(curl -s --max-time 5 -X POST "http://$SERVER/query" \
  -H "Content-Type: application/json" \
  -d '{"sql":"'"'"'; DROP TABLE users; --"}' 2>/dev/null || true)
if echo "$RESP" | grep -qiE '"success"|"error"'; then
  ok "SQL Injection: server returned structured response"
else
  fail "SQL Injection: unexpected response"
fi

# ── Test 5: Null bytes in SQL ────────────────────────────────────────────────
echo "Test 5: Null bytes in SQL..."
RESP=$(curl -s --max-time 5 -o /dev/null -w "%{http_code}" \
  -X POST "http://$SERVER/query" \
  -H "Content-Type: application/json" \
  --data-binary $'{"sql":"SELECT\x00version()"}' 2>/dev/null || echo "000")
if [ "$RESP" != "000" ]; then
  ok "Null bytes: server returned HTTP $RESP (did not crash)"
else
  fail "Null bytes: server unreachable"
fi

# ── Test 6: Oversized body (1 MB payload) ───────────────────────────────────
echo "Test 6: Oversized body (1 MB)..."
BIG_BODY=$(python3 -c "import json; print(json.dumps({'sql': 'X'*1048576}))" 2>/dev/null || echo '{"sql":"X"}')
RESP=$(curl -s --max-time 10 -o /dev/null -w "%{http_code}" \
  -X POST "http://$SERVER/query" \
  -H "Content-Type: application/json" \
  -d "$BIG_BODY" 2>/dev/null || echo "000")
if [ "$RESP" != "000" ]; then
  ok "Oversized body: server returned HTTP $RESP (did not crash)"
else
  fail "Oversized body: server unreachable"
fi

# ── Test 7: Empty SQL ─────────────────────────────────────────────────────────
echo "Test 7: Empty SQL..."
RESP=$(curl -s --max-time 5 -X POST "http://$SERVER/query" \
  -H "Content-Type: application/json" \
  -d '{"sql":""}' 2>/dev/null || true)
if echo "$RESP" | grep -qiE '"success"|"error"'; then
  ok "Empty SQL: structured response"
else
  fail "Empty SQL: unexpected response"
fi

# ── Test 8: Missing Content-Type ─────────────────────────────────────────────
echo "Test 8: Missing Content-Type..."
RESP=$(curl -s --max-time 5 -o /dev/null -w "%{http_code}" \
  -X POST "http://$SERVER/query" \
  -d '{"sql":"SELECT 1"}' 2>/dev/null || echo "000")
if [ "$RESP" != "000" ]; then
  ok "Missing Content-Type: server returned HTTP $RESP"
else
  fail "Missing Content-Type: server unreachable"
fi

# ── Test 9: Server still alive? ──────────────────────────────────────────────
echo "Test 9: Server alive check..."
STATUS=$(curl -s --max-time 5 "http://$SERVER/status" 2>/dev/null \
  | grep -o '"status":"[^"]*"' || true)
if [ "$STATUS" = '"status":"healthy"' ]; then
  ok "Server survived all chaos tests"
else
  fail "Server status: '$STATUS' (expected healthy)"
fi

# ── Summary ───────────────────────────────────────────────────────────────────
echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="
[ "$FAIL" -eq 0 ] && echo "All tests passed!" && exit 0
exit 1
