#!/bin/bash
# MilanSQL Release Push Script
# Usage: ./scripts/push-release.sh "Phase 125: Load Balancer"

set -e

COMMIT_MSG="${1:-"chore: update release branch"}"

export GIT_AUTHOR_NAME="Mirwais Haidari"
export GIT_AUTHOR_EMAIL="haidari9819@gmail.com"
export GIT_COMMITTER_NAME="Mirwais Haidari"
export GIT_COMMITTER_EMAIL="haidari9819@gmail.com"

echo "=== MilanSQL Release Push ==="
echo "Message: $COMMIT_MSG"

# 1. Tests laufen lassen
echo ""
echo "Running tests..."
export PATH="/c/msys64/ucrt64/bin:$PATH"
cmake --build build --target milansql_tests -j4 2>&1 | tail -3
./build/milansql_tests.exe 2>&1 | tail -5
echo ""

# 2. main Branch pushen
echo "Pushing to main..."
git add -A
git push origin main
echo "✓ main updated"

# 3. release Branch aktualisieren (orphan — nutze checkout statt merge)
echo ""
echo "Updating release branch..."
git checkout release

# Alle Dateien von main übernehmen (funktioniert auch bei unrelated histories)
git checkout main -- .

git commit \
  --author="Mirwais Haidari <haidari9819@gmail.com>" \
  -m "$COMMIT_MSG"

git push origin release
echo "✓ release updated"

# 4. Zurück zu main
git checkout main

echo ""
echo "=== Done! ==="
echo "✓ main:    $(git rev-parse --short main)"
echo "✓ release: $(git rev-parse --short release)"
echo ""
echo "GitHub: https://github.com/haidari9819-lang/milansql"
