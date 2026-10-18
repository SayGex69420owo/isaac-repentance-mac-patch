#!/bin/bash
# Keep the steamwebhelper wrapper in place in the prefix's Windows Steam.
# Steam's bootstrapper verifies its files at every start and restores Valve's
# steamwebhelper.exe, which brings the black UI back (DECISIONS.md E12).
# Valve's admin switch steam.cfg (BootStrapperInhibitAll=enable) stops the
# bootstrapper's update/repair, so the wrapper persists. Steam then does not
# update itself: when Valve requires a newer client, run `off`, let Steam
# update once, then `on` again.
# Only the prefix's Windows Steam is touched, never the macOS Steam app.
# Usage: steam-update-inhibit.sh on [--no-start]|off|status
#   on --no-start  write steam.cfg and install the wrapper, but leave Steam closed (used by setup.sh)
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
. "$(dirname "$0")/../launch/common.sh"
prefix="$WINEPREFIX"   # common.sh: this checkout's prefix (ISAACPORT_PREFIX overrides)
cfg="$prefix/drive_c/Program Files (x86)/Steam/steam.cfg"
launch="$root/repplus-patch/launch"
content=$'BootStrapperInhibitAll=enable\nBootStrapperForceSelfUpdate=disable\n'

ours() { [[ -f "$cfg" && "$(cat "$cfg"; echo x)" == "${content}x" ]]; }

check_after_start() {
    echo "Waiting 45 seconds for Steam to start..."
    sleep 45
    "$here/install-webhelper-wrapper.sh" "$prefix" --status
    if ps -axo command= | grep -i 'cef.win64\\steamwebhelper' | grep -v grep | grep -q -- '--single-process'; then
        echo "steamwebhelper runs with --single-process: wrapper active."
    else
        echo "steamwebhelper is NOT running with --single-process."
    fi
}

case "${1:-}" in
    on)
        "$launch/steam-shutdown.sh" || exit 1
        if [[ -f "$cfg" ]] && ! ours; then
            echo "A different steam.cfg already exists; not touching it:" >&2; cat "$cfg" >&2; exit 1
        fi
        printf '%s' "$content" > "$cfg"
        echo "steam.cfg written (Steam self-update/repair inhibited)."
        "$here/install-webhelper-wrapper.sh" "$prefix" || exit 1
        [[ "${2:-}" == --no-start ]] && exit 0
        "$launch/steam-start.sh"
        check_after_start ;;
    off)
        "$launch/steam-shutdown.sh" || exit 1
        if ours; then rm -f "$cfg"; echo "steam.cfg removed; Steam will update and repair itself on its next start."
        elif [[ -f "$cfg" ]]; then echo "steam.cfg is not ours; left unchanged." >&2
        else echo "no steam.cfg present."; fi ;;
    status)
        if ours; then echo "steam.cfg: ours (update/repair inhibited)"
        elif [[ -f "$cfg" ]]; then echo "steam.cfg: present but not ours"
        else echo "steam.cfg: absent"; fi
        "$here/install-webhelper-wrapper.sh" "$prefix" --status ;;
    *) echo "usage: $0 on|off|status" >&2; exit 1 ;;
esac
