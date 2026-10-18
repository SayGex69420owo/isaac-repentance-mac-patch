#!/bin/bash
# Let the prefix's Windows Steam update itself once, then put the Steam window fix back.
# Needed only when Steam says it must be updated (the fix stops Steam's self-update,
# see steam-update-inhibit.sh). Your login, games and saves are kept.
# Usage: update-steam.sh
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
. "$here/../launch/common.sh"
"$here/steam-update-inhibit.sh" off || exit 1
echo "Starting Steam so it can update itself. Its window may stay black meanwhile; that is expected."
rm -f "$logs/steam-play.current" "$logs/steam-traced.current"
WINEDEBUG=-all "$wine" "$steam" >/dev/null 2>&1 &
# Steam starts its web helper (the window) only after updating; give it up to 15 minutes
sleep 20
done_update=""
for i in $(seq 1 90); do
    [[ -n "$(prefix_pids 'steamwebhelper')" ]] && { done_update=yes; break; }
    (( i % 6 == 0 )) && echo "  still updating ($(( i * 10 / 60 )) min): $(tail -1 "$steam_dir/logs/bootstrap_log.txt" 2>/dev/null | cut -c23-100)"
    sleep 10
done
if [[ -z "$done_update" ]]; then
    echo "Steam has not finished after 15 minutes; it is left running. When its window shows up," >&2
    echo "run: repplus-patch/steam-ui/steam-update-inhibit.sh on" >&2
    exit 1
fi
sleep 30   # let it finish writing its files
echo "Closing Steam and putting the Steam window fix back..."
"$here/steam-update-inhibit.sh" on --no-start || exit 1
echo "Done. Start the game as usual (Desktop app or play-repplus.sh)."
