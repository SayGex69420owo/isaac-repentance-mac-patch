#!/bin/bash
# Shut the prefix's Windows Steam down cleanly. If Rep+ is running, it is first
# closed like with its window's close button (it saves and exits), but only after
# you confirm in Terminal or with --close-game; it is never killed.
# Usage: steam-shutdown.sh [--close-game]
. "$(dirname "$0")/common.sh"
if game_running; then
    if [[ "${1:-}" == --close-game ]] || confirm "Rep+ is running. Close the game (like its close button) so Steam can shut down?"; then
        echo "Closing Rep+..."
        close_game || { echo "Rep+ did not close; quit it yourself, then try again." >&2; exit 1; }
        sleep 5   # let Steam sync the saves to the cloud
    else
        echo "Rep+ is running; quit it first, then try again." >&2; exit 1
    fi
fi
if steam_running; then
    echo "Shutting Steam down..."
    WINEDEBUG=-all "$wine" "$steam" -shutdown >/dev/null 2>&1
    for _ in $(seq 1 60); do steam_running || break; sleep 1; done
fi
if steam_running; then
    echo "Steam is still running; try again in a minute (or quit it from its Steam menu)." >&2; exit 1
fi
rm -f "$logs/steam-traced.current" "$logs/steam-play.current"
echo "Steam is closed."
