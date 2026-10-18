#!/bin/bash
# Read-only diagnosis of the black Steam UI: wrapper status, running
# steamwebhelper processes with their flags, and the tail of Steam's own
# web-UI logs. Writes to analysis/logs (local only, may contain account
# names) and copies the result to the clipboard. Changes nothing.
# Usage: diagnose-steam-ui.sh
here="$(cd "$(dirname "$0")" && pwd)"
. "$(dirname "$0")/../launch/common.sh"
prefix="$WINEPREFIX"   # common.sh: this checkout's prefix (ISAACPORT_PREFIX overrides)
steam="$prefix/drive_c/Program Files (x86)/Steam"
mkdir -p "$root/analysis/logs"
out="$root/analysis/logs/steam-ui-diagnosis.txt"
{
    echo "# Steam UI diagnosis, $(date '+%Y-%m-%d %H:%M')"
    echo "## wrapper status"
    "$here/install-webhelper-wrapper.sh" "$prefix" --status 2>&1
    echo
    echo "## cef directories and helper sizes"
    for d in "$steam"/bin/cef/cef.win*; do
        printf '  %-12s steamwebhelper.exe %9s bytes, _real %9s bytes\n' "$(basename "$d")" \
            "$(stat -f%z "$d/steamwebhelper.exe" 2>/dev/null || echo -)" "$(stat -f%z "$d/steamwebhelper_real.exe" 2>/dev/null || echo -)"
    done
    echo
    echo "## running steamwebhelper processes (command lines shortened)"
    ps -axo pid=,%cpu=,command= | grep -i 'steamwebhelper' | grep -v grep | sed -E 's/(--[a-z-]+=)[^ ]{60,}/\1<long>/g' | cut -c1-400 | head -20
    echo "  count: $(ps -axo command= | grep -i steamwebhelper | grep -vc grep)"
    echo
    for f in webhelper.txt cef_log.txt console_log.txt; do
        echo "## Steam/logs/$f (last 40 lines)"
        tail -40 "$steam/logs/$f" 2>/dev/null | cut -c1-300 || echo "  (missing)"
        echo
    done
    echo "## Steam/logs/webhelper*.txt and cef*.txt files"
    ls -l "$steam"/logs/ 2>/dev/null | grep -i -E 'webhelper|cef|gpu' | head
} > "$out" 2>&1
cat "$out"
command -v pbcopy >/dev/null && pbcopy < "$out" && echo && echo "Copied to the clipboard."
