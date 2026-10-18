#!/bin/bash
# Bring existing Rep+ progress from Steam Cloud into the prefix's Windows Steam
# (DECISIONS.md D4). If the prefix Steam already holds a local save (for example from
# a run with Cloud off, in userdata/<id>/250900), switching Cloud on could make Steam
# offer or even upload that save over the one in the cloud.
#   status   read-only: list the prefix's Rep+ cloud files (size and date), to
#            compare with the save files on another computer where you play
#   prepare  with Steam closed, MOVE (not delete) userdata/<id>/250900 aside to
#            250900.test-<date>, after the user types MOVE. Steam then has no
#            local copy and downloads the cloud copy once Cloud is switched on.
#   undo     move the newest 250900.test-<date> back (only if 250900 is absent)
# Never touches the macOS Steam app or the game's Documents folder.
# Usage: cloud-restore.sh status|prepare|undo
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
. "$(dirname "$0")/../launch/common.sh"
prefix="$WINEPREFIX"   # common.sh: this checkout's prefix (ISAACPORT_PREFIX overrides)
steam="$prefix/drive_c/Program Files (x86)/Steam"
launch="$root/repplus-patch/launch"

list_app() {  # userdata/<id>/250900 directory
    find "$1" -type f -exec stat -f '    %Sm  %8z  %N' -t '%Y-%m-%d %H:%M' {} + 2>/dev/null | sed "s|$1/||" | sort
}

status() {
    local found=""
    for d in "$steam"/userdata/*/250900 "$steam"/userdata/*/250900.test-*; do
        [[ -d "$d" ]] || continue
        found=yes
        echo "  ${d#"$steam"/}:"
        list_app "$d"
    done
    [[ -n "$found" ]] || echo "  no Rep+ cloud folder in the prefix Steam (nothing downloaded yet)"
    echo "  Compare rep+persistentgamedata1.dat with the same file on another computer where you play"
    echo "  (Steam\\userdata\\<id>\\250900\\remote): similar size and a recent date mean your progress is here."
}

case "${1:-}" in
    status) status ;;
    prepare)
        "$launch/steam-shutdown.sh" || { echo "Steam must be closed first." >&2; exit 1; }
        dirs=()
        for d in "$steam"/userdata/*/250900; do [[ -d "$d" ]] && dirs+=("$d"); done
        if (( ${#dirs[@]} == 0 )); then echo "Nothing to move: no local Rep+ cloud folder."; exit 0; fi
        echo "These local TEST files will be moved aside (not deleted):"
        for d in "${dirs[@]}"; do echo "  ${d#"$steam"/}:"; list_app "$d"; done
        read -r -p "Type MOVE to move them aside: " answer
        [[ "$answer" == MOVE ]] || { echo "Nothing changed."; exit 1; }
        stamp=$(date +%Y%m%d-%H%M%S)
        for d in "${dirs[@]}"; do
            mv "$d" "$d.test-$stamp" && echo "moved: ${d#"$steam"/} -> 250900.test-$stamp"
        done
        echo
        echo "Done. Next: start Steam, switch Steam Cloud on for Rep+ in the Steam window, then start the game."
        ;;
    undo)
        "$launch/steam-shutdown.sh" || exit 1
        for u in "$steam"/userdata/*/; do
            last=$(ls -dt "$u"250900.test-* 2>/dev/null | head -1)
            [[ -n "$last" ]] || continue
            if [[ -e "$u/250900" ]]; then echo "${u#"$steam"/}250900 exists; not overwriting it." >&2; continue; fi
            mv "$last" "$u/250900" && echo "restored ${last#"$steam"/} -> 250900"
        done ;;
    *) echo "usage: $0 status|prepare|undo" >&2; exit 1 ;;
esac
