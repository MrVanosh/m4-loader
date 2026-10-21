#!/bin/bash
# Add a loader rev to the repo as one commit and re-run the checks.
#
#   m4/tools/add-rev.sh REPO TREE NAME "ISO8601 DATE" MESSAGE_FILE
#   e.g. m4/tools/add-rev.sh ~/src/loader /work/n1/loader/rev14 rev14 "2026-10-22T04:10:00+00:00" msg-rev14.txt
#
# REPO    a clean checkout of this repo on the branch to extend (main). m4/ and .git are kept; everything else is replaced.
# TREE    the integrator's loader tree for the rev (it may still contain build/, __pycache__, payload_sizes.h: skipped).
# Needs git, python3, and for the checks INPUTS, PRIVATE, WORK as in m4/tools/verify-repo.sh plus EXPECTED_SHA256 and
# VERSION of the new build. Authorship: Jonathan Goyvaerts (the repo's git config); no other author or co-author line.
set -eu
REPO=$1; TREE=$2; NAME=$3; DATE=$4; MSG=$5
cd "$REPO"
[ -z "$(git status --porcelain)" ] || { echo "REPO is not clean" >&2; exit 1; }
find . -mindepth 1 -maxdepth 1 ! -name .git ! -name m4 -exec rm -rf {} +
tar -C "$TREE" --exclude=./build --exclude=__pycache__ --exclude='*.pyc' --exclude=./src/standalone/payload_sizes.h \
    --exclude=./m4 --exclude=./.git -cf - . | tar -xf -
PRIV=$(mktemp -d); python3 -I m4/tools/privatize-tree.py . "$PRIV"      # moves per-unit/per-install text out; keep $PRIV (never commit)
python3 -I m4/tools/audit-tree.py . ${ADT:+--adt "$ADT"} ${ROOT_UUID:+--root-uuid "$ROOT_UUID"} --install "$PRIV/level1-install.h"
git add -A
GIT_AUTHOR_DATE="$DATE" GIT_COMMITTER_DATE="$DATE" git commit -q -F "$MSG"
git tag "$NAME"
echo "committed $NAME $(git rev-parse --short HEAD); private files of this rev in $PRIV"
echo "compare them with the kit's private/ files: cmp $PRIV/gates-unit.h \$PRIVATE/private-gates-unit.h (a changed gate record means the loader refuses that Mac)"
if [ -n "${EXPECTED_SHA256:-}" ]; then
  git bundle create "${WORK:?}/loader-$NAME.bundle" --all
  PRIVATE=${PRIVATE:-$PRIV} m4/tools/verify-repo.sh "$WORK/loader-$NAME.bundle" "$NAME" "${VERSION:?}" "$EXPECTED_SHA256"
  [ -x m4/check.sh ] && echo "now run: m4/check.sh (see m4/README.md)"
fi
