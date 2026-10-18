#!/bin/bash
# Read-only: the last lines the game process wrote to the newest traced Steam
# log (all channels, unfiltered, numbers shortened), the end of the engine's
# log.txt, and what the Windows Steam logged about app 250900 at the end.
# Use it after a run that ended by itself. Copies the result to the clipboard.
# Usage: game-trace-tail.sh [steam-traced log] [lines]
. "$(dirname "$0")/common.sh"
prefix="$WINEPREFIX"   # common.sh: this checkout's prefix (ISAACPORT_PREFIX overrides)
log="${1:-$(ls -t "$root"/analysis/logs/steam-traced-*.log 2>/dev/null | grep -v sample | head -1)}"
lines="${2:-150}"
mkdir -p "$reports"; out="$reports/game-trace-tail.txt"
[[ -f "$log" ]] || { echo "no traced log found" >&2; exit 1; }
{
    echo "# end of the game process in $(basename "$log"), $(date '+%Y-%m-%d %H:%M')"
    python3 -I - "$log" "$lines" <<'PY'
import re, sys
pre = re.compile(r'^(?:[\d.]+:)?([0-9a-f]{4}):([0-9a-f]{4}):(.*)$')
pid = None
with open(sys.argv[1], errors="replace") as fh:
    lines = fh.read().splitlines()
for line in lines:
    m = pre.match(line)
    if m and 'isaac-ng.exe' in line and 'loaddll' in line:
        pid = m.group(1)
print(f"game windows pid: {pid}")
g = [line for line in lines if (m := pre.match(line)) and m.group(1) == pid]
noise = re.compile(r'opengl_drawable_(add_ref|release)|RtlGetExtendedContextLength|RtlInitializeExtendedContext')
tail = [line for line in g if not noise.search(line)][-int(sys.argv[2]):]
for line in tail:
    m = pre.match(line)
    print(f"  {m.group(2)}:{m.group(3)[:220]}")
PY
    echo
    game_log=$(ls "$prefix"/drive_c/users/*/Documents/My\ Games/Binding\ of\ Isaac\ Repentance+/log.txt 2>/dev/null | head -1)
    echo "## last 25 lines of the engine log.txt"
    tail -25 "$game_log" 2>/dev/null | cut -c1-220
    echo
    echo "## Windows Steam logs about the game ending (last matches)"
    steam="$prefix/drive_c/Program Files (x86)/Steam/logs"
    grep -h -i -E '250900|isaac|exit code|Game process' "$steam/console_log.txt" "$steam/content_log.txt" 2>/dev/null | tail -15 | cut -c1-220
} > "$out" 2>&1
cat "$out"
command -v pbcopy >/dev/null && pbcopy < "$out" && echo && echo "Copied to the clipboard."
exit 0
