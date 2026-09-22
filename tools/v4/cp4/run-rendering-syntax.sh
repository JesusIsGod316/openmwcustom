#!/usr/bin/env bash
# Supplemental actual-TU header/type checks; full Windows linking remains required.
set -euo pipefail
sdk=${1:?verified renderer SDK directory required}
root=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$root"
flags=(-std=c++20 -fsyntax-only -pthread -I. -Iextern/sol_config -Iextern/sol3 -Iextern/oics
    -isystem "$sdk/sysroot/usr/include" -isystem "$sdk/sysroot/usr/include/x86_64-linux-gnu"
    -isystem "$sdk/sysroot/usr/include/bullet" -isystem "$sdk/sysroot/usr/include/recastnavigation"
    -isystem "$sdk/sysroot/usr/include/luajit-2.1" -isystem "$sdk/mygui/MyGUIEngine/include"
    -isystem "$sdk/SDL3-3.4.10/include" -isystem "$sdk/prefix/include")
if [[ -n ${CP4F_OSG_ROOT:-} ]]; then flags+=(-isystem "$CP4F_OSG_ROOT/include"); fi
for source in apps/openmw/mwrender/{characterpreview,sky,v4engineframecoordinator,v4localmapbridge,v4previewbridge,v4skycapture}.cpp \
    components/render/backend/vsg/{legacymaterialshader,offscreenrendertarget,staticassetrealizer,uipipeline,vsgruntimehost,watersurface,nativesky,vsgsemanticsession}.cpp \
    components/vsgmygui/{rendermanager,texture}.cpp apps/openmw/engine.cpp; do
    echo "Vulkan syntax: $source"
    "${CXX:-c++}" "${flags[@]}" -DOPENMW_ENABLE_V4_VULKAN_RUNTIME=1 "$source"
done
for source in apps/openmw/mwrender/{characterpreview,sky}.cpp apps/openmw/engine.cpp; do
    echo "OpenGL syntax: $source"
    "${CXX:-c++}" "${flags[@]}" "$source"
done
echo 'PASS actual translation-unit syntax in 20 configurations; not a full-engine link'
