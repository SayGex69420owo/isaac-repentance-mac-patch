#!/bin/bash
# Build the steamwebhelper wrapper and install it into every cef.win* directory
# of the Steam installation in a Wine prefix. Valve's binary is preserved as
# steamwebhelper_real.exe; re-running is safe (also after Steam updates it).
# Usage: install-webhelper-wrapper.sh <WINEPREFIX> [--status|--uninstall]
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
prefix="${1:?usage: $0 <WINEPREFIX> [--status|--uninstall]}"
cc=$(ls -d "$root"/tools/llvm-mingw-*/bin/x86_64-w64-mingw32-clang 2>/dev/null | tail -1 || true)   # setup.sh installs one
[[ -x "$cc" ]] || { echo "llvm-mingw not found in tools/ (run ./setup.sh)" >&2; exit 1; }
out="$here/build/steamwebhelper.exe"
cef_root="$prefix/drive_c/Program Files (x86)/Steam/bin/cef"
# Valve's helper is several MB; the wrapper is a few KB.
wrapper_max=1000000

size_of() { stat -f%z "$1"; }

if [[ "${2:-}" == "--status" ]]; then
    for dir in "$cef_root"/cef.win*; do
        [[ -f "$dir/steamwebhelper.exe" ]] || continue
        if (( $(size_of "$dir/steamwebhelper.exe") <= wrapper_max )); then
            echo "$(basename "$dir"): wrapper installed"
        else
            echo "$(basename "$dir"): Valve's original (wrapper missing, e.g. after a Steam update); re-run without --status"
        fi
    done
    exit 0
fi

if [[ "${2:-}" == "--uninstall" ]]; then
    for dir in "$cef_root"/cef.win*; do
        if [[ -f "$dir/steamwebhelper_real.exe" ]]; then
            mv -f "$dir/steamwebhelper_real.exe" "$dir/steamwebhelper.exe"
            echo "restored Valve binary in $(basename "$dir")"
        fi
    done
    exit 0
fi

mkdir -p "$here/build"
"$cc" -municode -mwindows -O2 -Wall -Wextra -o "$out" "$here/steamwebhelper-wrapper.c"

for dir in "$cef_root"/cef.win*; do
    target="$dir/steamwebhelper.exe"
    real="$dir/steamwebhelper_real.exe"
    [[ -f "$target" ]] || { echo "skip $(basename "$dir"): no steamwebhelper.exe"; continue; }
    if (( $(size_of "$target") > wrapper_max )); then
        # target is Valve's (fresh install or Steam updated it): stash it.
        cp -f "$target" "$real"
    elif [[ ! -f "$real" ]] || (( $(size_of "$real") <= wrapper_max )); then
        echo "ERROR $(basename "$dir"): Valve's steamwebhelper.exe is missing; reinstall Steam" >&2
        exit 1
    fi
    cp -f "$out" "$target"
    echo "wrapper installed in $(basename "$dir") (real: $(size_of "$real") bytes)"
done
