#!/bin/bash
# Restart Windows Steam in the prefix with Wine tracing in its environment,
# have Steam launch Rep+ (which inherits the tracing), watch it, and write a
# summary filtered to the game process (also copied to the clipboard).
# Needed because SteamStub hands every launch to Steam, so a launcher's own
# environment never reaches the game process (DECISIONS.md E7).
# Per-app Wine DLL overrides for isaac-ng.exe are set explicitly on every run
# (prefix registry only; absent flag = Wine default):
# The Steam overlay is not loaded into Rep+ (it crashes under WoW64 on macOS, DECISIONS.md E8)
# and Apple's OpenGL is used through repplus-patch/glcore/isaacgl.dylib (E15-E24), as in normal play.
#   --with-overlay  load the Steam overlay anyway (to re-test E8)
#   --no-glcore     run without isaacgl.dylib (Wine's own OpenGL handling: the game quits, E10)
#   --no-dinput8    do not load dinput8.dll (DirectInput) into Rep+ (E7)
#   (--no-overlay and --glcore are accepted for compatibility; they are the default now)
#   --no-mods     set EnableMods=0 in the prefix's Rep+ options.ini; --mods sets EnableMods=1;
#                 without either, options.ini is not touched
# A Steam already started by this script (still running, same environment) is reused.
#   --gltrace     also log every OpenGL call the game makes into Wine's opengl32 (WINEDEBUG +opengl):
#                 shows whether Rep+ calls glDrawElements at all. Large log: use ~60 seconds.
#   --keytrace    also log keyboard handling (WINEDEBUG +key,+keyboard,+imm,+dinput,+rawinput): which Mac
#                 keycodes arrive, what Windows key and scan code they become, and who reads them.
# A running Rep+ is closed first (like its close button; you are asked), and the game is closed
# the same way when the watch time is over. Steam keeps running with tracing afterwards (large
# logs, slower): play-repplus.sh / the Desktop app restart it without, or run steam-shutdown.sh.
# Usage: steam-traced-run.sh [--with-overlay] [--no-glcore] [--no-dinput8] [--gltrace] [--keytrace] [--no-mods|--mods] [seconds]
set -uo pipefail
. "$(dirname "$0")/common.sh"
prefix="$WINEPREFIX"   # common.sh: this checkout's prefix (ISAACPORT_PREFIX overrides)
no_overlay=yes; no_dinput8=""; mods=""; glcore=yes; gltrace=""; keytrace=""
while [[ "${1:-}" == --* ]]; do
    case "$1" in
        --no-overlay) no_overlay=yes ;;
        --no-dinput8) no_dinput8=yes ;;
        --no-mods) mods=0 ;;
        --mods) mods=1 ;;
        --glcore) glcore=yes ;;
        --gltrace) gltrace=yes ;;
        --keytrace) keytrace=yes ;;
        --with-overlay) no_overlay="" ;;
        --no-glcore) glcore="" ;;
        *) echo "unknown option $1" >&2; exit 1 ;;
    esac
    shift
done
seconds="${1:-90}"
export WINEPREFIX="$prefix"
stamp=$(date +%Y%m%d-%H%M%S)
mkdir -p "$root/analysis/logs"
marker="$root/analysis/logs/steam-traced.current"
log="$root/analysis/logs/steam-traced-$stamp.log"
samplef="$root/analysis/logs/steam-traced-$stamp.sample.txt"
mkdir -p "$reports"; summary="$reports/steam-traced-summary.txt"
game_log=$(ls "$prefix"/drive_c/users/*/Documents/My\ Games/Binding\ of\ Isaac\ Repentance+/log.txt 2>/dev/null | head -1)

game_pids() { prefix_pids 'isaac-ng\.exe' | tr '\n' ' '; }

appkey='HKCU\Software\Wine\AppDefaults\isaac-ng.exe\DllOverrides'
set_override() {  # name, yes = disable
    if [[ "$2" == yes ]]; then
        WINEDEBUG=-all "$wine" reg add "$appkey" /v "$1" /t REG_SZ /d "" /f >/dev/null
    else
        WINEDEBUG=-all "$wine" reg delete "$appkey" /v "$1" /f >/dev/null 2>&1
    fi
}
override_state() { WINEDEBUG=-all "$wine" reg query "$appkey" /v "$1" >/dev/null 2>&1 && echo disabled || echo default; }

# 1. stop a running Rep+
if game_running; then
    confirm "Rep+ is running. Close it (like its close button) for the traced run?" || { echo "Quit Rep+ first, then run this again." >&2; exit 1; }
    echo "Closing running Rep+ ($(game_pids))..."
    close_game || { echo "Rep+ did not close; quit it yourself, then run this again." >&2; exit 1; }
    sleep 5   # let Steam notice the game has stopped
fi
set_override gameoverlayrenderer "$no_overlay"
set_override dinput8 "$no_dinput8"
gl_env=()
if [[ -n "$glcore" ]]; then
    dylib="$root/repplus-patch/glcore/build/isaacgl.dylib"
    "$root/repplus-patch/glcore/build-isaacgl.sh" || { echo "isaacgl.dylib build FAILED (see the lines above)" >&2; exit 1; }
    gl_env=(DYLD_INSERT_LIBRARIES="$dylib" ISAACGL_DEBUG=1)
fi
steam_env="${gl_env[*]+${gl_env[*]}}"
wine_debug="+pid,+timestamp,+seh,+loaddll,+wgl,err+all,fixme-all"
if [[ -n "$gltrace" ]]; then
    wine_debug="+pid,+timestamp,+seh,+loaddll,+wgl,+opengl,err+all,fixme-all"
    steam_env="$steam_env gltrace"   # a different Steam environment: Steam restarts with it
fi
if [[ -n "$keytrace" ]]; then
    wine_debug="${wine_debug/,err+all/,+key,+keyboard,+imm,+dinput,+rawinput,err+all}"
    steam_env="$steam_env keytrace"
fi
options=$(ls "$prefix"/drive_c/users/*/Documents/My\ Games/Binding\ of\ Isaac\ Repentance+/options.ini 2>/dev/null | head -1)
mods_state="options.ini not found (game default)"
if [[ -n "$options" ]] && grep -q '^EnableMods=' "$options"; then
    [[ -n "$mods" ]] && perl -pi -e "s/^EnableMods=.*/EnableMods=$mods/" "$options"
    mods_state=$(grep '^EnableMods=' "$options")
elif [[ -n "$options" ]]; then
    mods_state="no EnableMods key in options.ini (not changed)"
fi
echo "Mods: $mods_state"

# 2. reuse a traced Steam from an earlier run of this script, else (re)start Steam with tracing
marker_log=$(sed -n 1p "$marker" 2>/dev/null); marker_env=$(sed -n 2p "$marker" 2>/dev/null)
if steam_running && [[ -f "$marker_log" && "$marker_env" == "$steam_env" ]]; then
    log="$marker_log"
    echo "Reusing the traced Steam that is already running."
else
    if steam_running; then
        echo "Shutting Steam down (it is restarted with tracing)..."
        "$root/repplus-patch/launch/steam-shutdown.sh" >/dev/null || { echo "Steam did not exit; quit it from its menu and re-run." >&2; exit 1; }
    fi
    echo "Starting Steam with tracing. A black Steam window is fine."
    env ${gl_env[@]+"${gl_env[@]}"} \
        WINEDEBUG="$wine_debug" "$wine" "$steam" > "$log" 2>&1 &
    printf '%s\n%s\n' "$log" "$steam_env" > "$marker"
    rm -f "$root/analysis/logs/steam-play.current"   # this Steam is not play-repplus.sh's
    read -r -p "When Steam has been up for ~20 seconds (you may need to log in), press Enter... " || true
fi
log_start_line=$(( $(wc -l < "$log") + 1 ))

# 3. ask Steam to launch Rep+ and watch the game process
before=" $(game_pids) "
echo "Asking Steam to launch Rep+..."
WINEDEBUG=-all "$wine" "$steam" -applaunch 250900 >/dev/null 2>&1 &
started=$(date +%s); game_pid=""
for _ in $(seq 1 60); do
    for p in $(game_pids); do [[ "$before" == *" $p "* ]] || { game_pid=$p; break; }; done
    [[ -n "$game_pid" ]] && break; sleep 1
done
outcome="Steam did not start the game within 60s"
if [[ -n "$game_pid" ]]; then
    echo "Rep+ is running (pid $game_pid). Watching for up to ${seconds}s; Ctrl+C stops early."
    interrupted=""; sampled=""; sample_job=""
    trap 'interrupted=yes' INT
    while [[ -z "$interrupted" ]] && kill -0 "$game_pid" 2>/dev/null && (( $(date +%s) - started < seconds )); do
        el=$(( $(date +%s) - started ))
        printf '\r  game running, %3ss elapsed ' "$el"
        if [[ -z "$sampled" && $el -ge 30 ]]; then
            sampled=yes; sample "$game_pid" 3 -file "$samplef" >/dev/null 2>&1 & sample_job=$!
        fi
        sleep 2
    done
    trap - INT; echo
    if kill -0 "$game_pid" 2>/dev/null; then
        outcome="game still running after $(( $(date +%s) - started ))s, cpu $(ps -o %cpu= -p "$game_pid")% (then closed)"
        close_game || outcome="$outcome; it did not close by itself, quit it yourself"
    else
        outcome="game exited by itself after $(( $(date +%s) - started ))s"
    fi
fi
[[ -n "${sample_job:-}" ]] && wait "$sample_job" 2>/dev/null

# 4. summary filtered to the game's Windows pid
{
    echo "# Steam-traced Rep+ run $stamp"
    echo "outcome: $outcome"
    echo "per-app overrides: gameoverlayrenderer $(override_state gameoverlayrenderer), dinput8 $(override_state dinput8)"
    echo "mods: $mods_state"
    echo "OpenGL provider: $(if [[ -n "$glcore" ]]; then echo "Apple GL via isaacgl.dylib (${steam_env})"; else echo "Wine (winemac/CGL)"; fi)"
    echo
    python3 -I "$root/analysis/scripts/steam_trace_report.py" "$log" "$log_start_line"
    echo
    if [[ -s "$samplef" ]]; then
        echo "## macOS sample of the game at ~30s (top of stack)"
        sed -n '/Sort by top of stack/,$p' "$samplef" | head -30
    else
        echo "## macOS sample: none"
    fi
    echo
    if [[ -s "$game_log" ]]; then
        echo "## engine log.txt ($(wc -l < "$game_log") lines, repeats collapsed)"
        uniq -c "$game_log" | head -60
        echo "### last 80 lines of log.txt (repeats collapsed)"
        uniq -c "$game_log" | tail -80
    else
        echo "## engine log.txt: empty or missing"
    fi
} > "$summary"
cat "$summary"
command -v pbcopy >/dev/null && pbcopy < "$summary" && echo && echo "Summary copied to the clipboard."
echo "Steam keeps running (with tracing) in the background and is reused next time. Full log stays local: $log"
