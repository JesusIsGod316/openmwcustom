#!/usr/bin/env bash
# Caller selects the Vulkan ICD. This renders fixtures, not the game or saves.
set -euo pipefail
exe=${1:?usage: run-rendering-pixel-tests.sh PIXEL_EXE EVIDENCE_DIR}
out=${2:?evidence directory required}
mkdir -p "$out"
export OPENMW_V4_PIXEL_VALIDATION=1
unset OPENMW_V4_LEGACY_NORMAL_MAPPING_CONTROL OPENMW_V4_LEGACY_WATER_OPTICS_CONTROL
"$exe" all 2>&1 | tee "$out/pixels.log"
if grep -E 'VUID-|Validation Error|runtime error:|ERROR: AddressSanitizer' "$out/pixels.log"; then
    echo 'The pixel fixture returned with validation errors' >&2; exit 1
fi
set +e
OPENMW_V4_LEGACY_NORMAL_MAPPING_CONTROL=1 "$exe" normal > "$out/normal-control.log" 2>&1
normal=$?
OPENMW_V4_LEGACY_WATER_OPTICS_CONTROL=1 "$exe" water > "$out/water-control.log" 2>&1
water=$?
set -e
test "$normal" = 1 && test "$water" = 1
grep -F 'object RG normals did not reconstruct Z' "$out/normal-control.log"
grep -F 'native depth absorption did not distinguish shore and deep water' "$out/water-control.log"
if grep -E 'VUID-|Validation Error' "$out/normal-control.log" "$out/water-control.log"; then
    echo 'A comparison failed for a Vulkan API violation, not solely the expected pixel assertion' >&2; exit 1
fi
echo 'PASS repaired pixels; both old-path controls rejected by their target assertions'
