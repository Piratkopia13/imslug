# imslug - scripts/check_docs.ps1
#
# Docs-hygiene check (PLAN §12.5). Fails when:
#   1. anything under .agent-docs/ is tracked by git;
#   2. .gitignore lost its '.agent-docs/' entry;
#   3. markdown outside the durable allowlist exists (tracked OR
#      untracked-not-ignored).
#
# Allowlist: README.md, AGENTS.md, docs/**, .opencode/**, .github/**.
# Vendored trees (third_party/**, vendor/**) are excluded from check 3.
#
# Never "fix" a violation with `git add -f`: transient plan/session/scratch
# markdown belongs in the gitignored .agent-docs/ directory only.

$ErrorActionPreference = 'Continue'
Set-StrictMode -Version Latest

$root = Split-Path -Parent $PSScriptRoot
Push-Location $root

function Fail([string]$Message) {
    Write-Host "[check_docs] FAIL: $Message" -ForegroundColor Red
    exit 1
}

# --- 0. git must be available and this must be a repository -----------------
$gitOk = $false
try {
    & git rev-parse --is-inside-work-tree *> $null
    $gitOk = ($LASTEXITCODE -eq 0)
}
catch {
    $gitOk = $false
}
if (-not $gitOk) {
    Fail "not inside a git repository (run from the repo root)"
}

# --- 1. nothing under .agent-docs/ may be tracked ---------------------------
$tracked = @(& git ls-files .agent-docs 2>$null)
if ($LASTEXITCODE -ne 0) {
    Fail "git ls-files .agent-docs failed"
}
if ($tracked.Count -gt 0) {
    Write-Host "[check_docs] tracked files under .agent-docs/ (must never be committed):" -ForegroundColor Red
    $tracked | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
    Fail "remove them from the index with 'git rm --cached <file>' (never git add -f)"
}

# --- 2. .gitignore must keep the .agent-docs/ entry -------------------------
if (-not (Test-Path -LiteralPath .gitignore)) {
    Fail ".gitignore is missing"
}
$hasIgnoreEntry = $false
foreach ($line in (Get-Content -LiteralPath .gitignore)) {
    if ($line -match '^\s*/?\.agent-docs/?\s*$') {
        $hasIgnoreEntry = $true
        break
    }
}
if (-not $hasIgnoreEntry) {
    Fail ".gitignore lost its '.agent-docs/' entry (PLAN §12.5)"
}

# --- 3. markdown outside the durable allowlist is a violation ---------------
$files = @(& git ls-files -co --exclude-standard 2>$null)
if ($LASTEXITCODE -ne 0) {
    Fail "git ls-files -co --exclude-standard failed"
}

$allowedRoot = '^(README\.md|AGENTS\.md)$'
$allowedDirs = '^(docs|\.opencode|\.github)/'
$vendored = '^(third_party|vendor)/'

$violations = @()
foreach ($file in $files) {
    $normalized = $file -replace '\\', '/'
    if ($normalized -notmatch '\.md$') { continue }
    if ($normalized -match $vendored) { continue }
    if ($normalized -match $allowedRoot) { continue }
    if ($normalized -match $allowedDirs) { continue }
    $violations += $normalized
}

if ($violations.Count -gt 0) {
    Write-Host "[check_docs] stray markdown outside the durable allowlist:" -ForegroundColor Red
    $violations | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
    Fail "move transient docs into .agent-docs/ (gitignored) or delete them; durable docs are README.md, AGENTS.md, docs/**, .opencode/**, .github/**"
}

$mdCount = @($files | Where-Object { $_ -match '\.md$' }).Count
Write-Host "[check_docs] OK: $($files.Count) files scanned, $mdCount markdown within the allowlist; .agent-docs/ untracked and ignored."
Pop-Location
exit 0
