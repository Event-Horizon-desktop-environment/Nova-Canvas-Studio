#!/usr/bin/env bash
# check_qtdep.sh — enforce the headless Qt-free invariant (splitplan Phase 21)
#
#   A headless module may include ONLY <system>, <canvas/core/...>, and other
#   headless modules. Never <Q...>. If a module needs Qt, it is NOT headless.
#
# Static scan: greps every headless source for a Qt include. This is the
# belt-and-braces half of the guarantee; the other half is compile-time — the
# canvas_add_headless_test() CMake targets compile the modules as projected, and a
# stray <Q...> include fails that link because no Qt is linked there.
#
# Usage:
#   ./scripts/check_qtdep.sh            # scan and report (exit 1 on any hit)
#   ./scripts/check_qtdep.sh -q         # quiet: exit code only (for build.sh)
#   ./scripts/check_qtdep.sh -h         # this help

set -euo pipefail

QUIET=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        -q|--quiet) QUIET=1; shift ;;
        -h|--help)  sed -n '2,16p' "$0"; exit 0 ;;
        *) echo "unknown arg: $1" >&2; exit 2 ;;
    esac
done

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# The headless surface: core/ (Qt-free by policy) + the extracted GUI headless
# modules + the GUI headless tests. gui/Widgets, gui/UX, gui/features (outside
# playback) and gui/src/core (Qt display helpers, e.g. timecode) are
# intentionally NOT here — they are Qt UI code.
HEADLESS=(
    "core/include"
    "core/src"
    "gui/src/features/playback/sync_constants.hpp"
    "gui/src/features/playback/audio_sink.hpp"
    "gui/src/features/playback/audio_pipeline.hpp"
    "gui/src/features/playback/audio_pipeline.cpp"
    "gui/src/features/playback/sonicsync.hpp"
    "gui/src/features/playback/sonicsync.cpp"
    "gui/src/features/playback/timeline_decoder.hpp"
    "gui/src/features/playback/timeline_decoder.cpp"
    "gui/src/Widgets/timeline_snap.hpp"
    "gui/src/Widgets/timeline_snap.cpp"
    "gui/src/Widgets/timeline_selection.hpp"
    "gui/src/Widgets/timeline_selection.cpp"
    "gui/src/Widgets/timeline_drag.hpp"
    "gui/src/Widgets/timeline_drag.cpp"
    "gui/src/Widgets/transition_handle_editor.hpp"
    "gui/src/Widgets/transition_handle_editor.cpp"
    "gui/tests"
)

OFFENDING=0
for path in "${HEADLESS[@]}"; do
    target="$ROOT/$path"
    [[ -e "$target" ]] || continue
    # <Qt...> and <Q...> includes (anchor to the opening <Q so e.g. <queue>,
    # <chrono>, <qlabel> std headers are not false-flagged).
    hits=$(grep -rnE '#[[:space:]]*include[[:space:]]*<Q' "$target" || true)
    if [[ -n "$hits" ]]; then
        OFFENDING=1
        if [[ "$QUIET" -eq 0 ]]; then
            printf '%s\n' "$hits"
        fi
    fi
done

if [[ "$OFFENDING" -eq 1 ]]; then
    [[ "$QUIET" -eq 1 ]] || printf 'check_qtdep: FAIL — headless modules include Qt headers\n' >&2
    exit 1
fi
if [[ "$QUIET" -eq 0 ]]; then
    printf 'check_qtdep: OK — no Qt includes in headless modules\n'
fi
exit 0