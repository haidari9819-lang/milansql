#!/bin/bash
# MilanSQL Daily Health Check — v9.7.0
# Usage: bash scripts/daily-check.sh [--log]

DATE=$(date '+%Y-%m-%d %H:%M')
BASE="https://milansql.de"
LOG="logs/daily_health.log"

mkdir -p logs

echo "=== MilanSQL Daily Check ==="
echo "Date: $DATE"

# Health endpoint
HEALTH=$(curl -s --connect-timeout 10 "$BASE/health" 2>/dev/null)
if [ -z "$HEALTH" ]; then
  echo "ERROR: Server nicht erreichbar!"
  echo "$DATE | ERROR: unreachable" >> "$LOG"
  exit 1
fi

VERSION=$(echo "$HEALTH" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d.get('version','?'))" 2>/dev/null)
UPTIME=$(echo  "$HEALTH" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d.get('uptime_seconds','?'))" 2>/dev/null)

echo "Version:  $VERSION"
echo "Uptime:   ${UPTIME}s"

# Memory on server
MEMORY=$(ssh -o StrictHostKeyChecking=no -o ConnectTimeout=5 root@178.105.206.36 \
  "ps aux | grep '[m]ilansql' | awk '{print \$6}'" 2>/dev/null)
echo "Memory:   ${MEMORY:-n/a} KB"

# Quick query test
QTEST=$(curl -s --connect-timeout 5 -X POST "$BASE/query" \
  -H "Content-Type: application/json" \
  -d '{"sql":"SELECT 1"}' 2>/dev/null | python3 -c \
  "import sys,json; d=json.load(sys.stdin); print('OK' if d.get('success') else 'FAIL')" 2>/dev/null)
echo "Query:    $QTEST"

# Log entry
echo "$DATE | v=$VERSION | uptime=${UPTIME}s | mem=${MEMORY:-?}KB | query=$QTEST" >> "$LOG"

echo ""
echo "Full health:"
echo "$HEALTH" | python3 -m json.tool 2>/dev/null || echo "$HEALTH"
