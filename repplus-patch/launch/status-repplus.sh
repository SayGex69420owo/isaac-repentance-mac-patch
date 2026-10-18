#!/bin/bash
# Show whether Rep+ is running in the prefix and what its engine log says,
# and copy that to the clipboard. Read-only; changes nothing.
# Usage: status-repplus.sh
. "$(dirname "$0")/common.sh"
prefix="$WINEPREFIX"   # common.sh: this checkout's prefix (ISAACPORT_PREFIX overrides)
game_log=$(ls "$prefix"/drive_c/users/*/Documents/My\ Games/Binding\ of\ Isaac\ Repentance+/log.txt 2>/dev/null | head -1)
mkdir -p "$reports"; out="$reports/status-repplus.txt"
{
    echo "# Rep+ status, $(date '+%Y-%m-%d %H:%M:%S')"
    pids=$(prefix_pids 'isaac-ng\.exe' | tr '\n' ' ')
    echo "Rep+ processes running: ${pids:-none}"
    for p in $pids; do
        echo "  pid $p: started $(ps -o lstart= -p "$p"), cpu $(ps -o %cpu= -p "$p")%, dinput8 mapped: $(lsof -p "$p" 2>/dev/null | grep -ic dinput8)"
    done
    echo "Steam running: $(steam_running && { steam_has_fix && echo "yes, with the OpenGL fix" || echo "yes, without the OpenGL fix (or traced)"; } || echo no)"
    echo
    if [[ -s "$game_log" ]]; then
        echo "## engine log.txt, modified $(stat -f '%Sm' "$game_log"), $(wc -l < "$game_log") lines (repeats collapsed)"
        uniq -c "$game_log" | head -100
    else
        echo "## engine log.txt: ${game_log:-not found} (empty or missing)"
    fi
} > "$out"
cat "$out"
command -v pbcopy >/dev/null && pbcopy < "$out" && echo && echo "Copied to the clipboard."
