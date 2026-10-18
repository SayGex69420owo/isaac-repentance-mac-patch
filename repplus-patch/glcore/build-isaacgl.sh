#!/bin/bash
# Build isaacgl.dylib (x86_64, runs under Rosetta inside Wine's host process).
# Output: repplus-patch/glcore/build/isaacgl.dylib (local only, never committed).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$here/build"
# Exact name of the glShaderSource slot in the SDK's GL dispatch table.
hdr="$(xcrun --show-sdk-path)/System/Library/Frameworks/OpenGL.framework/Headers/gliDispatch.h"
slot() {  # first dispatch member matching the pattern
    local f
    f=$(grep -oE "\(\*$1\)" "$hdr" 2>/dev/null | head -1 | tr -d '(*)' || true)
    [[ -n "$f" ]] || { echo "could not find dispatch slot '$1' in $hdr" >&2; grep -n -i "${1%%[*(]*}" "$hdr" | head >&2; exit 1; }
    echo "$f"
}
ss=$(slot 'shader_source[A-Za-z_]*') || exit 1
vap=$(slot 'vertex_attrib_pointer(_ARB)?') || exit 1
eva=$(slot 'enable_vertex_attrib_array(_ARB)?') || exit 1
dva=$(slot 'disable_vertex_attrib_array(_ARB)?') || exit 1
slot 'draw_arrays' >/dev/null && slot 'draw_elements' >/dev/null && slot 'viewport' >/dev/null || exit 1
# optional slots: hooked only if the SDK has them
opt=()
for pair in TEX2D:'tex_image_?2D' TEXSUB2D:'tex_sub_image_?2D' RBSTORAGE:'renderbuffer_storage(_EXT)?' DRE:'draw_range_elements(_EXT)?' \
            CS:'compile_shader(_ARB)?' LP:'link_program(_ARB)?' GAL:'get_attrib_location(_ARB)?' GUL:'get_uniform_location(_ARB)?' \
            CFS:'check_framebuffer_status(_EXT)?' GRP:'get_renderbuffer_parameteriv(_EXT)?'; do
    f=$(grep -oE "\(\*${pair#*:}\)" "$hdr" 2>/dev/null | head -1 | tr -d '(*)' || true)
    if [[ -n "$f" ]]; then opt+=("-DISAACGL_${pair%%:*}_FIELD=$f"); else echo "optional slot ${pair#*:} not in SDK (skipped)"; fi
done
# slot names in struct order, for the call statistics
names="$here/build/slot_names.h"
{ echo "static const char *const slot_names[] = {"
  grep -oE '\(\*[A-Za-z0-9_]+\)' "$hdr" | tr -d '(*)' | sed 's/.*/    "&",/'
  echo "};"; } > "$names"
opt+=("-DISAACGL_SLOT_NAMES=\"$names\"")
echo "slot names: $(grep -c '",' "$names")"
echo "dispatch slots: $ss $vap $eva $dva draw_arrays draw_elements ${opt[*]+${opt[*]}}"
xcrun clang ${opt[@]+"${opt[@]}"} -DISAACGL_SS_FIELD="$ss" -DISAACGL_VAP_FIELD="$vap" -DISAACGL_EVA_FIELD="$eva" -DISAACGL_DVA_FIELD="$dva" -arch x86_64 -dynamiclib -O2 -Wall -Wextra -Wno-deprecated-declarations -Wno-unused-function \
    -mmacosx-version-min=11.0 -framework OpenGL \
    -o "$here/build/isaacgl.dylib.new" "$here/isaacgl.c"
# replaced by a rename: a game still running keeps the old file, a failed build keeps the old dylib
mv -f "$here/build/isaacgl.dylib.new" "$here/build/isaacgl.dylib"
echo "built $here/build/isaacgl.dylib ($(lipo -archs "$here/build/isaacgl.dylib"))"
