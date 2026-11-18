#!/bin/sh
# Fetch the Amiga ARM Runtime (BlitterStudio/aar) at the commit pinned in
# sdk/AAR_REF.
#
# Usage: sdk/scripts/fetch-aar.sh [DEST]
#
# DEST defaults to <firmware root>/sdk/aar, which is gitignored. The repository
# comes from $AAR_REPO (default https://github.com/BlitterStudio/aar.git); a
# local path works too.
#
#   DEST already at AAR_REF         prints "aar <short> ok", changes nothing.
#   DEST at another commit          refuses if it has local changes, otherwise
#                                   fetches and checks out AAR_REF detached.
#   DEST missing (or empty)         clones and checks out AAR_REF.
#
# Exit status is non-zero on any failure.
#
# SPDX-License-Identifier: GPL-3.0-or-later

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
REF_FILE="$ROOT/sdk/AAR_REF"
REPO=${AAR_REPO:-https://github.com/BlitterStudio/aar.git}
DEST=${1:-$ROOT/sdk/aar}

die() {
  echo "fetch-aar: $*" >&2
  exit 1
}

[ -f "$REF_FILE" ] || die "missing $REF_FILE"
REF=$(tr -d ' \t\r\n' < "$REF_FILE")
case "$REF" in
  *[!0-9a-f]*) die "$REF_FILE must hold one 40-digit lowercase hex commit" ;;
esac
[ "${#REF}" -eq 40 ] || die "$REF_FILE must hold one 40-digit lowercase hex commit"
SHORT=$(printf '%s' "$REF" | cut -c1-12)

command -v git >/dev/null 2>&1 || die "git not found"

# The aar checkout is nested in the firmware tree, so ask for the .git entry
# itself instead of letting git walk up into the firmware repository.
if [ -e "$DEST/.git" ]; then
  HEAD_NOW=$(git -C "$DEST" rev-parse HEAD) || die "$DEST is not a usable git checkout"
  if [ "$HEAD_NOW" = "$REF" ]; then
    echo "aar $SHORT ok"
    exit 0
  fi
  if [ -n "$(git -C "$DEST" status --porcelain --untracked-files=no)" ]; then
    die "$DEST is at $(printf '%s' "$HEAD_NOW" | cut -c1-12), not $SHORT, and has local changes; commit or discard them, or set AAR_DIR to use this checkout as it is"
  fi
  if ! git -C "$DEST" cat-file -e "$REF^{commit}" 2>/dev/null; then
    git -C "$DEST" fetch --quiet --tags "$REPO" '+refs/heads/*:refs/remotes/origin/*' ||
      die "cannot fetch $REPO"
  fi
  if ! git -C "$DEST" cat-file -e "$REF^{commit}" 2>/dev/null; then
    git -C "$DEST" fetch --quiet "$REPO" "$REF" || die "commit $REF is not in $REPO"
  fi
  git -C "$DEST" checkout --quiet --detach "$REF" || die "cannot check out $REF in $DEST"
else
  CREATED=0
  [ -e "$DEST" ] || CREATED=1
  if [ -e "$DEST" ] && [ -n "$(ls -A "$DEST" 2>/dev/null)" ]; then
    die "$DEST exists and is not an aar git checkout"
  fi
  # core.autocrlf=false keeps the shell scripts in the checkout LF on Windows.
  git clone --quiet --no-checkout --config core.autocrlf=false "$REPO" "$DEST" ||
    die "cannot clone $REPO"
  if ! git -C "$DEST" checkout --quiet --detach "$REF"; then
    [ "$CREATED" -eq 1 ] && rm -rf "$DEST"
    die "commit $REF is not in $REPO"
  fi
fi

[ -f "$DEST/include/aar/app.h" ] || die "$DEST has no include/aar/app.h at $SHORT"
echo "aar $SHORT ok"
