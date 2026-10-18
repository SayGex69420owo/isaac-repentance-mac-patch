#!/usr/bin/env python3
"""Attribute Wine +relay "Call ... ret=ADDR" records to the PE image/section that
issued the call. Return addresses inside isaac-ng.exe's .text prove that code from
the Rep+ binary executed. Usage: relay_provenance.py <relay.log> <isaac-ng.exe> <tid,...>"""
import re
import sys
from collections import Counter, defaultdict

import pefile

log_path, exe_path, tids = sys.argv[1], sys.argv[2], set(sys.argv[3].split(","))
pe = pefile.PE(exe_path, fast_load=True)
base = pe.OPTIONAL_HEADER.ImageBase
sections = [(s.Name.rstrip(b"\0").decode(), base + s.VirtualAddress,
             base + s.VirtualAddress + s.Misc_VirtualSize) for s in pe.sections]

image_end = base + pe.OPTIONAL_HEADER.SizeOfImage


def module_size(mod):
    """Mapped size: exact for isaac-ng.exe; for other modules bound by the next
    module base (good enough to separate modules from anonymous memory)."""
    if mod[1].lower() == "isaac-ng.exe":
        return image_end - base
    higher = [m[0] for m in modules if m[0] > mod[0]]
    return (min(higher) - mod[0]) if higher else 0x01000000


call_re = re.compile(r"^([0-9a-f]{4}):Call ([\w.\-]+)\((.*)\) ret=([0-9a-f]+)$")
load_re = re.compile(r'^([0-9a-f]{4}):trace:loaddll:\w+ Loaded L"([^"]+)" at ([0-9A-Fa-f]+): (\w+)')

modules = []          # (base, name, kind)
by_section = Counter()
first_in = {}
examples = defaultdict(list)
order = []
for line in open(log_path, errors="replace"):
    line = line.rstrip("\n")
    m = load_re.match(line)
    if m and m.group(1) in tids:
        modules.append((int(m.group(3), 16), m.group(2).split("\\")[-1], m.group(4)))
        continue
    m = call_re.match(line)
    if not m or m.group(1) not in tids:
        continue
    ret = int(m.group(4), 16)
    where = None
    for name, lo, hi in sections:
        if lo <= ret < hi:
            where = f"isaac-ng.exe:{name}"
            break
    if where is None:
        # Nearest loaded module below the address, but only if the address is
        # within that module's mapped size; otherwise it is anonymous memory
        # (e.g. code unpacked at runtime), labelled by its 64 KiB region.
        cands = [mod for mod in modules if mod[0] <= ret]
        mod = max(cands) if cands else None
        if mod and ret - mod[0] < module_size(mod):
            where = mod[1]
        else:
            where = f"anonymous@{ret & ~0xffff:#010x}"
    by_section[where] += 1
    if where.startswith(("isaac-ng.exe", "anonymous@")):
        fn = m.group(2)
        if len(examples[where]) < 40 and fn not in [e[1] for e in examples[where]]:
            examples[where].append((hex(ret), fn, m.group(3)[:90]))
        if where not in first_in:
            first_in[where] = (hex(ret), fn)
        order.append((where, fn))

print("isaac-ng.exe sections:")
for s in sections:
    print(f"  {s[0]:8} {s[1]:#010x}-{s[2]:#010x}")
print("\ncalls into Wine builtins, attributed by caller (return address):")
for k, v in by_section.most_common(25):
    print(f"  {v:8}  {k}")
for sec in sorted(examples):
    print(f"\nfirst distinct APIs called from {sec} (ret, api, args):")
    for e in examples[sec]:
        print("  ", *e)
# transitions between .bind and .text in call order
trans = []
prev = None
for where, fn in order:
    if where != prev:
        trans.append((where, fn))
        prev = where
print("\nexecution-region transitions (first API in each run):")
for t in trans[:30]:
    print("  ", *t)
