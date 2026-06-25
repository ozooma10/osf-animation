#!/usr/bin/env bash
# Register the .pex clean filter for this clone.
#
# Git clean/smudge filter *commands* live in .git/config, which is not
# shared via the repo. So each clone must run this once to make the
# filter referenced by .gitattributes (filter=pexnorm) actually work.
set -euo pipefail

repo_root="$(git rev-parse --show-toplevel)"
script="$repo_root/tools/git/pex-normalize.pl"

git config filter.pexnorm.clean "perl '$script'"
# No smudge: we don't want to rewrite working-tree files on checkout.
git config --unset filter.pexnorm.smudge 2>/dev/null || true

echo "Registered filter.pexnorm.clean -> perl $script"
echo "Re-normalizing already-tracked .pex blobs..."
# Touch the .pex so git re-runs the filter and drops timestamp-only diffs.
git add --renormalize dist/Scripts/*.pex 2>/dev/null || true
echo "Done. 'git status' will now ignore timestamp-only .pex changes."
