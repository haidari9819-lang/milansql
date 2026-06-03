# MilanSQL Release Push Script (PowerShell)
# Usage: .\scripts\push-release.ps1 "Phase 125: Load Balancer"

param([string]$CommitMsg = "chore: update release branch")

$env:GIT_AUTHOR_NAME = "Mirwais Haidari"
$env:GIT_AUTHOR_EMAIL = "haidari9819@gmail.com"
$env:GIT_COMMITTER_NAME = "Mirwais Haidari"
$env:GIT_COMMITTER_EMAIL = "haidari9819@gmail.com"
$env:PATH = "C:\msys64\ucrt64\bin;" + $env:PATH

Write-Host "=== MilanSQL Release Push ===" -ForegroundColor Cyan
Write-Host "Message: $CommitMsg" -ForegroundColor Yellow

# 1. Tests
Write-Host "`nRunning tests..." -ForegroundColor Yellow
cmake --build build --target milansql_tests -j4 2>&1 | Select-Object -Last 3
.\build\milansql_tests.exe 2>&1 | Select-Object -Last 5

# 2. main pushen
Write-Host "`nPushing to main..." -ForegroundColor Yellow
git add -A
git push origin main
Write-Host "✓ main updated" -ForegroundColor Green

# 3. release Branch
Write-Host "`nUpdating release branch..." -ForegroundColor Yellow
git checkout release
git merge main --squash -X theirs
git commit --author="Mirwais Haidari <haidari9819@gmail.com>" -m $CommitMsg
git push origin release
Write-Host "✓ release updated" -ForegroundColor Green

# 4. Zurück zu main
git checkout main

$mainHash = git rev-parse --short main
$releaseHash = git rev-parse --short release

Write-Host "`n=== Done! ===" -ForegroundColor Cyan
Write-Host "✓ main:    $mainHash" -ForegroundColor Green
Write-Host "✓ release: $releaseHash" -ForegroundColor Green
Write-Host "`nGitHub: https://github.com/haidari9819-lang/milansql"
