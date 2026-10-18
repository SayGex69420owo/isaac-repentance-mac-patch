# shellcheck shell=bash
# Shared settings and helpers for the launch, setup and Steam scripts (sourced, not run).
#
# The Wine prefix is always this checkout's own (repplus-patch/prefix/repplus). A WINEPREFIX
# you set for other Wine use is ignored on purpose; set ISAACPORT_PREFIX to use another one.
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"   # physical path: same for every caller
export WINEPREFIX="${ISAACPORT_PREFIX:-$root/repplus-patch/prefix/repplus}"
wine_bin="$root/tools/wine/Wine Staging.app/Contents/Resources/wine/bin"
wine="$wine_bin/wine"
wineserver="$wine_bin/wineserver"
steam='C:\Program Files (x86)\Steam\steam.exe'
steam_dir="$WINEPREFIX/drive_c/Program Files (x86)/Steam"
logs="$root/analysis/logs"
# Per-run reports. They contain local user names, paths and Steam account details, so they
# stay in analysis/logs/ (never committed).
reports="$logs/reports"

# PIDs of processes matching a pgrep -f pattern (case-insensitive) that belong to this prefix.
# A Wine process started for another prefix carries a different WINEPREFIX in its environment;
# if the environment cannot be read, the process is counted as ours (as before).
lc() { printf '%s' "$1" | tr '[:upper:]' '[:lower:]'; }   # the Mac's disk ignores case
prefix_lc=$(lc "$WINEPREFIX")
prefix_pids() {
    local pid envs
    for pid in $(pgrep -if "$1" 2>/dev/null); do
        envs=$(lc "$(ps -E -ww -o command= -p "$pid" 2>/dev/null)")
        case "$envs" in
            *" wineprefix=$prefix_lc "*|*" wineprefix=$prefix_lc") echo "$pid" ;;
            *" wineprefix="*) ;;
            *) echo "$pid" ;;
        esac
    done
}
steam_running() { [[ -n "$(prefix_pids 'Steam\\steam\.exe')" ]]; }
game_running() { [[ -n "$(prefix_pids 'isaac-ng\.exe')" ]]; }
macos_steam_running() { pgrep -x steam_osx >/dev/null; }   # not its ipcserver helper, which stays after Quit

# Whether the running Steam is play-repplus.sh's: isaacgl.dylib in its environment and no
# tracing (a traced Steam from steam-traced-run.sh is slow and writes huge logs). The
# environment is read from the process; when it cannot be read, the marker file decides.
steam_has_fix() {
    local pid envs seen=""
    for pid in $(prefix_pids 'Steam\\steam\.exe'); do   # also short-lived '-applaunch' requests: look at all
        envs=$(ps -E -ww -o command= -p "$pid" 2>/dev/null)
        case "$envs" in
            *DYLD_INSERT_LIBRARIES=*isaacgl.dylib*)
                case "$envs" in *" WINEDEBUG=+"*|*" WINEDEBUG=*,+"*) return 1 ;; *) return 0 ;; esac ;;
            *" WINEPREFIX="*) seen=yes ;;
        esac
    done
    [[ -z "$seen" && -f "$logs/steam-play.current" ]]
}

# Close Rep+ the way its window's close button does (it saves and exits), and wait for it.
# Returns 1 if it is still running after 30 seconds. It is never killed.
close_game() {
    game_running || return 0
    WINEDEBUG=-all "$wine" taskkill /im isaac-ng.exe >/dev/null 2>&1
    for _ in $(seq 1 30); do game_running || return 0; sleep 1; done
    return 1
}

# Ask a yes/no question; without a terminal (e.g. from the Desktop app) the answer is no.
confirm() {
    local answer=""
    [[ -t 0 ]] || return 1
    read -r -p "$1 [y/N] " answer || return 1
    [[ "$answer" =~ ^[Yy] ]]
}

# Keep only the newest N files matching a glob in analysis/logs (default 10).
prune_logs() {
    local keep="${2:-10}" f n=0
    while IFS= read -r f; do
        n=$((n + 1))
        (( n > keep )) && rm -f "$logs/$f"
    done < <(cd "$logs" 2>/dev/null && ls -t $1 2>/dev/null)
}
