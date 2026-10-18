#!/bin/bash
# Create a double-clickable "Isaac Repentance+.app" on the Desktop that runs
# play-repplus.sh --gui (Windows Steam with the OpenGL fix, then the game), with
# no Terminal window. The app only contains a short AppleScript that calls this
# checkout's script, so updates to the patch need no new app. If this folder is
# moved or deleted, the app says so instead of failing silently.
# Usage: install-desktop-app.sh [target folder, default ~/Desktop]
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
dest="${1:-$HOME/Desktop}/Isaac Repentance+.app"
script="$root/repplus-patch/launch/play-repplus.sh"
if [[ -e "$dest" ]]; then
    echo "$dest exists; replacing it (it is only the launcher, no game data)."
    rm -rf "$dest"
fi
# AppleScript string literal: escape backslashes and double quotes of the path
as_script=${script//\\/\\\\}
as_script=${as_script//\"/\\\"}
osacompile -o "$dest" \
    -e "set s to \"$as_script\"" \
    -e 'try' \
    -e '    do shell script "test -x " & quoted form of s' \
    -e 'on error' \
    -e '    display alert "Isaac Repentance+" message ("The IsaacPort folder is no longer at" & return & s & return & return & "Run repplus-patch/launch/install-desktop-app.sh from its new place to update this app.") as critical' \
    -e '    return' \
    -e 'end try' \
    -e 'do shell script quoted form of s & " --gui > /dev/null 2>&1 &"'
echo "Created: $dest"
echo "Double-click it to start Steam (with the OpenGL fix) and Rep+. The first start may ask you to log in to Steam."
