#!/bin/bash
# Read-only: show the OpenGL-fix messages and the last lines of the newest play
# session (play-repplus.sh / the Desktop app), plus the end of the engine log.
# Copies the result to the clipboard.
# Usage: play-log.sh
. "$(dirname "$0")/common.sh"
prefix="$WINEPREFIX"   # common.sh: this checkout's prefix (ISAACPORT_PREFIX overrides)
log=$(ls -t "$root"/analysis/logs/steam-play-*.log 2>/dev/null | head -1)
mkdir -p "$reports"; out="$reports/play-log.txt"
{
    echo "# newest play session: ${log:-none}"
    [[ -n "$log" ]] && { echo "## isaacgl messages (last 60)"; grep '^\[isaacgl\]' "$log" | grep -v 'translated to GLSL' | tail -60 | cut -c1-220; }
    game_log=$(ls "$prefix"/drive_c/users/*/Documents/My\ Games/Binding\ of\ Isaac\ Repentance+/log.txt 2>/dev/null | head -1)
    echo; echo "## last 30 lines of the engine log.txt"
    tail -30 "$game_log" 2>/dev/null | cut -c1-220
} > "$out" 2>&1
cat "$out"
command -v pbcopy >/dev/null && pbcopy < "$out" && echo && echo "Copied to the clipboard."
exit 0
