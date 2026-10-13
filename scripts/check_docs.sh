#!/bin/sh
# ============================================================================
# imslug - scripts/check_docs.sh
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
# ============================================================================
set -u

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd) || exit 1
cd "$SCRIPT_DIR/.." || exit 1

fail() {
    echo "[check_docs] FAIL: $1" >&2
    exit 1
}

command -v git >/dev/null 2>&1 || fail "git is not available"
git rev-parse --is-inside-work-tree >/dev/null 2>&1 || fail "not inside a git repository (run from the repo root)"

# --- 1. nothing under .agent-docs/ may be tracked ---------------------------
tracked=$(git ls-files .agent-docs) || fail "git ls-files .agent-docs failed"
if [ -n "$tracked" ]; then
    echo "[check_docs] tracked files under .agent-docs/ (must never be committed):" >&2
    echo "$tracked" | sed 's/^/  /' >&2
    fail "remove them from the index with 'git rm --cached <file>' (never git add -f)"
fi

# --- 2. .gitignore must keep the .agent-docs/ entry -------------------------
[ -f .gitignore ] || fail ".gitignore is missing"
grep -Eq '^[[:space:]]*/?\.agent-docs/?[[:space:]]*$' .gitignore || fail ".gitignore lost its '.agent-docs/' entry (PLAN §12.5)"

# --- 3. markdown outside the durable allowlist is a violation ---------------
files=$(git ls-files -co --exclude-standard) || fail "git ls-files -co --exclude-standard failed"
count=$(printf '%s\n' "$files" | grep -c .)
md_count=$(printf '%s\n' "$files" | grep -Ec '\.md$')
bad=$(printf '%s\n' "$files" \
    | grep -E '\.md$' \
    | grep -Ev '^(README\.md|AGENTS\.md)$' \
    | grep -Ev '^(docs|\.opencode|\.github)/' \
    | grep -Ev '^(third_party|vendor)/')
if [ -n "$bad" ]; then
    echo "[check_docs] stray markdown outside the durable allowlist:" >&2
    echo "$bad" | sed 's/^/  /' >&2
    fail "move transient docs into .agent-docs/ (gitignored) or delete them; durable docs are README.md, AGENTS.md, docs/**, .opencode/**, .github/**"
fi

echo "[check_docs] OK: $count files scanned, $md_count markdown within the allowlist; .agent-docs/ untracked and ignored."
exit 0
