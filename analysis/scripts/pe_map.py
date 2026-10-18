#!/usr/bin/env python3
"""Static structural map of a PE file (headers, sections, imports, exports, TLS,
relocations, load config, debug info). Read-only; does not unpack or modify
anything. Usage: pe_map.py <pe-file> <out.json>"""
import hashlib
import json
import math
import sys
from collections import Counter

import pefile


def entropy(data: bytes) -> float:
    if not data:
        return 0.0
    counts = Counter(data)
    n = len(data)
    return -sum(c / n * math.log2(c / n) for c in counts.values())


def main(path, out):
    raw = open(path, "rb").read()
    pe = pefile.PE(data=raw, fast_load=False)
    oh = pe.OPTIONAL_HEADER
    fh = pe.FILE_HEADER
    res = {
        "file": path,
        "size": len(raw),
        "sha256": hashlib.sha256(raw).hexdigest(),
        "machine": hex(fh.Machine),
        "timestamp": fh.TimeDateStamp,
        "characteristics": hex(fh.Characteristics),
        "image_base": hex(oh.ImageBase),
        "entry_rva": hex(oh.AddressOfEntryPoint),
        "entry_va": hex(oh.ImageBase + oh.AddressOfEntryPoint),
        "subsystem": oh.Subsystem,
        "subsystem_version": f"{oh.MajorSubsystemVersion}.{oh.MinorSubsystemVersion}",
        "linker": f"{oh.MajorLinkerVersion}.{oh.MinorLinkerVersion}",
        "dll_characteristics": hex(oh.DllCharacteristics),
        "size_of_image": hex(oh.SizeOfImage),
        "stack_reserve": hex(oh.SizeOfStackReserve),
        "heap_reserve": hex(oh.SizeOfHeapReserve),
        "checksum": hex(oh.CheckSum),
        "data_directories": {
            d.name: {"rva": hex(d.VirtualAddress), "size": hex(d.Size)}
            for d in oh.DATA_DIRECTORY if d.Size
        },
        "sections": [],
        "imports": {},
        "delay_imports": {},
        "exports": None,
        "tls": None,
        "load_config": None,
        "debug": [],
        "relocations": None,
    }
    for s in pe.sections:
        data = s.get_data()
        res["sections"].append({
            "name": s.Name.rstrip(b"\0").decode(errors="replace"),
            "va": hex(oh.ImageBase + s.VirtualAddress),
            "vsize": hex(s.Misc_VirtualSize),
            "raw_off": hex(s.PointerToRawData),
            "raw_size": hex(s.SizeOfRawData),
            "characteristics": hex(s.Characteristics),
            "entropy": round(entropy(data), 4),
        })
    for entry in getattr(pe, "DIRECTORY_ENTRY_IMPORT", []):
        res["imports"][entry.dll.decode()] = [
            (i.name.decode() if i.name else f"#{i.ordinal}") for i in entry.imports
        ]
    for entry in getattr(pe, "DIRECTORY_ENTRY_DELAY_IMPORT", []):
        res["delay_imports"][entry.dll.decode()] = [
            (i.name.decode() if i.name else f"#{i.ordinal}") for i in entry.imports
        ]
    if hasattr(pe, "DIRECTORY_ENTRY_EXPORT"):
        exp = pe.DIRECTORY_ENTRY_EXPORT
        names = [s.name.decode() for s in exp.symbols if s.name]
        prefixes = Counter(n.split("_")[0] if "_" in n else n for n in names)
        res["exports"] = {
            "dll_name": exp.name.decode() if exp.name else None,
            "count": len(exp.symbols),
            "named": len(names),
            "prefix_histogram": prefixes.most_common(20),
            "non_epoxy": [n for n in names if not n.startswith("epoxy_")],
        }
    if hasattr(pe, "DIRECTORY_ENTRY_TLS"):
        t = pe.DIRECTORY_ENTRY_TLS.struct
        callbacks = []
        if t.AddressOfCallBacks:
            rva = t.AddressOfCallBacks - oh.ImageBase
            for i in range(32):
                cb = pe.get_dword_at_rva(rva + 4 * i)
                if not cb:
                    break
                callbacks.append(hex(cb))
        res["tls"] = {
            "start": hex(t.StartAddressOfRawData),
            "end": hex(t.EndAddressOfRawData),
            "index_addr": hex(t.AddressOfIndex),
            "callbacks_addr": hex(t.AddressOfCallBacks),
            "callbacks": callbacks,
        }
    if hasattr(pe, "DIRECTORY_ENTRY_LOAD_CONFIG"):
        lc = pe.DIRECTORY_ENTRY_LOAD_CONFIG.struct
        res["load_config"] = {
            k: (hex(v) if isinstance(v, int) else str(v))
            for k, v in lc.dump_dict().items()
            if k != "Structure" and isinstance(v, dict) and "Value" in v
            for v in [v["Value"]]
        }
    for d in getattr(pe, "DIRECTORY_ENTRY_DEBUG", []):
        item = {"type": d.struct.Type, "size": d.struct.SizeOfData}
        if d.entry is not None and hasattr(d.entry, "PdbFileName"):
            item["pdb"] = d.entry.PdbFileName.rstrip(b"\0").decode(errors="replace")
        res["debug"].append(item)
    if hasattr(pe, "DIRECTORY_ENTRY_BASERELOC"):
        blocks = pe.DIRECTORY_ENTRY_BASERELOC
        per_section = Counter()
        total = 0
        for b in blocks:
            for e in b.entries:
                if e.type == 0:
                    continue
                total += 1
                sec = pe.get_section_by_rva(e.rva)
                per_section[sec.Name.rstrip(b"\0").decode() if sec else "?"] += 1
        res["relocations"] = {"blocks": len(blocks), "entries": total,
                              "per_section": dict(per_section)}
    # Does the entry point fall inside .bind?
    ep_sec = pe.get_section_by_rva(oh.AddressOfEntryPoint)
    res["entry_section"] = ep_sec.Name.rstrip(b"\0").decode() if ep_sec else None
    json.dump(res, open(out, "w"), indent=1)
    print(json.dumps({k: res[k] for k in ("machine", "image_base", "entry_va",
                                          "entry_section", "subsystem", "linker",
                                          "dll_characteristics")}, indent=1))
    for s in res["sections"]:
        print(f"  {s['name']:8} va={s['va']} vsize={s['vsize']} ent={s['entropy']} ch={s['characteristics']}")
    print("imports:", {k: len(v) for k, v in res["imports"].items()})
    print("delay_imports:", {k: len(v) for k, v in res["delay_imports"].items()})
    print("exports:", res["exports"] and {k: res["exports"][k] for k in ("dll_name", "count", "named")},
          "non-epoxy:", res["exports"] and res["exports"]["non_epoxy"][:40])
    print("tls:", res["tls"])
    print("relocs:", res["relocations"])
    print("debug:", res["debug"])
    lc = res["load_config"] or {}
    print("load_config:", {k: lc.get(k) for k in ("SecurityCookie", "SEHandlerTable", "SEHandlerCount",
                                                    "GuardFlags", "GuardCFCheckFunctionPointer")})


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
