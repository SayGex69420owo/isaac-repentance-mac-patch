#!/bin/bash
# Build and run the isaacgl.c unit tests against the fake GL in this folder.
# Works on Linux and macOS (any C compiler; no Wine, Steam or game needed).
# Usage: repplus-patch/glcore/test/run-tests.sh
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
out="$here/build"
mkdir -p "$out"
# AddressSanitizer/UBSan with the first compiler that has their runtime (ISAACGL_TEST_NOSAN=1 to skip)
SAN=""; cc="${CC:-}"
if [[ -z "${ISAACGL_TEST_NOSAN:-}" ]]; then
    for c in ${CC:-clang gcc cc}; do
        if echo 'int main(void){return 0;}' | "$c" -x c -fsanitize=address,undefined -o "$out/san-probe" - 2>/dev/null; then
            cc="$c"; SAN="-fsanitize=address,undefined -fno-omit-frame-pointer"; break
        fi
    done
fi
[[ -n "$cc" ]] || cc="$(command -v clang || command -v cc)"
echo "compiler: $cc${SAN:+ with AddressSanitizer/UBSan}"
"$cc" -std=gnu11 -g -O1 -Wall -Wextra -Wno-unused-function -Wno-unused-parameter -Wno-deprecated-declarations \
    ${SAN:-} \
    -DISAACGL_TEST -isystem "$here/sdk" \
    -DISAACGL_SS_FIELD=shader_source_ARB -DISAACGL_VAP_FIELD=vertex_attrib_pointer_ARB \
    -DISAACGL_EVA_FIELD=enable_vertex_attrib_array_ARB -DISAACGL_DVA_FIELD=disable_vertex_attrib_array_ARB \
    -DISAACGL_TEX2D_FIELD=tex_image2D -DISAACGL_TEXSUB2D_FIELD=tex_sub_image2D \
    -DISAACGL_RBSTORAGE_FIELD=renderbuffer_storage_EXT -DISAACGL_DRE_FIELD=draw_range_elements \
    -DISAACGL_CS_FIELD=compile_shader_ARB -DISAACGL_LP_FIELD=link_program_ARB \
    -DISAACGL_GAL_FIELD=get_attrib_location_ARB -DISAACGL_GUL_FIELD=get_uniform_location_ARB \
    -DISAACGL_CFS_FIELD=check_framebuffer_status_EXT -DISAACGL_GRP_FIELD=get_renderbuffer_parameteriv_EXT \
    ${ISAACGL_TEST_CFLAGS:-} \
    -o "$out/test_isaacgl" "$here/test_isaacgl.c" -lpthread
ISAACGL_DEBUG=0 "$out/test_isaacgl"

# On Linux: also compile the real (non-test) source as a macOS x86_64 object, which checks the
# Mach-O-only parts (trampoline assembly, __interpose section). On a Mac, build-isaacgl.sh does that.
if [[ "$(uname)" != Darwin ]] && command -v clang >/dev/null; then
    slots=(-DISAACGL_DRE_FIELD=draw_range_elements -DISAACGL_TEX2D_FIELD=tex_image2D -DISAACGL_TEXSUB2D_FIELD=tex_sub_image2D
           -DISAACGL_RBSTORAGE_FIELD=renderbuffer_storage_EXT -DISAACGL_CS_FIELD=compile_shader_ARB -DISAACGL_LP_FIELD=link_program_ARB
           -DISAACGL_GAL_FIELD=get_attrib_location_ARB -DISAACGL_GUL_FIELD=get_uniform_location_ARB
           -DISAACGL_CFS_FIELD=check_framebuffer_status_EXT -DISAACGL_GRP_FIELD=get_renderbuffer_parameteriv_EXT)
    clang -E -P -isystem "$here/sdk" "${slots[@]}" "$here/../isaacgl.c" -o "$out/isaacgl-macos.i"
    clang -target x86_64-apple-macos11 -x c -c -O2 -Wall -Wno-unused-function -Wno-deprecated-declarations \
        -o "$out/isaacgl-macos.o" "$out/isaacgl-macos.i"
    echo "macOS x86_64 object: OK"
fi
