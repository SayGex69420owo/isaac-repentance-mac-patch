# Decision record — Repentance+ on Apple Silicon

Status: see the newest entries below. **Observed** = reproduced on this Mac with logs in `analysis/`.
**Hypothesis** = plausible but not yet tested. Logs in `analysis/logs/` are local-only (large);
committed summaries are in `analysis/evidence/`.

This is the development log kept while the project was built (by an AI model, run on one Mac).
Files it mentions under `repplus-patch/tests/`, `native-host/`, `known-good/`, the Mesa option
(`setup/mesa-gl.sh`), the early launchers (`launch-repplus.sh`, `trace-repplus.sh`) and the AI's
instruction files belonged to experiments and were removed before publishing.

## D1. Strategy: mature compatibility runtime, not a custom loader or lifter

**Decision:** Run the unmodified Windows `isaac-ng.exe` under Wine (new WoW64 mode, x86_64 under
Rosetta 2). The patch consists of a reproducible, isolated runtime setup plus narrowly scoped
fixes or shims only where Rep+ demonstrably needs them.

**What executes where:**
- Wine host side (`wine`, `wineserver`, `*.so` Unix libraries): x86_64 Mach-O under **Rosetta 2**.
- Rep+ code, its DLLs and Wine's PE-side DLLs: **32-bit x86 guest code**. It runs in 32-bit
  compatibility segments inside the x86_64 process and is translated by Rosetta. Nothing here is
  ARM64-native game code.
- Graphics: Rep+ OpenGL → Wine `opengl32` WoW64 thunks → `winemac.drv` → CGL (Apple's OpenGL 4.1
  on Metal).

**Alternatives rejected:**
| Option | Why not |
|---|---|
| Custom PE loader + x86 emulator + Win32 reimplementation | Rebuilds Wine badly. Months of work before reaching SteamStub, CRT, threads, WGL, OpenAL, EOS, curl. |
| Static lifting of `.text` to ARM64 | 7.1 MB of MSVC C++ with indirect calls, SEH, RTTI/vtables and a SteamStub entry. Not a "patch", and lifting the protection path would mean touching the DRM, which is off limits. |
| Transplanting Rep+ code into Mac AB+ | Different ABI (32-bit MSVC vs 64-bit Itanium C++). Shared class names are not shared layouts. |
| Official closer-to-Mac build | None found. Nicalis dropped macOS/Linux with Repentance. CodeWeavers lists Repentance as "Runs Great" on CrossOver (commercial Wine), which shows the Wine route works in principle. |
| CrossOver | Commercial and closed. Kept as a reference/control only. |

Runtime chosen: Gcenx `wine-staging-11.18-osx64` (sha256 `b63704b9…f127`, matches the GitHub
asset digest). Unpacked to `tools/wine/` (local only).

## D2. Ownership/DRM boundary

`isaac-ng.exe`'s entry point (`0x00CFE310`) is in `.bind` (entropy 7.98). This is SteamStub.
**Observed** under Wine: the stub unpacks a payload into anonymous memory at `0x01EB0000`. The
payload checks `HKCU\Software\Valve\Steam\ActiveProcess`, calls `IsDebuggerPresent` and
`NtSetInformationThread(ThreadHideFromDebugger)`, prints the `[S_API] SteamAPI_Init()` "no running
Steam" messages, runs `ShellExecute("steam://run/250900//")` and finally
`TerminateProcess(-1, 0x35)` (exit code 53).

**Decision:** Never patch, unpack, bypass or emulate any of this. Satisfy it the legitimate way:
run the Windows Steam client, logged in by the user, inside the same Wine prefix.

## D3. Tooling

- Ghidra (headless) and a Python venv with pefile, LIEF and capstone; Xcode's `llvm-objdump`.
- Python venv: pefile, LIEF, capstone. Xcode `llvm-objdump`.
- No custom subagents. No further MCPs.

## Evidence so far

### E1. PE map (`analysis/evidence/isaac-ng.pe_map.json`)
- PE32 i386 GUI, linker 14.29 (VS2019), ImageBase `0x400000`, ASLR+NX (DllCharacteristics `0x8140`),
  CFG-instrumented (GuardFlags `0x100`), 180,939 base relocations, 1 TLS callback.
- `.text` entropy 6.56 (ordinary code), `.bind` 7.98 (packed stub).
- Imports: KERNEL32 94, USER32 104, GDI32 13, ADVAPI32 3, SHELL32 6, ole32 3, WINMM 3, bcrypt 1,
  dbghelp 5, **OPENGL32 1** (libepoxy is linked in statically and resolves GL at runtime),
  **OpenAL32 28**, **Lua5.3.3r 67**, **steam_api 10**, **EOSSDK 82**, libcurl 11,
  MSVCP140/VCRUNTIME140 + UCRT api-sets (VS2015+ runtime; Wine provides built-ins).
- No delay imports. No XInput/DInput imports (loaded dynamically, if at all).
  3,226 exports, all `epoxy_*`.
- Not imported by the exe (may be dynamically loaded or vestigial): msvcp/msvcr 100/110,
  vccorlib110, lua51, Lua5.3.3f, libtheora*, libogg, libvorbis.

### E2. Wine WoW64 bring-up — Observed
- `wineboot --init` OK (10.6 s). MoltenVK instance OK.
- `syswow64\cmd.exe`: `PROCESSOR_ARCHITECTURE=x86`, `PROCESSOR_ARCHITEW6432=AMD64` → a real
  32-bit guest process.
- Wine's `opengl32.so` contains WoW64 GL thunks with a copy fallback for mapped buffers when
  `VK_EXT_map_memory_placed` is unavailable ("expect performance issues").

### E3. `isaac-ng.exe` loader test without Steam — Observed (`analysis/evidence/loadertest-3-provenance.txt`)
- Loaded natively at its preferred base `0x00400000`. `steam_api`, `EOSSDK-Win32-Shipping`,
  `OpenAL32`, `Lua5.3.3r` and `libcurl` all load and their `DllMain`s run. All other imports
  resolve to Wine built-ins.
- Attribution by return address (`+relay`, thread 00f4): `.bind` 134 calls, unpacked stub payload
  281 calls, **`.text` 0 calls**.
- **Milestone 1 not met in this run** (met later, see E5). The only Rep+-file code that ran was the SteamStub wrapper.
  Game code is gated on a running, logged-in Steam client, as designed.

### E4. Static strings: windowing, GL and input stack (`strings isaac-ng.exe`)
- **GLFW is statically linked** (WGL backend): "WGL: Failed to create OpenGL context",
  "WGL: OpenGL profile requested but WGL_ARB_create_context_profile is unavailable",
  "Forward-compatibility is only defined for OpenGL version 3.0 and above", "GLFW Error %d: %s".
  Plus the libepoxy loader. Engine message: "Failed to create OpenGL window / context".
  The exact requested GL version and profile are not yet known. Read them dynamically
  (`WINEDEBUG=+wgl`) once the game runs, or from the `glfwWindowHint` call sites in Ghidra.
- **Input:** GLFW gamepad mapping DB plus a "Gamepad_init" layer that dynamically loads
  `XInput1_4.dll` → `XInput1_3.dll` → `DINPUT8.dll`. Wine provides all three built in. The Gcenx build
  bundles SDL2 for host gamepads.
- **Audio:** OpenAL32.dll (bundled OpenAL Soft) → Wine winmm/mmdevapi → CoreAudio.

### E5. Milestone 1 met: Rep+ `.text` executes on this Mac — Observed (`analysis/evidence/milestone1-text-calls.txt`)
- Run 6: the Windows Steam client runs logged in, in the same prefix. Rep+ is launched through it
  (`repplus-patch/launch/trace-repplus.sh`, relay restricted to calls *from* `isaac-ng.exe`).
- SteamStub passes the legitimate way. Then calls with return addresses inside `.text`
  (`0x401000–0xb17134`): `0x9a9b09` (`CreateDirectoryA` for `Documents/My Games/Binding of Isaac
  Repentance+/`), `0xa813ab`/`0xa81405` (`RegisterClassExW`/`CreateWindowExW` "GLFW message
  window", i.e. GLFW's `createHelperWindow`).
- Execution class: **32-bit x86 guest code from `isaac-ng.exe`, translated by Rosetta 2** inside a
  Wine WoW64 process. Not ARM64-native.

### E6. Startup crash after the GLFW helper window — Observed, cause not yet known
- Run 7 (Steam overlay off): the engine `log.txt` holds 420 × `Caught exception, writing minidump...`
  (`run7-engine-log-summary.txt`).
- First exception (`run7-first-exception.txt`): a new thread (proc `7B7E9960`, started right after
  GLFW registers for HID device notifications, which is Wine sechost's `device_notify_proc`) faults
  4 ms after its start and after Rep+'s TLS callback (`0x00AEFEC1`, THREAD_ATTACH). Details:
  `EXCEPTION_ACCESS_VIOLATION`, read of `0x2EC5`, `cs=0x2b` (64-bit code).
- Reading Wine source (wine-mirror master, 11.19-dev, `dlls/wow64cpu/cpu.c`): `r13=0x10041fd24` has
  the shape of the WoW64 CPU context pointer (`TlsSlots[WOW64_TLS_CPURESERVED]+4`), and
  `rax=0x99` looks like a syscall number. **Hypothesis:** the fault is in `syscall_32to64`, during
  the thread's first syscalls. Under Rosetta the reported `rip` (`…110c`) and ExceptionAddress
  (`…1135`) differ, so do not map the faulting instruction from these alone.
- Standalone repro (`repplus-patch/tests/devnotify32.c`, 32- and 64-bit): the same GLFW sequence
  **survives** (`test-devnotify32.txt`, `test-devnotify64.txt`). So the trigger is something
  specific to the Rep+ process.
- Differences between the Rep+ process and the repro, to bisect:
  1. **LARGE_ADDRESS_AWARE.** `isaac-ng.exe` characteristics are `0x122` (LAA set). The repro was
     built without it. Under WoW64, LAA moves allocations, stacks and DLLs above 2 GB.
  2. Third-party DLLs in the process: EOSSDK, OpenAL Soft, Lua, libcurl.
  3. steam_api → steamclient (and the overlay when it is enabled).
  4. SteamStub's `ThreadHideFromDebugger` on the main thread.
  5. Rep+'s TLS callback runs in every new thread, including Wine's internal threads.
- Lead: in run 7 the main thread's `RegisterClassExW` callers (`0x7773b8ef`, `0x7674d93f`) are
  *outside* `isaac-ng.exe`, and the faulting thread's `rdi=0x7674aa30` points into that same region.
  Identify that module from run 7's `+loaddll` lines (local log) before anything else.

### E7. Crash correlates with Wine's DirectInput, not with LAA/Steam/game DLLs — Observed (`run7-bisect.txt`)
- The "outside the exe" addresses from E6 map to modules (from `+loaddll`, runs 6 and 7):
  `0x7674d93f` = `dinput8.dll+0xd93f`, which is the `RegisterClassExW` call. In Wine source that is
  `register_di_em_win_class()` in dinput's `DLL_PROCESS_ATTACH`, so dinput8 is being *loaded* on the
  main thread at that moment. The faulting thread's `rdi` = `dinput8+0xaa30`. Thread proc =
  `sechost+0x9960` (device-notify thread). Fault rip = `wow64cpu+0x110c`.
- Standalone repro bisection: plain, LAA, `--hide`, game DLLs, and steam_api with the logged-in
  client **all survive**. The repro never loads dinput8. So LAA, debugger hiding, EOS/OpenAL/Lua/curl
  and steamclient are ruled out as the trigger on their own.
- **Hypothesis:** GLFW's joystick init (`DirectInput8Create` plus device enumeration on the main
  thread) races with the sechost device-notify thread's start, under WoW64 on Rosetta.
- **Experiment:** `repplus-patch/launch/launch-repplus.sh --no-dinput8` sets
  `WINEDLLOVERRIDES=dinput8=d`. GLFW treats dinput8 as optional and falls back to XInput only. This is a
  runtime setting, not a change to `isaac-ng.exe`. If Rep+ then gets past startup, a launcher-level
  workaround exists. Then root-cause the race Wine-side, without losing DirectInput-only pads.
- Attempt 1 (env var `WINEDLLOVERRIDES`) was **invalid**. The first process exited with 53 after
  1 s, because SteamStub hands the launch to the running Steam client (`steam://run`). Steam then starts the
  real game process with *its own* environment, so the override never reached it (engine log
  again: 400 × minidump). Lesson: anything the game process needs must live in the prefix (per-app
  registry keys under `HKCU\Software\Wine\AppDefaults\isaac-ng.exe`), not in the
  launcher's environment. The launcher now does this and follows the Steam-started process.
- Attempt 2 (per-app override, run 9): **behaviour changed**. Steam started
  Rep+, and the process stayed alive ≥ 97 s at ~100 % CPU (one core). dinput8 was not mapped, no
  window appeared, and `log.txt` was truncated and stayed empty. Previously: crash plus minidump
  loop. Where it spins is not known yet. Possibilities: a later startup stage busy-waiting; the
  minidump path without output; or SteamStub itself. The game-side trace is missing, because only
  Steam's environment reaches the game.
- Next: `repplus-patch/launch/steam-traced-run.sh`. It restarts Steam with
  `WINEDEBUG=+pid,+timestamp,+seh,+loaddll,err+all`, launches via `steam.exe -applaunch 250900`,
  takes a macOS `sample` of the game at 30 s, and summarises the trace lines of the game pid.

### E8. Root mechanism: a 32→64 transition entered in 32-bit mode, under the Steam overlay — Observed + decoded
Evidence: `run10-steam-traced.txt` (Steam itself traced, so the Steam-started game inherits tracing).
- With dinput8 disabled, the **same fault** appears on the game's *main* thread: `addr=7BDD1135`,
  read of `0x2EC5`, `eax=0x99`, `ecx=0x14`. So dinput8 (E7) was a bystander, not the cause.
- `eax=0x99` is `NtFlushInstructionCache` (`dlls/ntdll/ntsyscalls.h`, ALL_SYSCALLS32). Code that
  has just written machine code calls it, i.e. a hooking engine.
- **Decoding (exact).** `0x7bdd1135` is `wow64cpu!syscall_32to64+0x29` (start `0x7bdd110c`, from
  run 7), i.e. `mov cs32_sel(%rip),%edx` = `8b 15 <disp32>`. The 32-bit context reports `cs=0107`,
  a 32-bit selector. Executed in **32-bit mode**, `8b 15 disp32` is `mov edx,[disp32]`, an absolute
  read of the RIP-relative displacement → **`0x2EC5`** (= `cs32_sel`'s offset). Running the
  preceding bytes as 32-bit code reproduces every register: `49`=`dec ecx` and five `41`=`inc ecx`
  give a net +5 (`ecx=0x14`). `87 e6`=`xchg esi,esp` gives `esp=7bd50f4a` and `esi=0026f0f4`
  (= the real stack). `41 8b 16`=`mov edx,[esi]` loads the return address into 32-bit ntdll
  (`edx=7be3db90`).
  ⇒ Something transferred control to `syscall_32to64` **without switching CS to the 64-bit
  selector**. Wine's own thunk (`ljmp *` with `cs64_sel`) works for every other syscall.
- First SEH handler on the faulting stack: `0x780A43B7`, inside **`gameoverlayrenderer.dll`**
  (Steam overlay, base `0x77FC0000`; also loaded at that base in runs 6 and 7, despite "overlay off").
  The crash follows the load of an input DLL (dinput8 in run 7, xinput1_4 in run 10, `edi` points into
  it). The overlay hooks input APIs.
- **Hypothesis (to test):** the overlay's hook engine handles the WoW64 transition and hardcodes
  the Windows/Linux 64-bit CS `0x33`. On macOS the 64-bit user CS is `0x2b` (seen in these traces),
  so its far jump lands in a 32-bit segment. Rep+'s handlers (`00AEF9D0`, filter `00931458`) catch
  the fault and retry, which gives the 100 % CPU loop and earlier the minidump loop.
- **Experiment:** `steam-traced-run.sh --no-overlay`. This sets the per-app override
  `gameoverlayrenderer=""` in the prefix registry: no Steam files touched, game unmodified,
  ownership checks untouched. dinput8 goes back to default.
- If confirmed, possible patch levels: (a) ship the per-app override (overlay off for Rep+);
  (b) Wine-side, investigate whether macOS can provide a 64-bit code selector at `0x33`, or make
  wow64cpu tolerate a transition entered in the wrong mode. That needs evidence first.

- Outside support for the workaround (not proof for macOS): a FreeBSD user had to blacklist
  `gameoverlayrenderer.dll` in winecfg to run 32-bit games under Wine (FreeBSD forums). An old Wine
  bug (46305) reports a crash in gameoverlayrenderer even with the overlay disabled in Steam. That
  matches runs 6 and 7, where "overlay off" still loaded it.
- Wine source check (`dlls/ntdll/unix/signal_x86_64.c`): on macOS `cs64_sel` is read from `%cs`
  (`0x2b`, xnu `USER64_CS`), and `cs32_sel` is an LDT entry. Linux uses `0x33`/`0x23`. Selector
  `0x33` cannot be made 64-bit from user space on macOS (GDT), so a Wine-side remap is not a small
  fix.
- Patch pieces, ready but **not yet verified**: `repplus-patch/setup/repplus-overrides.sh
  apply|remove|status` (per-app `gameoverlayrenderer` disabled for `isaac-ng.exe`) and
  `repplus-patch/launch/play-repplus.sh` (apply, make sure Steam runs, `-applaunch 250900`).

### E9. Overlay off → startup crash gone; Rep+ gets a GL 2.1 context and quits — Observed (`run11-no-overlay.txt`)
- With `gameoverlayrenderer` disabled per app: **zero access violations** (runs 7–10 all had the
  `wow64cpu+0x1135` fault). E8 is confirmed as the trigger. The per-app override is the first real
  patch component.
- Rep+ creates its GL context. Its engine log reads `OpenGL version 2.1 Metal - 90.5` and `GLSL version
  1.20`, i.e. the winemac **legacy** context. 4 s later the game runs its shutdown handler
  ("Shutdown handler: cleanup.") and exits on its own. No window was reported.
- **Hypothesis:** Rep+ needs more than GL 2.1 / GLSL 1.20 and shuts down. The requested
  version/profile (`macdrv_context_create attrs`) and any message are in the local log.
  `repplus-patch/launch/report-last-trace.sh` extracts them, without relaunching.
- Side finding: Steam's update restored Valve's `steamwebhelper.exe` in `cef.win64`. That brings back
  the black Steam UI. Re-run `install-webhelper-wrapper.sh` to fix it.

### E11. MILESTONE: Rep+ reaches its main menu on the Mac — Observed (`run12-mesa-llvmpipe-menu.txt`)
- Configuration: unmodified `isaac-ng.exe` (32-bit x86 guest code, translated by Rosetta, inside
  Wine 11.18 WoW64). Ownership comes from the logged-in Windows Steam in the prefix. Per-app
  overrides for isaac-ng.exe: `gameoverlayrenderer` disabled, `opengl32` = Mesa 26.2.4 with
  `GALLIUM_DRIVER=llvmpipe`.
- Engine log: `OpenGL version 4.6 (Compatibility Profile) Mesa 26.2.4`, `GLSL 4.60`, OpenAL Soft,
  XInput 1.4, Theora, Lua, archives loaded in 5.1 s, all 20 shaders compiled. This confirms E10:
  given a compatibility-profile GL, Rep+ boots. Menu shown and keyboard works there.
- Open problems:
  1. **Saves / Steam Cloud (safety first):** the game started without progress. Cloud state in
     the prefix is unknown, and the Steam UI is invisible, so conflict dialogs cannot be seen. Do not
     play further until `launch/save-status.sh` has been read and the user has decided.
  2. **Mods:** Workshop mods load. In a run, their error messages pile up over the screen. Many
     probably need REPENTOGON, which is not part of a clean baseline. Next: test with mods disabled.
  3. **Speed:** llvmpipe is CPU rendering, translated by Rosetta. Next: `--mesa zink` (GPU via
     MoltenVK).
  4. **In-game controls not working:** unclear. Could be the mod error state, the frame rate, or
     input. Re-test once 2 and 3 are fixed.

### E35. Frame rate on the Mac: 60 fps on average, with short hitches — Observed, cause open
- New meter (CGLFlushDrawable interposed, one line per 10 s). A play session:
  59.5-60.0 fps throughout (vsync-locked), 17-140 draws and 20-70 KB streamed per frame, i.e. the GL
  load is small. In most gameplay windows 1-5 frames take 40-75 ms (3-4 frames' time), which feels
  like stutter; paused/idle windows have none. Hitches do not grow with the draw count.
- The meter now also logs each slow frame split into time in draws, texture uploads, shader
  compiles, the swap, buffer renewals and the rest (the game and all other GL calls through Wine).

### E34. Online co-op "Quick match": the game stops in a Wine-side crash — Observed, cause open
- Traced run (overlay off, isaacgl on): 24 s after start, Quick match makes a game thread (0370) fault:
  `Unhandled page fault on read access to 00002EC5 at address 7BDD1135`, then
  `NtRaiseException: Exception frame is not in stack limits => unable to dispatch exception`, so no
  Windows crash dialog or winedbg backtrace. The same address appeared in an earlier run with the overlay
  on, so the overlay is not the cause, and isaacgl (rendering) is not involved.
- 0x7BDD1135 lies between the game process's 32-bit kernel32.dll (0x7BD60000, size 0x58000 in this
  Wine build) and ntdll.dll (0x7BDF0000): not inside any module the trace lists. A thread with an
  invalid stack and code outside any module points at Wine's WoW64 thread/exception handling on macOS
  rather than the game. The faulting thread logs nothing before the crash with the channels used.
- Next steps for whoever continues: trace with `+thread,+module,+virtual,+winsock` (only for a short
  run: the log grows fast), find which code creates thread 0370 and what owns 0x7BDB8000-0x7BDF0000, and
  test a newer Wine build. Single-player and local co-op are unaffected.
- A later untraced session joined another player's lobby and played online co-op without problems,
  so online play itself works; only Quick match crashes.

### E33. Streaming build on the Mac: a full run works — Observed
- After updating the test copy to the current build (it had stayed on an old one, so the previous
  play logs were from the old build), a full run was played from the Desktop app.
  isaacgl: 4,000,000 draws, all emulated through the ring buffer (map mode), about 465 bytes per draw,
  the 4 MB buffer renewed every ~9,000 draws; no GL errors reported; engine log ends with
  "Isaac has shut down successfully" after a save to Steam Cloud.
- Open: online co-op "Quick match" makes the game stop at once (traced run pending).

### E32. Launcher and setup review: safety and speed — Implemented (not yet run on the Mac)
- The game is never killed any more. Scripts that need it closed (Steam shutdown, traced runs) ask in
  Terminal and close it like its close button (`taskkill` without `/f`, so it saves and exits); the
  Desktop app does nothing when the game already runs. Before, `steam-shutdown.sh` and traced runs sent
  SIGTERM/SIGKILL, which can lose progress since the last save.
- Steam and game detection counts only processes of this checkout's prefix (`WINEPREFIX` in the
  process environment, read with `ps -E`; unreadable environments count as ours, as before). The prefix
  comes from `ISAACPORT_PREFIX`, not from an inherited `WINEPREFIX` meant for other Wine use.
- Whether the running Steam has the OpenGL fix is read from its environment (isaacgl.dylib and
  `WINEDEBUG=-all`, so a traced Steam is restarted); the marker file is the fallback.
- Launch speed: the Wine settings are checked by reading `user.reg` (instant) instead of starting
  Wine's `reg` tool (seconds); isaacgl.dylib is rebuilt only when its source changed, by rename, so a
  failed rebuild keeps the working dylib. A launch lock stops double starts from a double double-click.
- After a Steam update replaced the Steam window fix, the launcher puts it back; `update-steam.sh` lets
  Steam update once on purpose.
- Per-run reports moved to `analysis/logs/reports/` (local only): they had overwritten a tracked file.
- Not changed, on purpose: Steam's start flags (`-silent`, `-noverifyfiles`, more `-cef-*` switches)
  and the Mac driver's registry options. They are untested here; `ISAACPORT_STEAM_ARGS` passes extra
  Steam flags for experiments.

### E31. Client-array streaming: one upload per draw — Implemented, tested on Linux (not yet on the Mac)
- Before: every emulated draw made one `glBufferData` per client attribute (from vertex 0 up to the
  highest index) plus one for the indices, about 3 uploads and ~10 GL calls per sprite batch, each
  through Wine's thunks on the way in and Apple's engine on the way out; GL_QUADS also did `malloc`.
- Now each upgraded context has one stream buffer used as a ring. A draw reserves one range, writes
  the interleaved vertex block (attributes whose memory overlaps are copied once) and the indices into
  it through one `glMapBufferRange(WRITE | INVALIDATE_RANGE | UNSYNCHRONIZED)`, and only the vertices
  between the lowest and highest index are copied, with the indices rebased to start at 0 (not when an
  enabled attribute comes from one of the game's own buffers). When the ring is full its storage is
  orphaned (`glBufferData(NULL)`), the pattern Apple's GL guide recommends for streamed data, so the GPU
  neither waits nor reads overwritten data. If mapping fails, it falls back to `glBufferSubData`.
  `ISAACGL_STREAM=subdata` or `=data` select the simpler paths for an A/B comparison.
- Robustness: driver functions are kept per context (Apple can give contexts different ones, E23);
  contexts isaacgl did not upgrade are passed through; empty dispatch slots stay empty; invalid
  attribute pointers go to the driver; `glGetError` only with `ISAACGL_DEBUG=1` (it took errors from the
  game); GLSL `#version` only at a line start, explicit output location, more legacy texture functions;
  the texture swizzle is reset when a texture gets a modern format.
- An independent review of the rewrite found, and tests confirmed: a crash when a draw reads past the
  game's index buffer (now passed to the driver, which reports it), a crash when an array stays enabled
  after its memory is freed (only arrays the current program reads are copied; the mask is taken at
  link time), dropped draws for overlapping arrays with different strides (blocks now sorted by the
  first byte used), and wrong channels for legacy textures given RGBA data (now RGBA8 + swizzle). Also:
  destroyed contexts release their record (CGLDestroyContext/CGLReleaseContext interposed), more slots
  are re-hooked after a driver swap, byte indices are widened to 16 bits (Metal has no byte indices).
- Linux test harness (`glcore/test/run-tests.sh`, fake strict core driver, ASan/UBSan): 539 checks pass,
  1.00 buffer uploads per draw (was 3.00); the real source also compiles as a macOS x86_64 object with
  the trampolines and all seven interposers.
- To check on the Mac: game renders as before, `play-log.sh` shows "client arrays ... streamed through a
  4096 KB buffer (map)", CPU use of the game process (Activity Monitor) compared with the previous build.

### E30. Fresh install with setup.sh in a separate copy: works end to end — Observed
- A copy of the published tree in a new folder, `./setup.sh`: Mac checks, Wine 11.18 and llvm-mingw
  downloads (checksums match), prefix, Steam installer, the Steam window fix, game settings, isaacgl.dylib,
  Desktop app. First attempt: Steam's bootstrapper hung at `Manifest download: waiting for download to
  finish`; after killing leftovers (`wineserver -k`) the second attempt fetched the client within a minute.
  setup.sh now does that cleanup itself and shows Steam's own progress.
- Log in, install the game in the new Windows Steam, start via the Desktop app: the game runs and the
  progress comes down from Steam Cloud (Cloud on by default in a fresh Windows Steam).

### E29. Play session via play-repplus.sh: fullscreen works; surface follows the viewport — Observed
- Fullscreen (F) worked. isaacgl log: window-framebuffer viewports 1920x1220 / 1920x1240 / 960x540 with
  winemac's surface backing size within 0-3 px of each (1921x1221, 1921x1243, 1920x1240...), repeated over several
  toggles; no mismatch that would produce a zoomed corner. The earlier zoom was most likely a transient of the
  resize (or of a Steam started without the hook guards). Engine log ends with "Isaac has shut down successfully"
  after a save to Steam Cloud.
- Desktop app created (`install-desktop-app.sh`, osacompile).

### E28. Real progress restored from Steam Cloud; launcher; fullscreen shows a zoomed corner — Observed
- With the test folder moved aside (`userdata/<steam-account-id>/250900.test-<date>`) and Cloud switched on in
  the **Windows** Steam of the prefix (the user had first switched it in the native macOS Steam, which does not
  affect the prefix), the user's real progress appeared in the game. The native macOS Steam lists only AB and AB+
  as DLC because Repentance/Rep+ have no macOS build; the account owns all DLC.
- Starting the game with the Play button of a Steam started by `play-repplus.sh` works (the game inherits the
  Steam environment with isaacgl.dylib). Mods switched off by the user in the game's settings (their errors
  overlaid the screen; REPENTOGON etc. need their own loader).
- Fullscreen (F) no longer crashes but shows a zoomed-in corner, and window resizing is unreliable. isaacgl now logs,
  for viewports on the window framebuffer, the surface backing size winemac set on the context.
- `play-repplus.sh` passes `-applaunch 250900` to a Steam it starts (no Enter prompt); `--gui`/`--steam-only`;
  `install-desktop-app.sh` creates a Desktop app that runs it; `play-log.sh` shows a play session's messages.

### E27. Fullscreen exit explained: our viewport hook called glGetIntegerv without a current context — Observed + fix
- `game-trace-tail.sh` on run 25: after the F key, the engine logs `Viewport: 1920x1220`, then host thread 0964
  dies: `err:seh:send_debug_event Exception c0000005 in system thread at 0x7ffb249d40ac`, read of address
  0x348, rax=0. 0x348 = 8 + 104*8 = offset of `disp.get_integerv` (slot 104) in a CGLContextObject, i.e.
  libGL's `glGetIntegerv` with CGLGetCurrentContext() == NULL. (No macOS crash report: Wine handled the fault.)
- Cause: on a resize winemac updates the NSOpenGLContext on the Cocoa main thread (`-update`,
  `wine_updateBackingSize`, cocoa_opengl.m), and Apple's GL engine calls `disp.viewport` there, which is our
  hook; for a new size it logs and calls `glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING)`, with no context current
  on that thread. Fullscreen produced a new size (1921x1220), hence the crash.
- Fix: every hook first checks `CGLGetCurrentContext()->rend == rend` and otherwise forwards the call untouched.

### E26. Run 25 (--keytrace): keyboard fine; F (fullscreen) ends the game — Observed
- After a change of the Mac input source (still US English, HKL 0409), moving and shooting
  work. The key trace shows every key arriving with the right vkey/scan code (A 0x41/0x1e, S, D, W 0x57/0x11,
  Space 0x20/0x39, Enter, Tab, Esc, arrows 0x25-0x28 extended), `ImeProcessKey` called but not taking keys,
  and normal WM_CHAR translation. The earlier failure is therefore unexplained (possibly window focus, or the
  input-source state at Wine start); keep an eye on it.
- Pressing F (Rep+ fullscreen toggle): the engine logs `Viewport: 1920x1220, Framebuffer Width: 1921,
  Multiplier 4.80`, then the process ends ("exited by itself after 111 s"); no Windows exception is traced, so the
  failure is presumably in host code (winemac/Apple GL/isaacgl). `last-crash.sh` extracts the macOS crash report.
- Progress: still the fresh test save (Cloud off, D4). `setup/cloud-restore.sh prepare` moves the test folder
  userdata/<id>/250900 aside (user types MOVE); then Cloud is switched on in the Steam UI so the real save is
  downloaded; `status` lists the downloaded files for comparison with an existing save. `play-repplus.sh` now starts Steam with
  isaacgl.dylib (statistics off, no tracing) for normal play.

### E25. Run 24: GAMEPLAY on Apple native GL; letters/Space ignored, arrows/Enter work — Observed + hypothesis
- User report: with mods switched off in the game's own settings, a run starts and renders correctly
  ("the game appears fine"); shooting (arrow keys) works, moving (WASD) does not; Space does not work in the
  main menu, Enter does. Engine log: floor generated, start room entered, Basement music queued; 60 fps, no GL
  errors after start-up, 9,072 GL_QUADS draws emulated.
- Hypothesis (Wine 11.18 source): user32 calls `ImmProcessKey` for every key; Wine's builtin IME
  (`dlls/imm32/ime.c` ImeProcessKey) claims the key whenever the thread's keyboard layout is an IME language
  (`is_ime_hkl`: Chinese, Japanese, Korean, Indic languages, Amharic...), and winemac's `ImeToAsciiEx` passes it to
  the Cocoa input context. Character keys (letters, Space) are consumed as text input, so the game sees
  VK_PROCESSKEY instead of the key; arrows and Enter become Cocoa commands (`moveLeft:`, `insertNewline:`), are
  not consumed, and arrive as normal keys. The active HKL comes from the Mac's current input source.
- FALSIFIED: the active input source maps to HKL 0409 (US English), not an IME language, so `ImeProcessKey` returns
  FALSE and the IME path is not taken.
- Next (run 25): `--keytrace` (WINEDEBUG +key,+keyboard,+imm,+dinput,+rawinput) while the user presses W A S D,
  Space and arrows in a run: shows which Mac keycodes arrive, their vkey/scan codes, and whether Rep+ reads keys
  through DirectInput, raw input or messages.

### E24. Run 23: FIRST PICTURE on Apple native GL — Rep+ reaches character selection — Observed
- With the hooks re-installed (36 driver replacements of the vertex-attribute and draw slots logged; the driver
  swaps `draw_arrays/draw_elements/draw_range_elements` between specialised functions at run time, confirming
  E23), 148,698 draws in 2 minutes, ~96 % with emulated client arrays, 2 GL errors. The user saw the menus and
  reached character selection (fresh save, as expected with Steam Cloud off).
- Starting a run hangs on the floor load: engine log shows `[Frame 0] Starting room transition (type 0)` x206 and
  `[ModManager] load stage 0/1/13` x74, i.e. the level start repeats at frame 0. Under Mesa llvmpipe (run 12) a
  run did start, so this may be renderer-related or mod-related (many Workshop mods, some needing REPENTOGON). Next run
  prints the last 80 log.txt lines.
- `GL_INVALID_ENUM` after `glDrawArrays(mode 7)`: GL_QUADS, absent in core. Now emulated (two triangles per quad
  via a stream index buffer; quad strip → triangle strip, polygon → triangle fan).
- Save note: the game logged `Saving PersistentGameData to Steam Cloud: rep+persistentgamedata1.dat` 14 times.
  With cloud off in the prefix Steam this lands in the prefix's local userdata/.../250900/remote cache only. It is
  a fresh test save and must be moved aside (with the user's OK) before cloud is ever switched on (D4).

### E23. Run 22 (+opengl): Rep+ DOES call glDrawElements; Wine forwards it; our hook never sees it — Observed
- `WINEDEBUG=+opengl`, 60 s: 20,728 `glDrawElements` (mode 4 triangles, count 6/12…, GL_UNSIGNED_SHORT,
  client index pointer e.g. 047187A4), exactly one per batch, between the last `glVertexAttribPointer`
  (client pointers, stride 20) and the first `glDisableVertexAttribArray`. Same counts as the batches seen
  at the Apple dispatch table.
- Wine 11.18 `wow64_gl_glDrawElements` (dlls/opengl32/unix_thunks.c) only calls `funcs->p_glDrawElements`
  (OpenGL.framework export) with `ULongToPtr(indices)`; no wrapper, no filtering, and no WARN about a failed
  unix call appears. So the call reaches OpenGL.framework but bypasses our `ctx->disp.draw_elements` hook
  after draw #1.
- Hypothesis: Apple's GL engine rewrites draw slots in `ctx->disp` at run time (specialised draw paths), which
  removes our hook; the original function then gets client pointers it never saw (our `glVertexAttribPointer`
  hook keeps client arrays for upload at draw time), the draw fails in the core profile, and the frame stays black.
- Fix attempt (run 23): `check_hooks()` before every batch restores our hooks on the attribute and draw slots,
  adopting the driver's new function as the forward target (logged as "driver replaced dispatch slot");
  the exported `glDrawElements/glDrawArrays/glDrawRangeElements` are also interposed and always go through
  `ctx->disp` (counted as "exported draw calls").

### E22. Run 21: one frame recorded — every batch is set up and torn down without a draw — Observed
- 311 dispatch calls per frame, identical frame after frame (57 fps). Structure: bind FBO 1 (1024² texture 4
  RGBA8 + depth renderbuffer, complete; no `glCheckFramebufferStatus` ever returned incomplete), viewport, clear,
  then per sprite batch: `use_program` → uniforms → `active_texture/bind_texture` → per attribute
  `get_attrib_location, enable, vertex_attrib_pointer` → per attribute `get_attrib_location, disable`. **No draw
  between the last pointer and the first disable.** Then FBO 0, viewport 960×540, clear, the final blit batch
  (also no draw), `swap_APPLE`. Renderbuffer queries return 1024×1024; viewports 1024², 512², 960×540.
- So either Rep+ never calls glDrawElements, or Wine's opengl32 receives it and does not forward it to the
  host GL (our hooks sit at the Apple dispatch table, below Wine). Only draw #1, during start-up, arrived.
- Next (run 22): `steam-traced-run.sh --gltrace` adds WINEDEBUG `+opengl`, which logs every GL call in
  Wine's opengl32 thunks (the guest side); the report counts them and prints one frame as the game issues it.

### E21. Runs 19–20: Rep+ prepares every draw on Apple GL but never issues one; programs are fine — Observed
- Run 19 (call statistics, all 975 slots): per 10 s ~13k each of `vertex_attrib_pointer_ARB`/`enable_`/`disable_
  vertex_attrib_array_ARB`, ~12k `use_program_object_ARB`, ~30k `get_uniform_location_ARB`, ~27k
  `get_attrib_location_ARB`, uniforms, texture binds, ~2k clears, ~570 `swap_APPLE` (≈57 fps). **No draw slot of
  any kind** appears; the hooked draw counter stays at 1 (that one client-array draw raised 0x0506 while FBO 1
  reported complete).
- Run 20 (compile/link/location hooks): 66 shaders compiled, 0 failed; 33 programs linked, 0 failed; no
  attribute or uniform lookup returned -1. So the GLSL translation is not the cause.
- Pattern per batch: use program → uniforms → enable/pointer → (no draw) → disable. The skip decision is
  CPU-side in Rep+, between setting the pointers and disabling them, with no GL query in between (the
  `get_integerv` count equals ours from `hooked_vap`). Suspects: a GL answer read earlier (renderbuffer size
  queries ~7/frame, `glCheckFramebufferStatus` ~100/s with textures re-created at ~60/s, viewport) that makes
  the batch empty or the target invalid.
- Next (run 21): the trampolines also record the slot sequence (r10/r11 scratch only, tested on x86_64 here);
  one complete frame is printed at 30 s. Also logged: every non-complete `glCheckFramebufferStatus` with the
  attachments (incl. texture size and internal format), `glGetRenderbufferParameteriv` results and distinct
  viewports.

### E20. Run 18: framebuffer is complete; Rep+ reaches menu init but issues < 10 hooked draws — Observed
- `glRenderbufferStorage 1024x1024 0x81a6` (DEPTH_COMPONENT24). At the logged 0x0506 the draw FBO 1 is
  **complete** (color0 texture 4 RGBA8, depth renderbuffer 1 depth24/32). So the 0x0506 is a stale earlier error
  (glGetError returns the first error since the last query), probably a clear before the window surface existed.
- Fewer than 10 draws reach the hooked `draw_arrays/draw_elements/draw_range_elements` in 3 minutes (Mesa core 4.1:
  ~950/s). The engine log shows full menu initialisation ("Menu Game Init" … "Menu Online Awards Init") and menu music
  queued, so the game logic runs and only the picture is missing.
- SDK slot names (Xcode SDK): `tex_image2D`, `tex_sub_image2D` (no underscore before 2D), plus draw variants
  `draw_elements_base_vertex`, `draw_elements_instanced`, `multi_draw_*`, `draw_*_indirect`, `*_APPLE`.
- Next: signature-agnostic call statistics for **all** ~900 dispatch slots (16-byte x86_64 trampolines `lock incq
  count[i]; jmp *target[i]`, verified assembled here), logged every 10 s with SDK names. This shows which GL
  entry points Rep+ actually uses per frame. The texture-format hooks now use the right slot names.

### E19. Run 17: client arrays confirmed; first draw hits an incomplete framebuffer — Observed + next fix
- `run17-glcore-clientarrays.txt`: the first draw is a client-array `glDrawElements` (emulation taken),
  and it fails with `GL_INVALID_FRAMEBUFFER_OPERATION` (0x0506). Fewer than 50 draws in 3 minutes, so
  Rep+ apparently stops drawing after that; swaps continue, so the window stays black.
- Candidates: (a) Rep+'s render target (FBO 1, renderbuffer 1024x1024) uses a legacy texture format
  (`GL_ALPHA`/`GL_LUMINANCE`/component counts), which core rejects, so the attachment is missing or
  incomplete; (b) the default framebuffer is undefined at that moment (no surface attached to the
  context, status 0x8219).
- Added to isaacgl (mock-tested): on the first 0x0506, dump draw/read FBO bindings,
  `glCheckFramebufferStatus` and the color/depth/stencil attachments. Translate legacy texture formats
  in `glTexImage2D`/`glTexSubImage2D` (ALPHA/LUMINANCE/LUMINANCE_ALPHA/INTENSITY → R8/RG8 +
  `GL_TEXTURE_SWIZZLE_RGBA`; 3/4/BGRA → RGB8/RGBA8). Log renderbuffer formats. Route
  `glDrawRangeElements` through the client-array path. Draw counters at 10/50/500/5000.

### E18. Black window on native GL: probable client-side arrays — hypothesis + fix implemented
- User observation for run 16: "there was nothing" (no visible picture), although the log shows frames
  being presented and no errors are logged by the game.
- **Hypothesis:** Rep+'s renderer feeds `glVertexAttribPointer`/`glDrawElements` from application memory
  (client-side arrays, legal in compatibility profiles). In a core profile every such draw fails with
  `GL_INVALID_OPERATION`, so nothing is drawn. Mesa's run-14 errors cannot confirm or refute this,
  because the missing VAO error masks this check.
- Implemented in isaacgl (mock-tested): dispatch hooks on `vertex_attrib_pointer*`,
  `enable/disable_vertex_attrib_array*`, `draw_arrays` and `draw_elements`. A client pointer (no
  `GL_ARRAY_BUFFER` bound) is recorded. At draw time each enabled client attribute's range [0, max
  index] goes into its own stream VBO, and client indices go into a stream EBO. Bindings are restored.
  Draws from real buffers pass through. Diagnostics: `glGetError` after the first 300 draws and every
  5000th, logged as `GL error 0x… after …`, plus counters `draws / emulated / errors`.
  `ISAACGL_CLIENT_ARRAYS=0` disables the emulation.
- Slot names are read from the SDK's `gliDispatch.h` by `build-isaacgl.sh` (Apple uses `_ARB` names).

### E17. Rep+ runs on Apple's native GL (Metal, GPU): all shaders compile, no crash — Observed (`run16-glcore-glsl.txt`)
- With the GLSL translation, all 20 Rep+ shaders compile on Apple's 4.1 core compiler (no errors). The
  game runs 3 minutes without an access violation and presents frames; CPU ~114 % (vs ~190 % on
  llvmpipe). On-screen result: pending user observation.
- Patch stack at this point (all prefix/launcher-level, no binary changes): per-app
  `gameoverlayrenderer` disabled; `isaacgl.dylib` via `DYLD_INSERT_LIBRARIES` in Steam's environment;
  Steam UI wrapper + `steam.cfg` (UI only).

### E16. isaacgl.dylib works; next blocker: GLSL 1.x shaders on Apple's core compiler — Observed (`run15-glcore.txt`)
- The injection works: the dylib is active in the Rep+ process and upgrades both Rep+ contexts to 4.1 core,
  shared with Wine's core context, VAO bound. The engine log reads `OpenGL version 4.1 Metal - 90.5` / `GLSL 4.10`:
  **Apple's native GL on the GPU**, with no Mesa.
- Every Rep+ shader fails to compile with `#version required and missing` / `'attribute' : syntax error`.
  Rep+'s shaders are GLSL 1.10/1.20 (no `#version`, `attribute`/`varying`, `gl_FragColor`). Mesa
  accepted these in core mode; Apple's core compiler requires `#version` >= 140. With no programs the
  engine then null-derefs (read 0x44 at `0x00A6702E`).
- Fix (implemented, mock-tested): isaacgl hooks the context's `disp.shader_source` entry (CGLContext.h
  dispatch table, which catches every caller path) and prepends `#version 410` plus defines: `attribute`→`in`,
  `varying`→`out` (VS) / `in` (FS), `texture2D*`/`textureCube`→`texture*`, and `gl_FragColor`/`gl_FragData`
  → declared `out` variables. Shaders that already declare `#version >= 140` are left alone. Remaining
  risk: compatibility built-ins (`gl_TexCoord`, `gl_Color`, `ftransform`), which would show as new compile errors.

### E15. Fix candidate: isaacgl.dylib, Apple's native core 4.1 + default VAO — implemented, not yet run on the Mac
- Run-14 report, ranked: `glVertexAttribPointer(no array object bound)` 251,496x and `glDrawElements`
  90,738x make up **99.9 %** of all GL errors. The only other one is `glClampColor(GL_CLAMP_VERTEX_COLOR)`
  2x (harmless). So Rep+ is core-clean except that it never binds a VAO.
- Delivery without rebuilding Wine or touching the game: `repplus-patch/glcore/isaacgl.c`, an
  x86_64 dylib loaded via `DYLD_INSERT_LIBRARIES` (a plain variable for Wine; `env.c` passes it to child
  processes, as it did `GALLIUM_DRIVER`). It interposes the two CGL calls in winemac's
  `create_context()` (Wine 11.18): a legacy app request (starts with `kCGLPFAMinimumPolicy`, no
  profile, no `kCGLPFASingleRenderer`) gets `kCGLOGLPVersion_GL4_Core`, with aux/accum/pbuffer dropped.
  The new context shares with Wine's own core context (restoring the sharing Wine intends but marked
  broken for non-core contexts) and gets one VAO bound. Inert outside the isaac-ng.exe process. Wine's
  enumeration, renderer queries and probes are untouched (they carry an explicit profile or
  SingleRenderer).
- Ruled out first: Wine 11.18 has no AppInit_DLLs support (no hit in user32 sources) and no IFEO
  Debugger support. A same-name opengl32 proxy cannot load Wine's builtin underneath it.
- Verified on Linux with a mock CGL: enumeration and Wine core lists unchanged; the
  legacy list becomes `MinPolicy ClosestPolicy DoubleBuffer ColorSize DepthSize Profile=GL4_Core`;
  share = Wine's core context; VAO bound. Not yet compiled against the real SDK or run.
- Test: `steam-traced-run.sh --no-overlay --glcore` (builds with xcrun, sets the env, ISAACGL_DEBUG=1).
  Expected log line from Rep+: `OpenGL version 4.1 Metal`. Possible remaining issues: Wine-internal
  legacy GL calls on these contexts (Wine still believes they are compatibility contexts), deprecated
  calls that only occur in-game.

### E14. Core 4.1: Rep+ initialises completely but draws black, with a GL error flood — Observed (`run14-mesa-core41.txt`)
- Forward-compatible core 4.1 (Mesa override, i.e. the profile Apple offers natively): Rep+ accepts
  it. GL 4.1 Core / GLSL 4.10 are logged, **all 20 shaders compile**, and init reaches the menu set-up.
  So Rep+ does not hard-require a compatibility profile.
- The screen stays black. ~342k debug strings in 95 s: MESA_DEBUG reports a GL error on almost every
  call. Most likely class: core-only rules (draw without a bound VAO, client-side vertex arrays,
  deprecated state such as `GL_QUADS`/`glAlphaFunc`/line width > 1). Exact messages: local log,
  `report-last-trace.sh` (report fixed to decode these strings).
- If it is only "no VAO bound", the fix is tiny: when promoting isaac-ng.exe's context to core, create
  and bind one VAO at first make-current. That would be a winemac patch or a small GL shim, used
  with Apple's native GL on Metal.
- First decoded messages (report of run 14): Rep+'s own log lines are also sent as debug strings.
  Seen once: `Mesa: error: GL_INVALID_ENUM in glClampColor(GL_CLAMP_VERTEX_COLOR)`, a compatibility-only
  enum, harmless on its own. **`EnableMods=0` does not stop mod loading** (`LOADED MOD …` lines for the
  Workshop mods under `<game>/mods/`), so `--no-mods` is ineffective as implemented. The frequent
  messages were cut off; the report now ranks Mesa messages by count.

### E13. zink fails; next question: can Rep+ run on a core 4.1 context? — Observed + plan
- Run 13 (`run13-mesa-zink.txt`): with `GALLIUM_DRIVER=zink`, winevulkan loads and unloads at once.
  Mesa has no driver, so WGL pixel-format setup fails ("driver does not appear to support OpenGL").
  Rep+ asserts, then faults on a null object. The reason zink rejects winevulkan/MoltenVK is still
  open (portability enumeration? missing features?). The report now lists Mesa's messages in order.
- The decisive architecture question is whether Rep+'s rendering is **core-profile clean**. If yes,
  the performant patch is a small winemac change: promote isaac-ng.exe's non-core context request
  to Apple's native 4.1 core profile (CGL on Metal, GPU, no Mesa). Mesa/zink then becomes unneeded.
- **Test (no Wine rebuild):** `steam-traced-run.sh --no-overlay --mesa llvmpipe --core41 --no-mods`
  sets `MESA_GL_VERSION_OVERRIDE=4.1FC` (forward-compatible core, like Apple),
  `MESA_GLSL_VERSION_OVERRIDE=410`, and `MESA_DEBUG=1` (Mesa prints each GL error, such as a
  deprecated call). Correct menu/run rendering and no GL errors → do the winemac core-promotion
  patch. Many errors → map them; either a shim or stay on Mesa (and fix zink).

### E12. Black Steam UI: Steam repairs the wrapper away on every start — Observed (diagnosis)
- `install-webhelper-wrapper.sh` installed the wrapper into `cef.win64`, `cef.win7` and
  `cef.win7x64` . A minute later, after `steam-start.sh`, `cef.win64/steamwebhelper.exe` was
  Valve's binary again (7,488,152 B = `_real`). The two `cef.win7*` copies, which Steam does not use,
  kept the wrapper. Steam's bootstrapper verifies its package files at start-up and restores modified ones.
- Running helpers (all from `cef.win64`): browser plus `--type=gpu-process --use-gl=angle
  --use-angle=swiftshader-webgl`, renderers with `--disable-gpu-compositing`, network/storage/audio
  utilities. Multi-process, no `--single-process`: exactly the configuration that paints black.
- The UI itself works (webhelper.txt: store page, context menus, notification toasts created, "Browser
  requested transparent background, but it is not supported"). Only presentation fails.
- Wine 11.18 does not implement IFEO `Debugger` redirection (`kernelbase/process.c`; `ntdll/loader.c`
  reads IFEO only for loader options), so a registry-only redirect is not available.
- Options: (A) Valve's `steam.cfg` with `BootStrapperInhibitAll=enable` in the prefix Steam root, which
  stops self-update/repair so the wrapper persists. It needs the user's permission (a file in the Steam
  install) and a periodic un-inhibit, update, re-wrap cycle. (B) Operate without the UI: login is
  cached, the game launches through `-applaunch`, and the cloud switch is a config value.
- **Decision: option A.** `repplus-patch/steam-ui/steam-update-inhibit.sh on|off|status`
  writes or removes only its own `steam.cfg` (it refuses to touch a foreign one), re-wraps, restarts
  Steam, and checks after 45 s that `cef.win64` still holds the wrapper and that the helper runs with
  `--single-process`.
- **Result: Steam UI visible and usable.** After `steam-update-inhibit.sh on`, all three cef
  dirs keep the wrapper across Steam's start, and `cef.win64` helper runs with `--single-process`.

### D4. Save policy for the prefix
- Latest real progress: **Steam Cloud**. The user's own save backups are read-only. Never copy
  `rep_` over `rep+`.
- State (a save-status snapshot): Steam Cloud is **off** in the prefix Steam
  (`cloudenabled 0`). Run 12 created a fresh 4 KB `rep+persistentgamedata1.dat` locally. With
  Cloud off, nothing should have been uploaded. The user can verify on Valve's remote-storage page.
- Before Cloud is ever enabled in the prefix: (1) the Steam UI must be visible, so conflict dialogs
  can be seen (webhelper wrapper); (2) with Steam closed, move the fresh test files aside (moved,
  not deleted, and only with the user's go-ahead), so Steam downloads the cloud copy instead of
  offering the empty one. Until then, testing uses Cloud off and throwaway local progress.
- Reference: the real progress in Steam Cloud (backed up first). Any Mac-side Rep+ save smaller or older than that is test data, never upload material.

### Pre-read for the next blocker: OpenGL context rules in winemac.drv (Wine source, confirmed by E9)
- `winemac.drv/opengl.c` probes Legacy, then GL3_Core, then GL4_Core. `max_major.max_minor` becomes
  the highest core version (4.1 on Apple Silicon).
- `macdrv_context_create`: a version above max is rejected (`ERROR_INVALID_VERSION_ARB`). A request
  **without** `WGL_CONTEXT_CORE_PROFILE_BIT_ARB` gets no `kCGLPFAOpenGLProfile`, i.e. a **Legacy 2.1**
  context, even if 3.x was asked for. If Rep+/GLFW asks for a 3.x compatibility context, creation
  "succeeds" but GL 3 entry points or shaders then fail. That is the likely place for a Rep+-specific
  shim.
- `steam-traced-run.sh` now adds `+wgl` and summarises `macdrv_context_create` attrs, so the
  requested version/profile is read from the next run instead of guessed.

## Strategic risk: Rosetta 2 lifetime (Observed warning + Apple's published plan)
macOS shows a Rosetta deprecation notice for the Wine runtime (x86_64). This warning has appeared
since macOS 26.4. Apple's stated plan: full Rosetta through macOS 27. From macOS 28, only an
undefined "subset aimed at older unmaintained gaming titles". The whole D1 route depends on Rosetta.
**Mitigation:** keep the patch runtime-agnostic. The prefix, launcher and any Rep+ shims must not
depend on this specific Wine build, so the runtime can later be replaced (e.g. an ARM64-native Wine
with an x86 guest emulator such as FEX, if one matures on macOS, or whatever survives in the Rosetta
games subset). Re-evaluate before macOS 28.

## Known risks (hypotheses, untested)
0. **Windows Steam UI under Wine 11 (Observed):** the login window is black with
   `-cef-disable-gpu`/SwiftShader. Steam connects to the CM servers and waits for credentials, so
   only the presentation is broken. Known workarounds: CEF `--single-process`, or GPU web views on
   (`HKCU\Software\Valve\Steam\GPUAccelWebViewsV3=1`).
1. **Windows Steam client under Wine on macOS.** steamwebhelper (CEF) is the usual failure point.
2. **OpenGL profile.** macOS offers only legacy 2.1 or core 3.2–4.1, with no compatibility profile.
   If Rep+ mixes fixed-function calls with GL3+ features, it needs a shim. This is the most likely
   place a real Rep+-specific patch is needed.
3. Mapped-buffer copy fallback performance under WoW64.
4. Input: Rep+ has no static XInput/DInput imports. Gamepad path TBD.
5. EOS/network/multiplayer stack: needed for offline boot? TBD.

## Smallest next experiment (local Mac) — superseded by E8: run `steam-traced-run.sh --no-overlay`
1. Find the module at `0x7674xxxx`/`0x7773xxxx` in run 7's local log:
   `grep -i 'loaddll' analysis/logs/<run7 log> | grep -i -e 7674 -e 7773`. Alternatively match the
   load addresses in `+loaddll` against `+module`.
2. `repplus-patch/tests/build-devnotify.sh`, then `repplus-patch/tests/bisect-devnotify.sh
   repplus-patch/prefix/repplus` with Windows Steam running. This adds LAA, debugger hiding, the game
   DLLs and steam_api one at a time. The first case that crashes names the trigger. It writes
   nothing into the game directory.
3. If every case survives, the trigger is Rep+'s own code (TLS callback or the GLFW call order).
   Then look at `0x00AEFEC1` and GLFW's `createHelperWindow` in Ghidra.
- Fix policy: a Wine-side fix (patched `sechost`/`wow64cpu` built from source, or a Wine
  configuration change) or a launcher-level workaround is fine. Never patch `isaac-ng.exe`.
