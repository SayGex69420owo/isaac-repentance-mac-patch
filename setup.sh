#!/bin/bash
# One-time setup on a new Mac: everything needed to play the Windows version of
# The Binding of Isaac: Repentance+ through Wine. Safe to run again; finished
# steps are skipped. Nothing outside this folder is changed, except the optional
# Desktop app at the end.
#
#   1. checks: Apple Silicon, Rosetta 2, Xcode Command Line Tools
#   2. downloads Wine Staging 11.18 (Gcenx, osx64) into tools/, checksum-verified
#   3. downloads llvm-mingw 20261006 (macOS universal) into tools/, checksum-verified
#   4. creates the Wine prefix (an isolated "Windows" folder) in repplus-patch/prefix/
#   5. installs Valve's Windows Steam client into it and lets it update once
#   6. installs the Steam window fix, the per-game Wine settings and builds isaacgl.dylib
#   7. optionally creates "Isaac Repentance+.app" on the Desktop
#   8. starts Steam: you log in, Steam offers to install the game and then starts it
# Usage: ./setup.sh
set -euo pipefail
root="$(cd "$(dirname "$0")" && pwd)"
tools="$root/tools"
# WINEPREFIX (this checkout's own prefix), wine, wineserver, steam_dir and process helpers
. "$root/repplus-patch/launch/common.sh"

WINE_URL="https://github.com/Gcenx/macOS_Wine_builds/releases/download/11.18/wine-staging-11.18-osx64.tar.xz"
WINE_SHA256="b63704b91af269bc026a87f12bd297c4a50caaf570c322e600b6621ef918f127"
MINGW_NAME="llvm-mingw-20261006-ucrt-macos-universal"
MINGW_URL="https://github.com/mstorsjo/llvm-mingw/releases/download/20261006/$MINGW_NAME.tar.xz"
MINGW_SHA256="a7a19b11048e89372637d26279cdb7c430ed3cf8c1e224310421312bf986ebf6"
STEAM_URL="https://cdn.cloudflare.steamstatic.com/client/installer/SteamSetup.exe"

step() { printf '\n== %s\n' "$1"; }
die() { printf '\nSETUP STOPPED: %s\n' "$1" >&2; exit 1; }

download() {  # url, sha256, destination file
    mkdir -p "$(dirname "$3")"
    echo "Downloading $(basename "$1")..."
    curl -fL --progress-bar -o "$3.part" "$1" || die "download failed: $1"
    actual=$(shasum -a 256 "$3.part" | cut -d' ' -f1)
    if [[ -n "$2" && "$actual" != "$2" ]]; then
        rm -f "$3.part"
        die "checksum mismatch for $(basename "$1") (got $actual). Nothing was installed."
    fi
    mv "$3.part" "$3"
}

unpack() {  # archive, destination folder, top-level name it creates
    # into a temporary folder first, so an interrupted unpack never looks finished
    local tmp="$2/.unpack.$$"
    rm -rf "$tmp"; mkdir -p "$tmp"
    tar -xJf "$1" -C "$tmp" || { rm -rf "$tmp"; die "unpacking $(basename "$1") failed (is the disk full?)."; }
    [[ -e "$tmp/$3" ]] || { rm -rf "$tmp"; die "$(basename "$1") does not contain $3."; }
    rm -rf "${2:?}/$3"; mv "$tmp/$3" "$2/$3"; rm -rf "$tmp"
    rm -f "$1"   # the download is not needed any more
}

step "1/8 Checking this Mac"
[[ "$(uname -m)" == arm64 ]] || die "this project is for Apple Silicon Macs."
arch -x86_64 /usr/bin/true 2>/dev/null \
    || die "Rosetta 2 is missing. Install it with:  softwareupdate --install-rosetta --agree-to-license  and run ./setup.sh again."
xcrun --find clang >/dev/null 2>&1 \
    || die "the Xcode Command Line Tools are missing. Install them with:  xcode-select --install  and run ./setup.sh again."
if [[ -x "$wine" ]] && steam_running; then
    die "the Windows Steam of this folder is already running. Quit it from its Steam menu (or run repplus-patch/launch/steam-shutdown.sh) first."
fi
if macos_steam_running; then
    die "the macOS Steam app is running. Quit it (Steam > Quit Steam in the menu bar) and run ./setup.sh again; it gets in the way of the Windows Steam."
fi
echo "OK: Apple Silicon, Rosetta 2, Xcode Command Line Tools."
[[ -d "$root/.git" ]] && git -C "$root" config core.hooksPath scripts/git-hooks

step "2/8 Wine Staging 11.18"
if [[ -x "$wine" ]]; then
    echo "already installed"
else
    download "$WINE_URL" "$WINE_SHA256" "$tools/downloads/wine-staging-11.18-osx64.tar.xz"
    mkdir -p "$tools/wine"
    unpack "$tools/downloads/wine-staging-11.18-osx64.tar.xz" "$tools/wine" "Wine Staging.app"
    [[ -x "$wine" ]] || die "Wine was unpacked but $wine is missing."
    echo "installed to tools/wine/"
fi

step "3/8 llvm-mingw (builds the small Steam window fix)"
if [[ -x "$tools/$MINGW_NAME/bin/x86_64-w64-mingw32-clang" ]]; then
    echo "already installed"
else
    download "$MINGW_URL" "$MINGW_SHA256" "$tools/downloads/$MINGW_NAME.tar.xz"
    unpack "$tools/downloads/$MINGW_NAME.tar.xz" "$tools" "$MINGW_NAME"
    [[ -x "$tools/$MINGW_NAME/bin/x86_64-w64-mingw32-clang" ]] || die "llvm-mingw was unpacked but its compiler is missing."
    echo "installed to tools/$MINGW_NAME/"
fi

step "4/8 Wine prefix (repplus-patch/prefix/repplus)"
if [[ -f "$WINEPREFIX/system.reg" ]]; then
    echo "already created"
else
    mkdir -p "$(dirname "$WINEPREFIX")"
    echo "Creating it (about a minute; macOS may ask about Rosetta once)..."
    WINEDEBUG=-all "$wine" wineboot --init >/dev/null 2>&1 || die "wineboot failed."
    "$wineserver" -w
    echo "created"
fi

step "5/8 Windows Steam client"
if [[ -f "$steam_dir/steam.exe" ]]; then
    echo "already installed"
else
    download "$STEAM_URL" "" "$tools/downloads/SteamSetup.exe"   # Valve updates this file; no fixed checksum
    echo "Installing (silent)..."
    WINEDEBUG=-all "$wine" "$tools/downloads/SteamSetup.exe" /S >/dev/null 2>&1 || true
    "$wineserver" -w
    rm -f "$tools/downloads/SteamSetup.exe"
    [[ -f "$steam_dir/steam.exe" ]] || die "the Steam installer did not create steam.exe."
fi
if ls "$steam_dir"/bin/cef/cef.win*/steamwebhelper.exe >/dev/null 2>&1; then
    echo "Steam client files present"
else
    echo "Starting Steam once so it can download its client files (several minutes)."
    echo "A Steam window may appear and stay black or show an update; that is expected. Do not log in yet."
    rm -f "$root/analysis/logs/steam-play.current" "$root/analysis/logs/steam-traced.current"
    "$wineserver" -k 2>/dev/null || true   # leftovers of an interrupted earlier attempt
    trap 'echo; echo "Interrupted; stopping Steam."; "$wineserver" -k 2>/dev/null; exit 1' INT
    WINEDEBUG=-all "$wine" "$steam" >/dev/null 2>&1 &
    for i in $(seq 1 180); do
        ls "$steam_dir"/bin/cef/cef.win*/steamwebhelper.exe >/dev/null 2>&1 && break
        if (( i % 6 == 0 )); then
            last=$(tail -1 "$steam_dir/logs/bootstrap_log.txt" 2>/dev/null | cut -c23-120 || true)
            echo "  $(( i * 5 / 60 )) min, $(du -sh "$steam_dir" 2>/dev/null | cut -f1): ${last:-starting}"
        fi
        sleep 5
    done
    trap - INT
    if ! ls "$steam_dir"/bin/cef/cef.win*/steamwebhelper.exe >/dev/null 2>&1; then
        "$wineserver" -k 2>/dev/null || true
        echo "Last lines of Steam's own log:" >&2
        tail -5 "$steam_dir/logs/bootstrap_log.txt" >&2 2>/dev/null || true
        die "Steam did not finish downloading its client within 15 minutes (see its log above). Run ./setup.sh again."
    fi
    echo "Client files are there; waiting until Steam has started its window (up to 3 minutes), then closing it."
    for _ in $(seq 1 36); do [[ -n "$(prefix_pids 'steamwebhelper')" ]] && break; sleep 5; done
    sleep 15   # let it finish writing its files
    "$root/repplus-patch/launch/steam-shutdown.sh" || die "Steam did not close. Quit it from its Steam menu (or wait a minute) and run ./setup.sh again."
fi

step "6/8 Steam window fix, game settings, OpenGL fix"
"$root/repplus-patch/steam-ui/steam-update-inhibit.sh" on --no-start || die "installing the Steam window fix failed."
"$root/repplus-patch/setup/repplus-overrides.sh" apply || die "applying the Wine settings failed."
"$root/repplus-patch/glcore/build-isaacgl.sh" >/dev/null || die "building isaacgl.dylib failed; run repplus-patch/glcore/build-isaacgl.sh to see why."
echo "done"

step "7/8 Desktop app"
answer="${SETUP_DESKTOP_APP:-}"
[[ -n "$answer" ]] || read -r -p "Create 'Isaac Repentance+' on the Desktop? It starts Steam and the game with one double-click. [Y/n] " answer || answer=y
if [[ "$answer" =~ ^[Nn] ]]; then
    echo "skipped (you can run repplus-patch/launch/install-desktop-app.sh later)"
else
    "$root/repplus-patch/launch/install-desktop-app.sh"
fi

step "8/8 Starting Steam and the game"
cat <<EOF
Setup finished. Starting Steam and the game now:
  - Log in to Steam with your own account when its window appears.
  - Steam then offers to install The Binding of Isaac: Rebirth (with Repentance+ if you own it);
    confirm, wait for the download, and the game starts. Your saves come from Steam Cloud.
  - Next time, just double-click "Isaac Repentance+" on the Desktop
    (or run repplus-patch/launch/play-repplus.sh).
EOF
"$root/repplus-patch/launch/play-repplus.sh"
