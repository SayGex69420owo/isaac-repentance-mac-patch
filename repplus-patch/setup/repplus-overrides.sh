#!/bin/bash
# Per-app Wine settings the Rep+ compatibility patch needs, applied only to
# isaac-ng.exe inside the isolated prefix's registry. Nothing in the Steam
# install, the game files or saves is touched; `remove` restores Wine defaults.
#
#   gameoverlayrenderer = disabled
#       Steam injects its overlay into the game. Under WoW64 on macOS its
#       NtFlushInstructionCache call enters wow64cpu's 64-bit syscall path in
#       32-bit mode and faults (DECISIONS.md E8). The overlay is an optional
#       convenience; ownership checks (SteamStub, steam_api) are unaffected.
#
# Usage: repplus-overrides.sh apply|ensure|remove|status
#   ensure  apply only if the prefix's registry file does not already have the settings
#           (reading the file is instant; starting Wine's reg tool takes seconds)
# Exit status: 0 when the settings are in place (apply/ensure/status) or removed (remove).
set -uo pipefail
. "$(dirname "$0")/../launch/common.sh"
appkey='HKCU\Software\Wine\AppDefaults\isaac-ng.exe\DllOverrides'
disabled_dlls=(gameoverlayrenderer)
# Overrides used by earlier experiments; `apply` and `remove` clear them.
experimental_dlls=(dinput8)

reg() { WINEDEBUG=-all "$wine" reg "$@" >/dev/null 2>&1; }
state() { reg query "$appkey" /v "$1" && echo disabled || echo default; }

# user.reg as Wine writes it: [Software\\Wine\\AppDefaults\\isaac-ng.exe\\DllOverrides] <time>
# followed by "name"="" lines. Case-insensitive, like the registry.
in_user_reg() {
    awk -v want="\"$1\"=\"\"" '
        /^\[/ { inkey = (tolower($0) ~ /^\[software\\\\wine\\\\appdefaults\\\\isaac-ng\.exe\\\\dlloverrides\]/) }
        inkey && tolower($0) == tolower(want) { found = 1 }
        END { exit !found }' "$WINEPREFIX/user.reg" 2>/dev/null
}
all_applied() {
    local d
    for d in "${disabled_dlls[@]}"; do in_user_reg "$d" || return 1; done
    for d in "${experimental_dlls[@]}"; do in_user_reg "$d" && return 1; done
    return 0
}

status=0
action="${1:-}"
if [[ "$action" == ensure ]]; then
    all_applied && { echo "  isaac-ng.exe settings already in place"; exit 0; }
    action=apply
fi
case "$action" in
    apply)
        for d in "${disabled_dlls[@]}"; do reg add "$appkey" /v "$d" /t REG_SZ /d "" /f; done
        for d in "${experimental_dlls[@]}"; do reg delete "$appkey" /v "$d" /f; done ;;
    remove)
        for d in "${disabled_dlls[@]}" "${experimental_dlls[@]}"; do reg delete "$appkey" /v "$d" /f; done ;;
    status) ;;
    *) echo "usage: $0 apply|ensure|remove|status" >&2; exit 1 ;;
esac
for d in "${disabled_dlls[@]}"; do
    s=$(state "$d")
    printf '  isaac-ng.exe  %-20s %s\n' "$d" "$s"
    [[ "$action" == remove && "$s" == disabled ]] && status=1
    [[ "$action" != remove && "$s" != disabled ]] && status=1
done
for d in "${experimental_dlls[@]}"; do
    s=$(state "$d")
    printf '  isaac-ng.exe  %-20s %s\n' "$d" "$s"
    [[ "$action" != status && "$s" == disabled ]] && status=1
done
exit $status
