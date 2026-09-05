#!/usr/bin/env bash
# split-snapshot.sh <phase-number>
#
# Snapshot the current working tree (core/, gui/, CMakeLists.txt don't change for
# pure doc phases) into /tmp/split/phase-NN/ BEFORE starting a phase, so any single
# phase can be reverted independently. Phase 3+ snapshots are on-disk before the
# phase's changes are made.
#
# Usage:
#   ./scripts/split-snapshot.sh 7
set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "usage: $0 <phase-number>" >&2
    exit 2
fi

PHASE=$(printf "phase-%02d" "$1")
DEST="/tmp/split/$PHASE"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

mkdir -p "$DEST"
cp -r "$ROOT/core" "$DEST/core"
cp -r "$ROOT/gui" "$DEST/gui"
cp "$ROOT/CMakeLists.txt" "$DEST/CMakeLists.txt"

echo "snapshot: current tree -> $DEST"
