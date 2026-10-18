#!/bin/bash
# Play Rep+ on this Mac: apply the patch's per-app Wine settings, make sure the
# Windows Steam client is running in the prefix with the isaacgl.dylib OpenGL
# fix loaded (Steam provides ownership; you log in yourself), and ask Steam to
# start the game. No Wine tracing; isaacgl's per-call statistics are off.
# A Steam that runs without the fix (started by steam-start.sh or by a traced
# run) is shut down and restarted with it. A Steam started here gets
# -applaunch on its command line, so it starts the game by itself once you are
# logged in; no waiting or Enter needed.
#   --steam-only   start (or restart) Steam with the fix, but not the game
#   --gui          report problems in a macOS dialog (used by the Desktop app)
# Environment: ISAACPORT_STEAM_ARGS  extra options for a Steam started here (experiments)
#              ISAACGL_*             options of isaacgl.dylib (see the top of glcore/isaacgl.c)
# Usage: play-repplus.sh [--steam-only] [--gui]
set -uo pipefail
. "$(dirname "$0")/common.sh"
glcore="$root/repplus-patch/glcore"
dylib="$glcore/build/isaacgl.dylib"
marker="$logs/steam-play.current"
steam_only=""; gui=""
for a in "$@"; do
    case "$a" in
        --steam-only) steam_only=yes ;;
        --gui) gui=yes ;;
        *) echo "unknown option $a" >&2; exit 1 ;;
    esac
done
mkdir -p "$logs"

fail() {
    echo "$1" >&2
    # the message is passed as an argument, so quotes in it need no escaping
    [[ -n "$gui" ]] && osascript -e 'on run argv' -e 'display alert "Isaac Repentance+" message (item 1 of argv) as critical' \
        -e 'end run' "$1" >/dev/null 2>&1
    exit 1
}

[[ -x "$wine" ]] || fail "Wine is not installed in $root/tools. Run ./setup.sh in that folder first."
[[ -f "$steam_dir/steam.exe" ]] || fail "The Windows Steam is not installed in the Wine prefix. Run ./setup.sh first."
macos_steam_running && fail "The macOS Steam app is running. Quit it (Steam > Quit Steam in the menu bar), then start again."

# One launch at a time: a second double-click while the first is still starting does nothing.
lock="$logs/launch.lock"
if ! mkdir "$lock" 2>/dev/null; then
    if [[ -n "$(find "$lock" -maxdepth 0 -mmin -5 2>/dev/null)" ]]; then
        echo "Already starting (another launch is in progress)."; exit 0
    fi
    rm -rf "$lock"; mkdir "$lock" 2>/dev/null || true   # left over from an interrupted launch
fi
trap 'rmdir "$lock" 2>/dev/null' EXIT

if game_running; then echo "Rep+ is already running."; exit 0; fi

"$root/repplus-patch/setup/repplus-overrides.sh" ensure >/dev/null \
    || echo "Warning: could not confirm the Wine settings (repplus-overrides.sh status shows them)." >&2

# Rebuild isaacgl.dylib only when its source changed; keep a working old build if the rebuild fails.
if [[ ! -f "$dylib" || "$glcore/isaacgl.c" -nt "$dylib" || "$glcore/build-isaacgl.sh" -nt "$dylib" ]]; then
    echo "Building the OpenGL fix (isaacgl.dylib)..."
    if ! "$glcore/build-isaacgl.sh" >/dev/null 2>&1; then
        [[ -f "$dylib" ]] || fail "Building the OpenGL fix failed. Run repplus-patch/glcore/build-isaacgl.sh in Terminal to see why."
        echo "Warning: rebuilding isaacgl.dylib failed; using the previous build." >&2
    fi
fi

if steam_running && ! steam_has_fix; then
    echo "Steam is running without the OpenGL fix; restarting it."
    "$root/repplus-patch/launch/steam-shutdown.sh" >/dev/null || fail "Steam did not shut down; quit it from its Steam menu and try again."
fi
if ! steam_running; then
    # A Steam update replaces the Steam window fix; put it back (quick, Steam is not running).
    if "$root/repplus-patch/steam-ui/install-webhelper-wrapper.sh" "$WINEPREFIX" --status 2>/dev/null | grep -q "Valve's original"; then
        echo "Re-installing the Steam window fix (Steam replaced it)."
        "$root/repplus-patch/steam-ui/install-webhelper-wrapper.sh" "$WINEPREFIX" >/dev/null 2>&1 \
            || echo "Warning: re-installing the Steam window fix failed; the Steam window may stay black." >&2
    fi
    launch=(-applaunch 250900)
    [[ -n "$steam_only" ]] && launch=()
    # extra Steam start options for experiments, e.g. ISAACPORT_STEAM_ARGS="-silent"
    [[ -n "${ISAACPORT_STEAM_ARGS:-}" ]] && read -r -a extra <<< "$ISAACPORT_STEAM_ARGS" && launch=(${extra[@]+"${extra[@]}"} ${launch[@]+"${launch[@]}"})
    prune_logs 'steam-play-*.log' 10
    log="$logs/steam-play-$(date +%Y%m%d-%H%M%S).log"
    echo "Starting Steam$([[ -z "$steam_only" ]] && echo ", which then starts Rep+"). Log in with your own account if it asks."
    env DYLD_INSERT_LIBRARIES="$dylib" ISAACGL_STATS="${ISAACGL_STATS:-0}" WINEDEBUG=-all "$wine" "$steam" ${launch[@]+"${launch[@]}"} > "$log" 2>&1 &
    echo "$log" > "$marker"
    exit 0
fi
[[ -n "$steam_only" ]] && { echo "Steam is already running with the OpenGL fix."; exit 0; }
echo "Starting The Binding of Isaac: Repentance+ through Steam..."
# Only a request to the running Steam (which has the fix in its environment); the game starts from Steam.
WINEDEBUG=-all "$wine" "$steam" -applaunch 250900 >/dev/null 2>&1 &
