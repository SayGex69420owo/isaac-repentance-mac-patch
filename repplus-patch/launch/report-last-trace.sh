#!/bin/bash
# Re-summarise the most recent Steam-traced log (no game launch) and copy the
# report to the clipboard. Usage: report-last-trace.sh [log]
. "$(dirname "$0")/common.sh"
log="${1:-$(ls -t "$root"/analysis/logs/steam-traced-*.log 2>/dev/null | head -1)}"
[[ -f "$log" ]] || { echo "no steam-traced log found" >&2; exit 1; }
mkdir -p "$reports"; out="$reports/steam-trace-report.txt"
{ echo "# report for $(basename "$log")"; python3 -I "$root/analysis/scripts/steam_trace_report.py" "$log"; } > "$out"
cat "$out"
command -v pbcopy >/dev/null && pbcopy < "$out" && echo && echo "Report copied to the clipboard."
