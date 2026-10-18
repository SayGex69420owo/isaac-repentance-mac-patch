# IsaacPort

Run the **Windows** version of *The Binding of Isaac: Repentance+* on an **Apple Silicon Mac**,
using Wine, Rosetta 2 and a small OpenGL compatibility layer. You use your own copy of the game
from Steam. This repository contains no game files.

> ### Made entirely with AI, just for fun
> Every line of code, every script and every document here was written by an AI model
> (Anthropic's Claude, used through Claude Code). The person who published this is not a
> programmer and takes **no credit** for the code. They described what they wanted, ran the
> commands on their own Mac, and reported back what happened. In other words, this was
> "vibe coded" from start to finish.
>
> It started as a hobby experiment and was tested on **one** Mac. Expect rough edges. Read
> a script before you run it, and keep backups of your saves (see [Saves](#saves-read-this-first)).

## What it is (and is not)

- **It is** a set of scripts and one small library (`isaacgl.dylib`) that let the unmodified
  Windows `isaac-ng.exe` run under Wine on macOS. The **Windows Steam client** runs inside
  the same Wine environment and is logged in with your own account.
- **It is not** a crack, a DRM bypass or a re-implementation of the game. Steam's ownership
  checks (SteamStub, steam_api) work as they normally do. If your Steam account does not own
  Repentance+, it will not run. Do not ask for, or share, game files.
- **Not affiliated** with Edmund McMillen, Nicalis, Valve, Apple, the Wine project or
  Anthropic. All trademarks belong to their owners.

## Why not just plain Wine?

This *is* Wine. The project adds what plain Wine was missing for this game. Each of these was
observed with plain Wine on the test Mac:

1. **Crash at start:** the Steam overlay crashes the game under WoW64 on macOS. Fix: disable the
   overlay for the game only.
2. **No usable OpenGL:** Wine on macOS hands the game Apple's legacy OpenGL 2.1, and the game quits.
   Mesa software rendering reaches the menus, but it renders on the CPU and is far too slow.
3. **Black screen on Apple's OpenGL 4.1:** the game relies on features the core profile removed
   (client-side arrays, GL_QUADS, legacy texture formats, GLSL 1.x), and Apple's driver swaps its
   draw functions at run time. `isaacgl.dylib` handles all of this, so the game renders on the GPU.
4. **Black Steam window** under Wine. Fix: a small wrapper for Steam's web helper.

Whisky, Game Porting Toolkit and D3DMetal speed up *DirectX* games. Isaac uses OpenGL, so they do
not help here.

## How it works (short version)

| Layer | What runs |
| --- | --- |
| Game | the Windows `isaac-ng.exe` (32-bit x86), unmodified |
| Windows layer | Wine Staging 11.18 (Gcenx macOS build, WoW64) |
| CPU | x86/x86_64 code translated by Rosetta 2 |
| Graphics | Apple's OpenGL 4.1 (Metal-backed), made usable by `repplus-patch/glcore/isaacgl.dylib` |
| Ownership and saves | the Windows Steam client inside Wine, with Steam Cloud |

Repentance+ asks for an old-style ("compatibility") OpenGL context. On macOS, Wine maps that to
Apple's legacy OpenGL 2.1, which is too old for the game. `isaacgl.dylib` is loaded into Wine's
macOS process (`DYLD_INSERT_LIBRARIES`) and:

- upgrades the game's context to Apple's OpenGL 4.1 core profile;
- translates the game's GLSL 1.x shaders to GLSL 4.10;
- emulates features that the core profile removed: client-side vertex and index arrays
  (streamed to the GPU through one ring buffer, one upload per draw), `GL_QUADS`, and legacy
  texture formats such as `GL_LUMINANCE` and `GL_ALPHA`;
- puts its hooks back when Apple's driver swaps them out at run time;
- includes optional diagnostics (call statistics and frame traces) that were used to find all
  of the above.

Other fixes: the Steam overlay is disabled for the game (it crashes under WoW64 on macOS), and a
tiny wrapper makes the Steam window render under Wine.

The full story, including every dead end, is in `analysis/DECISIONS.md`.

## Status

Tested on: an Apple M5 Mac, macOS 26.6.2, Wine Staging 11.18 (osx64), game version
v1.9.7.17.J460.

| Works | Not working or not tested |
| --- | --- |
| Menus and gameplay at 60 fps on the GPU | Mods that need REPENTOGON (or other native loaders). Turn mods off in the game's settings. |
| Keyboard, sound | Controllers (not tested) |
| Online co-op by joining another player's lobby | Online "Quick match" crashes the game (a crash inside Wine; see `analysis/DECISIONS.md` E34) |
| Fullscreen (F) | Resizing the window is sometimes glitchy |
| Steam Cloud saves (after the steps below) | Any other Mac or macOS version |

**Long-term risk:** everything depends on Rosetta 2. Apple has announced that full Rosetta stays
available through macOS 27 only.

## Saves: read this first

Your progress lives in **Steam Cloud**. The Windows Steam inside Wine is a separate Steam
installation with its own settings; the macOS Steam app's settings do not count.

1. **Back up your saves first.** On a Windows PC they are in
   `Steam\userdata\<your id>\250900\remote\`. Copy the whole folder somewhere safe.
2. In the Windows Steam inside Wine, keep **Steam Cloud on** (Steam → Settings → Cloud, and the
   game's Properties → General) **before the game's first start**. Steam then downloads your
   progress before the game runs.
3. If the game was started once with Cloud off, it has made a fresh, empty save. Before switching
   Cloud on, quit Steam and run `repplus-patch/setup/cloud-restore.sh prepare`: it moves that empty
   save aside (never deletes it), so Steam downloads your real one instead.
   `cloud-restore.sh status` lists what is there.
4. If Steam ever shows a **Cloud Sync Conflict**, choose the **cloud** copy unless you are sure.
5. Do not play on two computers at the same time.

## Requirements

- An Apple Silicon Mac with **Rosetta 2** (`softwareupdate --install-rosetta --agree-to-license`)
- **Xcode Command Line Tools** (`xcode-select --install`), used to build `isaacgl.dylib`
- A Steam account that **owns The Binding of Isaac: Rebirth with Repentance+**
- Several GB of free disk space (Wine, the Windows Steam, the game) and an internet connection

`setup.sh` downloads the rest itself and checks its SHA-256 checksums:
[Wine Staging 11.18 for macOS](https://github.com/Gcenx/macOS_Wine_builds/releases/tag/11.18) (Gcenx)
and [llvm-mingw 20261006](https://github.com/mstorsjo/llvm-mingw/releases/tag/20261006) (used to build
the small Steam window fix), both into `tools/`, plus Valve's Steam installer.

The macOS Steam app cannot install Repentance or Repentance+ because they are Windows-only. That
is why this project uses the Windows Steam client inside Wine.

## Setup

```bash
git clone <this repository> IsaacPort      # or download the ZIP and unpack it
cd IsaacPort
./setup.sh
```

That is the whole setup. `setup.sh` checks the Mac, downloads and verifies the tools, creates the
Wine prefix (an isolated "Windows" folder inside `repplus-patch/prefix/`), installs the Windows
Steam client, installs the fixes, creates the Desktop app and finally starts Steam. Log in, let
Steam install the game when it offers to, and it starts. It can be run again safely.

The Steam window fix writes a `steam.cfg` that stops Steam from updating itself, so the fix stays
in place. If Steam ever insists on an update, run `repplus-patch/steam-ui/update-steam.sh`: it lets
Steam update once and then puts the fix back. Your login, games and saves are kept.

## Playing

Double-click **Isaac Repentance+** on the Desktop (`setup.sh` creates it; otherwise run
`repplus-patch/launch/install-desktop-app.sh`). It applies the Wine settings,
builds `isaacgl.dylib` if needed, starts the Windows Steam with the OpenGL fix loaded, and Steam
starts the game. You can also run `repplus-patch/launch/play-repplus.sh` from Terminal.
If the game is already running, it does nothing; it never closes or kills a running game.

Close the macOS Steam app while playing this way (the launcher tells you if it is open).
To quit, quit the game, then Steam (Steam menu, or `repplus-patch/launch/steam-shutdown.sh`).

## Troubleshooting tools

**Only Enter works (no movement, no Space):** the Mac's keyboard input source matters to Wine.
Select **ABC** (or U.S.) in the input menu of the menu bar, then click into the game window.
It can change by itself after typing in other apps.


They copy their output to the clipboard and keep a copy in `analysis/logs/reports/` (local only:
the reports contain your Mac user name and Steam details, so share them with care). All of them only
read and report, except `steam-traced-run.sh`: it restarts Steam with tracing, asks before closing a
running game, and closes the game when the watch time is over. Afterwards Steam keeps running with
tracing (slow, large logs); the Desktop app restarts it normally, or run `steam-shutdown.sh`.

| Script | Shows |
| --- | --- |
| `repplus-patch/launch/play-log.sh` | OpenGL-fix messages and the end of the game's log for the last session |
| `repplus-patch/launch/save-status.sh` | which save files exist and whether Steam Cloud is on |
| `repplus-patch/launch/steam-traced-run.sh 180` | a fully traced run with a summary (large logs; `--gltrace` and `--keytrace` add even more; `--no-mods`/`--mods` switch mods off/on) |
| `repplus-patch/launch/game-trace-tail.sh` | how the last traced run ended |
| `repplus-patch/launch/last-crash.sh` | the newest macOS crash report of Wine |

## Options for experiments

Set them in Terminal in front of the launcher, e.g. `ISAACGL_STREAM=subdata repplus-patch/launch/play-repplus.sh`
(quit Steam first, so it starts with them). None is needed for normal play.

| Variable | Effect |
| --- | --- |
| `ISAACGL_STREAM=subdata` or `=data` | upload vertex data with `glBufferSubData` / `glBufferData` instead of the default mapped ring buffer (to compare speed, or if graphics glitch) |
| `ISAACGL_DEBUG=1` | log every decision of `isaacgl.dylib` and check for GL errors after draws (slower) |
| `ISAACGL_STATS=1` | every 10 s, log which OpenGL calls the game makes most (for performance work) |
| `ISAACGL_FPS=0` | no frame-rate line in the log (by default one every 10 s: fps, slowest frame, stutters) |
| `ISAACGL_CLIENT_ARRAYS=0` | turn the vertex array emulation off (the game then draws nothing; for diagnosis) |
| `ISAACPORT_STEAM_ARGS="..."` | extra start options for Steam, e.g. `-silent` (untested here) |
| `ISAACPORT_PREFIX=/path` | use another Wine prefix than `repplus-patch/prefix/repplus` |

## Repository layout

- `setup.sh`: one-time setup (see above)
- `repplus-patch/`: the compatibility patch: launchers (`launch/`, shared settings in `launch/common.sh`),
  setup helpers (`setup/`), the Steam window fix (`steam-ui/`) and the OpenGL layer (`glcore/isaacgl.c`,
  with unit tests in `glcore/test/` that run on any Mac or Linux machine: `glcore/test/run-tests.sh`)
- `analysis/`: the development log (`DECISIONS.md`), evidence summaries and analysis scripts
- `scripts/`: git hooks

`.gitignore` is an allowlist, and the pre-commit hook refuses executables, Isaac resource archives
and anything larger than 2 MB, so game data cannot be committed by accident.

## Contributing

There is no maintainer. The person who published this does not plan to work on it further. Fork
it, fix it and share it, as you like. `analysis/DECISIONS.md` explains why things are the way they are.
Before changing `isaacgl.c`, run `repplus-patch/glcore/test/run-tests.sh`; it needs only a C compiler.
