#!/bin/bash
# Read-only: show what Rep+ save/cloud data exists in the prefix's Windows Steam
# and in the game's Documents folder, and whether Steam Cloud is on for app
# 250900 there. Changes nothing. Output is copied to the clipboard.
# Usage: save-status.sh   (BACKUP_DIR=<folder> also lists another folder of save files)
. "$(dirname "$0")/common.sh"
prefix="$WINEPREFIX"   # common.sh: this checkout's prefix (ISAACPORT_PREFIX overrides)
steam="$prefix/drive_c/Program Files (x86)/Steam"
mkdir -p "$reports"; out="$reports/save-status.txt"
list() { [[ -d "$1" ]] && find "$1" -maxdepth 2 -type f -exec stat -f '  %Sm  %10z  %N' -t '%Y-%m-%d %H:%M' {} + 2>/dev/null | sed "s|$prefix/||" | sort || echo "  (none)"; }
{
    echo "# Rep+ save status in the prefix, $(date '+%Y-%m-%d %H:%M') (read-only)"
    echo
    for u in "$steam"/userdata/*/; do
        [[ -d "$u" ]] || continue
        id=$(basename "$u")
        # the folder name is the Steam account ID: shortened, so a pasted report does not reveal it
        echo "## Steam user ${id:0:2}…, app 250900 cloud files (userdata/<id>/250900)"
        list "$u/250900"
        echo "  cloud setting for 250900 in localconfig.vdf:"
        awk '/"250900"/{f=1} f&&/cloud|Cloud/{print "    "$0} f&&/^\t*}/{if(++n>2)f=0}' "$u/config/localconfig.vdf" 2>/dev/null | head -10
        echo "  global cloud setting (sharedconfig/localconfig 'CloudEnabled'):"
        grep -h -i '"CloudEnabled"' "$u"/config/*.vdf "$u"/7/remote/sharedconfig.vdf 2>/dev/null | sed 's/^/    /' | head -5
        echo
    done
    echo "## Documents/My Games/Binding of Isaac Repentance+ (local game data)"
    for d in "$prefix"/drive_c/users/*/Documents/My\ Games/Binding\ of\ Isaac\ Repentance+; do list "$d"; done
    echo
    if [[ -n "${BACKUP_DIR:-}" ]]; then   # optional: another folder of save files, for comparison
        echo "## other save folder ($BACKUP_DIR)"
        list "$BACKUP_DIR"
    fi
} > "$out"
cat "$out"
command -v pbcopy >/dev/null && pbcopy < "$out" && echo && echo "Copied to the clipboard."
