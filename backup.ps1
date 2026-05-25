# =============================================================================
# backup.ps1  —  BADT automatic git backup
# =============================================================================
#
# Run this manually or via Windows Task Scheduler (see README or the schtasks
# command at the bottom of this file) to commit and push any uncommitted
# changes to GitHub.
#
# What it does:
#   1. Checks whether anything has changed since the last commit.
#   2. If yes: stages all tracked + new files, commits with a timestamp, pushes.
#   3. If no:  does nothing (no empty commits).
#
# To run manually from PowerShell:
#   cd "C:\Users\Will\Documents\Making Plugins\BADT"
#   .\backup.ps1
#
# =============================================================================

$projectDir = "C:\Users\Will\Documents\Making Plugins\BADT"
$logFile    = "$projectDir\backup.log"

Set-Location $projectDir

# Check whether there is anything to commit.
# git status --porcelain prints one line per changed/untracked file.
# If the output is empty, the working tree is clean.
$changes = git status --porcelain 2>&1
if (-not $changes) {
    $msg = "$(Get-Date -Format 'yyyy-MM-dd HH:mm')  No changes — nothing to commit."
    Write-Host $msg
    Add-Content $logFile $msg
    exit 0
}

# Stage everything that isn't ignored by .gitignore.
# -A includes new files, modified files, and deleted files.
git add -A

# Commit with an ISO-style timestamp so you can find any backup by date.
$timestamp = Get-Date -Format "yyyy-MM-dd HH:mm"
$commitMsg = "Auto-backup $timestamp"

git commit -m $commitMsg
if ($LASTEXITCODE -ne 0) {
    $msg = "$(Get-Date -Format 'yyyy-MM-dd HH:mm')  ERROR: git commit failed."
    Write-Host $msg -ForegroundColor Red
    Add-Content $logFile $msg
    exit 1
}

# Push to origin/main.
git push origin main
if ($LASTEXITCODE -ne 0) {
    $msg = "$(Get-Date -Format 'yyyy-MM-dd HH:mm')  ERROR: git push failed (no internet?)."
    Write-Host $msg -ForegroundColor Yellow
    Add-Content $logFile $msg
    exit 1
}

$msg = "$(Get-Date -Format 'yyyy-MM-dd HH:mm')  Backed up: $commitMsg"
Write-Host $msg -ForegroundColor Green
Add-Content $logFile $msg
