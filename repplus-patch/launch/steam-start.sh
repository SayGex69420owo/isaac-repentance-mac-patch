#!/bin/bash
# Start the prefix's Windows Steam on its own (no game, no tracing, no OpenGL fix).
# Usage: steam-start.sh
. "$(dirname "$0")/common.sh"
if steam_running; then echo "Steam is already running."; exit 0; fi
rm -f "$logs/steam-play.current" "$logs/steam-traced.current"   # plain Steam, no OpenGL fix
WINEDEBUG=-all "$wine" "$steam" >/dev/null 2>&1 &
echo "Steam is starting (give it ~30 seconds)."
