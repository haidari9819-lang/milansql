#!/bin/bash
# MilanSQL Chaos Test Suite v11.5.0
# Phase 1.4: Billion-Scale Roadmap Chaos Testing

API="https://milansql.de"
TOKEN=""
FAILS=0

# Login
login() {
  local PW
  PW=$(grep DB_ROOT_PW /etc/milansql/db.conf | cut -d"'" -f2)
  if [ -z "$PW" ]; then
    echo "ERROR: Cannot read DB_ROOT_PW from /etc/milansql/db.conf"
    exit 1
  fi
  TOKEN=$(curl -s -k -X POST "$API/auth/login" \
    -H "Content-Type: application/json" \
    -d "{\"username\":\"root\",\"password\":\"$PW\"}" \
    | python3 -c "import sys,json; print(json.load(sys.stdin).get('token',''))" 2>/dev/null)
  if [ -z "$TOKEN" ]; then
    echo "ERROR: Login failed"
    exit 1
  fi
  echo "Login OK (token: ${TOKEN:0:16}...)"
}

query() {
  curl -s -k -X POST "$API/api/query" \
    -H "Authorization: Bearer $TOKEN" \
    -H "Content-Type: application/json" \
    -d "{\"sql\":\"$1\"}"
}

# Extract COUNT(*) value from MilanSQL response
# Response: {"success":true,"message":"COUNT(*) = N (Tabelle 'X')"}
# or rows array: {"rows":[[N]]}
extract_count() {
  local resp="$1"
  # Try rows array first
  local v
  v=$(echo "$resp" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d['rows'][0][0] if d.get('rows') else '')" 2>/dev/null)
  if [ -z "$v" ]; then
    # Try message field: "COUNT(*) = 50 (Tabelle 'chaos1')"
    v=$(echo "$resp" | python3 -c "import sys,json,re; d=json.load(sys.stdin); m=re.search(r'COUNT\(\*\) = (\d+)', d.get('message','')); print(m.group(1) if m else '')" 2>/dev/null)
  fi
  echo "$v"
}

pass() { echo "PASS: $1"; }
fail() { echo "FAIL: $1"; FAILS=$((FAILS+1)); }

login

# === TEST 1: Restart under load — persistence check ===
echo ""
echo "=== Test 1: Kill+Restart under load ==="
query "CREATE TABLE IF NOT EXISTS chaos1 (id INT, val TEXT)" > /dev/null
query "DELETE FROM chaos1" > /dev/null
# Insert 50 rows before restart
for i in $(seq 1 50); do
  query "INSERT INTO chaos1 VALUES ($i, 'committed_$i')" > /dev/null
done
COUNT_BEFORE=$(extract_count "$(query "SELECT COUNT(*) FROM chaos1")")
echo "  Before restart: $COUNT_BEFORE rows"

# Restart service
systemctl restart milansql
sleep 4
login

COUNT_AFTER=$(extract_count "$(query "SELECT COUNT(*) FROM chaos1")")
echo "  After restart: $COUNT_AFTER rows"
if [ "$COUNT_AFTER" = "50" ]; then
  pass "Kill+Restart: Data persistent (50 rows)"
else
  fail "Kill+Restart: Expected 50, got $COUNT_AFTER"
fi

# === TEST 2: Concurrent Schema Changes ===
echo ""
echo "=== Test 2: Concurrent Schema Changes ==="
query "CREATE TABLE IF NOT EXISTS chaos2 (id INT, name TEXT)" > /dev/null
query "DELETE FROM chaos2" > /dev/null
for i in $(seq 1 100); do
  query "INSERT INTO chaos2 VALUES ($i, 'row$i')" > /dev/null
done

# Run concurrent SELECTs in background while doing ALTER
(for i in $(seq 1 20); do
  query "SELECT * FROM chaos2 LIMIT 10" > /dev/null
done) &
BGPID=$!
query "ALTER TABLE chaos2 ADD COLUMN IF NOT EXISTS extra TEXT" > /dev/null
wait $BGPID

RESULT=$(extract_count "$(query "SELECT COUNT(*) FROM chaos2")")
if [ "$RESULT" = "100" ]; then
  pass "Concurrent Schema Change: 100 rows preserved"
else
  fail "Concurrent Schema Change: expected 100, got $RESULT"
fi

# === TEST 3: 100 parallel queries ===
echo ""
echo "=== Test 3: Parallel Queries ==="
query "CREATE TABLE IF NOT EXISTS chaos3 (id INT)" > /dev/null
query "DELETE FROM chaos3" > /dev/null
for i in $(seq 1 100); do
  query "INSERT INTO chaos3 VALUES ($i)" > /dev/null
done

START=$(date +%s%N 2>/dev/null || date +%s)
for i in $(seq 1 100); do
  query "SELECT COUNT(*) FROM chaos3" > /dev/null &
done
wait
END=$(date +%s%N 2>/dev/null || date +%s)
# Check if date +%s%N works (ns) or falls back to %s (s)
if [ ${#END} -gt 12 ]; then
  MS=$(( (END - START) / 1000000 ))
else
  MS=$(( (END - START) * 1000 ))
fi
if [ $MS -lt 30000 ]; then
  pass "100 parallel queries in ${MS}ms"
else
  fail "100 parallel queries too slow: ${MS}ms"
fi

# === TEST 4: Large Insert (Memory Pressure) ===
echo ""
echo "=== Test 4: Memory Pressure (1000 rows) ==="
query "CREATE TABLE IF NOT EXISTS chaos4 (id INT, data TEXT)" > /dev/null
query "DELETE FROM chaos4" > /dev/null
for i in $(seq 1 1000); do
  query "INSERT INTO chaos4 VALUES ($i, 'data_$(date +%N | head -c 8 2>/dev/null || echo $i)')" > /dev/null
done
RESULT=$(extract_count "$(query "SELECT COUNT(*) FROM chaos4")")
if [ "$RESULT" = "1000" ]; then
  pass "Memory Pressure: 1000 rows inserted"
else
  fail "Memory Pressure: expected 1000, got $RESULT"
fi

# === TEST 5: ROLLBACK correctness ===
echo ""
echo "=== Test 5: ROLLBACK correctness ==="
query "CREATE TABLE IF NOT EXISTS chaos5 (id INT)" > /dev/null
query "DELETE FROM chaos5" > /dev/null
query "BEGIN" > /dev/null
query "INSERT INTO chaos5 VALUES (999)" > /dev/null
query "ROLLBACK" > /dev/null
RESULT=$(query "SELECT COUNT(*) FROM chaos5")
ROWS=$(extract_count "$RESULT")
if [ "$ROWS" = "0" ]; then
  pass "ROLLBACK: Data correctly rolled back"
else
  fail "ROLLBACK: Data still present (rows=$ROWS)"
fi

# === TEST 6: /metrics endpoint ===
echo ""
echo "=== Test 6: /metrics Prometheus format ==="
METRICS=$(curl -s -k -H "Authorization: Bearer $TOKEN" "$API/metrics")
if echo "$METRICS" | grep -q "milansql_queries_total"; then
  pass "/metrics: milansql_queries_total present"
else
  fail "/metrics: milansql_queries_total missing"
fi
if echo "$METRICS" | grep -q "milansql_query_duration_seconds"; then
  pass "/metrics: milansql_query_duration_seconds present"
else
  fail "/metrics: milansql_query_duration_seconds missing"
fi

# === TEST 7: Health sub-endpoints ===
echo ""
echo "=== Test 7: Health sub-endpoints ==="
LIVE=$(curl -s -k "$API/health/live")
if echo "$LIVE" | grep -q '"status":"ok"'; then
  pass "/health/live: status ok"
else
  fail "/health/live: unexpected response: $LIVE"
fi
READY=$(curl -s -k "$API/health/ready")
if echo "$READY" | grep -q '"status":"ready"'; then
  pass "/health/ready: status ready"
else
  fail "/health/ready: unexpected response: $READY"
fi
STARTUP=$(curl -s -k "$API/health/startup")
if echo "$STARTUP" | grep -q '"status":"started"'; then
  pass "/health/startup: status started"
else
  fail "/health/startup: unexpected response: $STARTUP"
fi

# Cleanup
echo ""
echo "=== Cleanup ==="
for t in chaos1 chaos2 chaos3 chaos4 chaos5; do
  query "DROP TABLE IF EXISTS $t" > /dev/null
done
echo "Cleanup done."

echo ""
echo "========================================"
if [ $FAILS -eq 0 ]; then
  echo "ALL Chaos Tests PASSED!"
else
  echo "$FAILS Chaos Test(s) FAILED"
fi
echo "========================================"
exit $FAILS
