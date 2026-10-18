#!/usr/bin/env python3
"""Summarise a Steam-traced Wine log (steam-traced-run.sh) for the Rep+ game process.

The game is the last process that loaded isaac-ng.exe. Lines are expected as
'<timestamp>:<pid>:<tid>:<class>:<channel>:<function> ...' (WINEDEBUG +pid,+timestamp).
Usage: steam_trace_report.py <log> [first line number]
"""
import re
import sys
from collections import Counter

PRE = re.compile(r'^(?:[\d.]+:)?([0-9a-f]{4}):([0-9a-f]{4}):(.*)$')
NOISE = re.compile(r'dump_renderer|macdrv_init_pixel_formats|renderer_properties|'
                   r'RtlInitializeExtendedContext|RtlGetExtendedContextLength')


def strip(line):
    m = PRE.match(line)
    return f"{m.group(2)}:{m.group(3)}" if m else line


def main():
    start = int(sys.argv[2]) if len(sys.argv) > 2 else 1
    with open(sys.argv[1], errors="replace") as fh:
        lines = fh.read().splitlines()[start - 1:]
    pids = [m.group(1) for line in lines
            if 'isaac-ng.exe' in line and 'loaddll' in line and (m := PRE.match(line))]
    game = pids[-1] if pids else None
    print(f"game windows pid: {game or 'not found'} (isaac-ng.exe loads in pids {sorted(set(pids))})")
    if not game:
        return
    g = [line for line in lines if (m := PRE.match(line)) and m.group(1) == game]
    tids = Counter(PRE.match(line).group(2) for line in g)
    print(f"trace lines from the game process: {len(g)}, threads: {len(tids)}")

    print("\n## modules loaded by the game (in order; Wine system DLLs omitted)")
    for line in g:
        if 'Loaded L"' in line:
            name = line.split('Loaded L"', 1)[1].split('"', 1)[0]
            if not re.search(r'(?i)\\+windows\\+(system32|syswow64)\\+(?!\\)(?!opengl|winemac|xinput|dinput|hid)', name):
                print("  " + name.rsplit("\\", 1)[-1] + ("  (" + line.rsplit(': ', 1)[-1] + ")"))

    # Debug strings appear as 'warn:seh:OutputDebugString[AW] "..."' or, when raised directly,
    # only as 'warn:seh:dispatch_exception "..."'. Count each distinct text once per exception.
    ods = Counter()
    ods_order = []
    for line in g:
        m = re.search(r':warn:seh:dispatch_exception (L?"(.*)")\s*$', line)
        if not m:
            continue
        text = re.sub(r'\b0x[0-9a-fA-F]+\b|\b\d{3,}\b', '#', m.group(2).replace('\\n', ''))[:180]
        if text not in ods:
            ods_order.append(text)
        ods[text] += 1
    print(f"\n## debug-string messages from the game: {sum(ods.values())} total, {len(ods)} distinct "
          "(numbers/addresses folded to #, in order of first appearance)")
    mesa = [(n, t) for t, n in ods.items() if t.lower().startswith('mesa')]
    print(f"\n### Mesa messages ({sum(n for n, _ in mesa)} total, {len(mesa)} distinct, most frequent first)")
    for n, text in sorted(mesa, reverse=True)[:40]:
        print(f"  {n:7d}  {text}")
    print("\n### most frequent messages overall")
    for text, n in ods.most_common(15):
        print(f"  {n:7d}  {text}")
    game_msgs = [t for t in ods_order if not t.lower().startswith('mesa') and 'LOADED MOD' not in t]
    mods = [t for t in ods_order if 'LOADED MOD' in t]
    print(f"\n### game messages in order (mods loaded: {len(mods)}, listed separately below; first 50)")
    for text in game_msgs[:50]:
        print(f"  {ods[text]:7d}  {text}")
    if mods:
        print("\n### mods loaded")
        for text in mods[:60]:
            print("  " + text.split('/mods/', 1)[-1].split('/content', 1)[0])

    ex = [i for i, line in enumerate(g) if 'dispatch_exception code=' in line]
    real = [i for i in ex if not re.search(r'code=4001000[6a]', g[i])]
    codes = Counter(re.search(r'code=(\w+)', g[i]).group(1) for i in ex)
    print(f"\n## exceptions: {len(ex)} ({', '.join(f'{c} x{n}' for c, n in codes.most_common()) or 'none'})")
    for i in real[:3]:
        print("\n".join("  " + strip(x) for x in g[max(0, i - 2):i + 8]))
        print("  --")

    print("\n## OpenGL (wgl) calls from the game, capability dumps omitted (repeats collapsed)")
    seen = Counter()
    order = []
    for line in g:
        if ':wgl:' in line and not NOISE.search(line):
            key = re.sub(r'\b(0x)?[0-9a-fA-F]{8,16}\b', '#', strip(line).split(':', 1)[1])[:170]
            if key not in seen:
                order.append(key)
            seen[key] += 1
    for key in order[:60]:
        print(f"  {seen[key]:5d}  {key}")
    if len(order) > 60:
        print(f"  ... {len(order) - 60} more distinct wgl lines")

    # WINEDEBUG +opengl (steam-traced-run.sh --gltrace): every GL call the game makes into opengl32
    gl = [(m.group(1), m.group(2)) for line in g
          if (m := re.search(r':trace:opengl:(\w+)\s*(.*)$', line))]
    if gl:
        calls = Counter(name for name, _ in gl)
        print(f"\n## opengl32 calls from the game (+opengl): {len(gl)} lines, {len(calls)} functions")
        draws = {n: c for n, c in calls.items() if 'Draw' in n or 'draw' in n}
        print("  draw functions: " + (", ".join(f"{n} x{c}" for n, c in sorted(draws.items(), key=lambda x: -x[1])) or "NONE"))
        for name, _ in calls.most_common(40):
            print(f"  {calls[name]:8d}  {name}")
        for name in sorted(draws)[:6]:
            ex = [args for n, args in gl if n == name][:3]
            print(f"  examples {name}: " + " | ".join(a[:120] for a in ex))
        swaps = [i for i, (n, _) in enumerate(gl) if 'SwapBuffers' in n or n == 'wglSwapLayerBuffers']
        if len(swaps) >= 4:
            lo, hi = swaps[len(swaps) // 2], swaps[len(swaps) // 2 + 1]
            print(f"\n### one frame as the game issues it ({hi - lo} calls, repeats collapsed)")
            i, out = lo + 1, []
            while i <= hi:
                j = i
                while j <= hi and gl[j][0] == gl[i][0]:
                    j += 1
                out.append(gl[i][0] + (f" x{j - i}" if j - i > 1 else "") +
                           (f" ({gl[i][1][:60]})" if 'Draw' in gl[i][0] else ""))
                i = j
            for k in range(0, min(len(out), 400), 8):
                print("  " + ", ".join(out[k:k + 8]))
        else:
            print(f"  (only {len(swaps)} SwapBuffers calls traced)")

    # WINEDEBUG +key,+keyboard,+imm,+dinput,+rawinput (steam-traced-run.sh --keytrace)
    keyconv = Counter()
    for line in g:
        m = re.search(r'keycode (\d+) converted to vkey (0x[0-9A-Fa-f]+) scan (0x[0-9a-f]+)', line)
        if m:
            keyconv[(int(m.group(1)), m.group(2), m.group(3))] += 1
    funcs = Counter(f"{m.group(1)}:{m.group(2)}" for line in g
                    if (m := re.search(r':(?:trace|warn|fixme):(key|keyboard|imm|dinput|rawinput):(\w+)', line)))
    if keyconv or funcs:
        mac = {0: 'A', 1: 'S', 2: 'D', 13: 'W', 49: 'Space', 36: 'Return', 123: 'Left', 124: 'Right', 125: 'Down',
               126: 'Up', 53: 'Esc', 48: 'Tab', 12: 'Q', 14: 'E'}
        print(f"\n## keyboard (--keytrace): Mac keycode -> Windows vkey/scan, press+release events")
        for (kc, vk, sc), n in sorted(keyconv.items()):
            print(f"  keycode {kc:3d} ({mac.get(kc, '?'):6s}) -> vkey {vk} scan {sc}: {n}")
        print("  trace functions by channel (most frequent):")
        for f, n in funcs.most_common(30):
            print(f"  {n:8d}  {f}")
        shown = 0
        for line in g:
            if re.search(r':(key|imm|dinput|rawinput):', line) and not re.search(r'converted to vkey|win .* key (press|release)', line):
                print("  " + strip(line)[:200])
                shown += 1
                if shown >= 40:
                    break

    errs = Counter(strip(line).split(':', 1)[1][:140] for line in g if ':err:' in line or (':warn:' in line
                   and 'OutputDebugString' not in line and 'dispatch_exception' not in line))
    print("\n## err/warn messages from the game (most frequent)")
    for e, n in errs.most_common(20):
        print(f"  {n:5d}  {e}")

    glcore = [line.strip() for line in lines if line.startswith('[isaacgl]')]
    # a reused Steam log holds several game runs: keep the last active run only
    starts = [i for i, t in enumerate(glcore) if 'active=1' in t]
    if starts:
        glcore = glcore[starts[-1]:]
    print(f"\n## isaacgl.dylib messages ({len(glcore)}; only the active/upgrade lines matter)")
    shaders = [t for t in glcore if 'translated' in t]
    # split off the periodic "GL calls in the last 10 s" blocks
    blocks, rest, cur = [], [], None
    for t in glcore:
        if 'GL calls in the last 10 s' in t:
            cur = [t]
            blocks.append(cur)
        elif cur is not None and (t.startswith('[isaacgl]   ') and t.split()[1].isdigit() or '(no GL calls)' in t
                                  or 'shaders compiled' in t or 'draw-family calls' in t or 'exported draw calls' in t):
            cur.append(t)
        else:
            cur = None
            rest.append(t)
    trace = [t for t in rest if t.startswith('[isaacgl] trace|')]
    important = [t for t in rest if 'translated' not in t and 'loaded in pid' not in t and t not in trace]
    print(f"  shaders translated: {len(shaders)}, GL statistics blocks: {len(blocks)}")
    for text in [t for t in glcore if 'active=1' in t][:1] + important[:90]:
        print("  " + text[:200])
    for label, block in (("second", blocks[1] if len(blocks) > 1 else None), ("last", blocks[-1] if blocks else None)):
        if block:
            print(f"  --- {label} statistics block ---")
            for text in block[:32]:
                print("  " + text[:200])
    if trace:
        print(f"  --- calls of one frame, in order ({len(trace)} lines) ---")
        for text in trace[:220]:
            print("  " + text[len('[isaacgl] trace| '):][:400])
    if glcore and not any('active=1' in t for t in glcore):
        print("  (loaded, but never active in a Rep+ process)")

    first = next(i for i, line in enumerate(lines) if (m := PRE.match(line)) and m.group(1) == game)
    vk_list = re.compile(r'^\s*(VK_\w+ v\d+|The following \d+ Vulkan extensions are supported:)\s*$')
    other, order = Counter(), []
    for line in lines[first:]:
        text = line.strip()[:200]
        if not text or PRE.match(line) or vk_list.match(text) or text.startswith('[isaacgl]'):
            continue
        if text not in other:
            order.append(text)
        other[text] += 1
    print("\n## lines without a Wine prefix after the game started (Mesa/MoltenVK messages; in order,"
          " repeats counted, Vulkan extension lists omitted)")
    for text in order[:80]:
        print(f"  {other[text]:5d}  {text}")
    if len(order) > 80:
        print(f"  ... {len(order) - 80} more distinct lines")

    tail = [strip(line) for line in g if ':seh:' not in line and not NOISE.search(line)]
    print("\n## last 40 non-exception trace lines of the game process")
    for line in tail[-40:]:
        print("  " + line[:170])


if __name__ == "__main__":
    main()
