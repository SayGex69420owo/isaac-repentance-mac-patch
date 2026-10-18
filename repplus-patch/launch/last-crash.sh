#!/bin/bash
# Read-only: summarise the newest macOS crash report of a Wine process (the
# game's host process) from the last 3 hours: exception, crashed thread and
# its top frames with the library each one is in. Copies the result to the
# clipboard. Changes nothing.
# Usage: last-crash.sh
. "$(dirname "$0")/common.sh"
mkdir -p "$reports"; out="$reports/last-crash.txt"
dir="$HOME/Library/Logs/DiagnosticReports"
report=$(ls -t "$dir"/*.ips 2>/dev/null | grep -i -E '/(wine|wine64|wine-preloader|wine64-preloader|isaac)[^/]*\.ips$' | head -1)
if [[ -n "$report" ]] && (( $(python3 -I -c 'import os, sys, time; print(int(time.time() - os.path.getmtime(sys.argv[1])))' "$report") > 10800 )); then report=""; fi
{
    echo "# newest Wine crash report, $(date '+%Y-%m-%d %H:%M')"
    if [[ -z "$report" ]]; then
        echo "none in the last 3 hours in $dir (Wine/macOS did not record a crash; the game may have exited by itself)"
        ls -t "$dir" 2>/dev/null | head -5 | sed 's/^/  recent: /'
    else
        echo "file: $(basename "$report")"
        python3 -I - "$report" <<'PY'
import json, sys
text = open(sys.argv[1], errors="replace").read()
head, _, body = text.partition("\n")
try:
    h = json.loads(head)
    print("process:", h.get("app_name"), h.get("timestamp"), "os", h.get("os_version"))
except ValueError:
    pass
b = json.loads(body)
print("exception:", json.dumps(b.get("exception")))
print("termination:", json.dumps(b.get("termination"))[:300])
if b.get("asi"):
    print("asi:", json.dumps(b["asi"])[:400])
imgs = b.get("usedImages", [])
ft = b.get("faultingThread", 0)
threads = b.get("threads", [])
if ft < len(threads):
    t = threads[ft]
    print(f"crashed thread {ft}: {t.get('name', '')} {t.get('queue', '')}")
    for i, f in enumerate(t.get("frames", [])[:30]):
        img = imgs[f["imageIndex"]] if "imageIndex" in f and f["imageIndex"] < len(imgs) else {}
        name = img.get("name") or (img.get("path") or "?").rsplit("/", 1)[-1]
        print(f"  {i:2d} {name:28s} +0x{f.get('imageOffset', 0):x} {f.get('symbol', '')}+{f.get('symbolLocation', '')}")
    regs = t.get("threadState", {})
    if regs:
        print("registers:", ", ".join(f"{k}={v.get('value', v) if isinstance(v, dict) else v}" for k, v in list(regs.items())[:24])[:600])
PY
    fi
} > "$out" 2>&1
cat "$out"
command -v pbcopy >/dev/null && pbcopy < "$out" && echo && echo "Copied to the clipboard."
exit 0
