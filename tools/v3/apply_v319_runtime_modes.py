import os
import re
from pathlib import Path

ROOT = Path(os.environ.get("OPENMW_PATCH_ROOT", Path(__file__).resolve().parents[2])).resolve()
launcher = ROOT / "tools/v3/launchers/V3_Lab.ps1"
text = launcher.read_text(encoding="utf-8")

for marker in ("v318-nis-77", "Enter 1 through 101", "95 = V3.18 native-resolution control"):
    if marker not in text:
        raise RuntimeError(f"V3.19 runtime layer expected V3.18 launcher marker: {marker}")

menu101 = [line for line in text.splitlines() if line.startswith("Write-Host '101 = V3.18")]
if len(menu101) != 1:
    raise RuntimeError(f"V3.19 menu101 anchor mismatch: {len(menu101)}")
old_menu = menu101[0]
extra_menu = "\n".join([
    "Write-Host '102 = V3.19 CPU control: native + OSG auto + focus every frame'",
    "Write-Host '103 = V3.19 focus temporal coherence: refresh every 2 frames'",
    "Write-Host '104 = V3.19 focus temporal coherence aggressive: refresh every 3 frames'",
    "Write-Host '105 = V3.19 OSG CullDrawThreadPerContext + focus every frame'",
    "Write-Host '106 = V3.19 OSG CullThreadPerCameraDrawThreadPerContext + focus every frame'",
    "Write-Host '107 = V3.19 CullDrawThreadPerContext + focus every 2 frames'",
    "Write-Host '108 = V3.19 per-camera cull/draw + focus every 2 frames'",
])
text = text.replace(old_menu, old_menu + "\n" + extra_menu, 1)

text, n = re.subn(
    r"do \{ \$choice = Read-Host 'Enter 1 through 101' \} until \(\$choice -in @\(([^\n]+)\)\)",
    lambda m: "do { $choice = Read-Host 'Enter 1 through 108' } until ($choice -in @(" + m.group(1)
    + ",'102','103','104','105','106','107','108'))",
    text,
    count=1,
)
if n != 1:
    raise RuntimeError("V3.19 choice-range anchor mismatch")

default_anchor = "$V318RenderScaleManaged = 'false'"
if text.count(default_anchor) != 1:
    raise RuntimeError("V3.19 defaults anchor mismatch")
text = text.replace(default_anchor,
    default_anchor + "\n$V319FocusCadence = '1'\n$V319OsgThreading = ''", 1)

line95 = next((line for line in text.splitlines() if line.lstrip().startswith("'95'")), None)
line101 = next((line for line in text.splitlines() if line.lstrip().startswith("'101'")), None)
if not line95 or not line101:
    raise RuntimeError("V3.19 mode switch anchors missing")
base_body = line95[line95.index("{") + 1:line95.rindex("}")].strip()
if "v318-native-control" not in base_body:
    raise RuntimeError("V3.19 expected native control body")

def mode_body(exp, cadence="1", osg=""):
    body = base_body.replace("v318-native-control", exp, 1)
    return body + f"; $V319FocusCadence = '{cadence}'; $V319OsgThreading = '{osg}'"

new_lines = [
    "        '102' { " + mode_body("v319-cpu-control") + " }",
    "        '103' { " + mode_body("v319-focus2", "2") + " }",
    "        '104' { " + mode_body("v319-focus3", "3") + " }",
    "        '105' { " + mode_body("v319-osg-culldraw", "1", "CullDrawThreadPerContext") + " }",
    "        '106' { " + mode_body("v319-osg-percamera", "1", "CullThreadPerCameraDrawThreadPerContext") + " }",
    "        '107' { " + mode_body("v319-osg-culldraw-focus2", "2", "CullDrawThreadPerContext") + " }",
    "        '108' { " + mode_body("v319-osg-percamera-focus2", "2", "CullThreadPerCameraDrawThreadPerContext") + " }",
]
anchor = line101 + "\n"
if text.count(anchor) != 1:
    raise RuntimeError("V3.19 mode insertion anchor mismatch")
text = text.replace(anchor, line101 + "\n" + "\n".join(new_lines) + "\n", 1)

manifest_anchor = '    "v318_upscaler_sharpness=$V318UpscalerSharpness",'
if text.count(manifest_anchor) != 1:
    raise RuntimeError("V3.19 manifest anchor mismatch")
text = text.replace(manifest_anchor, manifest_anchor
    + '\n    "v319_focus_cadence=$V319FocusCadence",'
    + '\n    "v319_osg_threading=$V319OsgThreading",', 1)

# V3.17's stock runtime stash is generated packaging state rather than an engine
# dependency.  Some later Windows artifacts retain the verified stock lua51.dll
# at the build root but omit v317-runtime/stock/lua51.dll.  Reconstruct that
# stash only for stock-Lua modes and only when the root DLL has the exact known
# V3.17 stock hash.  Rubicon remains fail-closed if its separate runtime is absent.
bootstrap_anchor = "    $selectedLua = if ($V317LuaRuntime -eq 'rubicon') { $rubiconLua } else { $stockLua }"
if text.count(bootstrap_anchor) != 1:
    raise RuntimeError("V3.19 stock Lua bootstrap anchor mismatch")
bootstrap_block = r'''
    $V319StockLuaSha256 = 'A8636655927F70BAD350ED60E0F369992B32259EC8D2FD5D350E1A9A9811AE8B'
    if ($V317LuaRuntime -eq 'stock' -and -not (Test-Path -LiteralPath $stockLua)) {
        if (-not (Test-Path -LiteralPath $rootLua)) {
            throw "V3.19 stock Lua bootstrap failure: missing both $stockLua and $rootLua"
        }
        $rootLuaHash = (Get-FileHash -LiteralPath $rootLua -Algorithm SHA256).Hash
        if ($rootLuaHash -ne $V319StockLuaSha256) {
            throw "V3.19 stock Lua bootstrap failure: root lua51.dll hash mismatch ($rootLuaHash)"
        }
        New-Item -ItemType Directory -Path (Split-Path -Parent $stockLua) -Force | Out-Null
        Copy-Item -LiteralPath $rootLua -Destination $stockLua -Force
        Write-Host 'V3.19: bootstrapped verified stock LuaJIT runtime from packaged root lua51.dll.' -ForegroundColor DarkGray
    }
'''
text = text.replace(bootstrap_anchor, bootstrap_anchor + bootstrap_block, 1)

launch_anchor = "    $process = Start-Process -FilePath $Exe -WorkingDirectory $GameDir -PassThru"
if text.count(launch_anchor) != 1:
    raise RuntimeError("V3.19 Start-Process anchor mismatch")
env_block = r'''    $env:OPENMW_V319_FOCUS_CADENCE = $V319FocusCadence
    if ([string]::IsNullOrWhiteSpace($V319OsgThreading)) {
        Remove-Item Env:OSG_THREADING -ErrorAction SilentlyContinue
    }
    else {
        $env:OSG_THREADING = $V319OsgThreading
    }
'''
text = text.replace(launch_anchor, env_block + launch_anchor, 1)

for required in (
    "Enter 1 through 108",
    "v319-cpu-control",
    "v319-focus2",
    "v319-focus3",
    "CullDrawThreadPerContext",
    "CullThreadPerCameraDrawThreadPerContext",
    "v319_focus_cadence=$V319FocusCadence",
    "OPENMW_V319_FOCUS_CADENCE",
    "A8636655927F70BAD350ED60E0F369992B32259EC8D2FD5D350E1A9A9811AE8B",
    "V3.19 stock Lua bootstrap failure",
    "bootstrapped verified stock LuaJIT runtime",
):
    if required not in text:
        raise RuntimeError(f"V3.19 launcher missing marker: {required}")

# Ordering is a safety property: verification/bootstrap must occur before the
# inherited V3.17 missing-runtime loop and before any root DLL replacement.
if text.index("$V319StockLuaSha256") > text.index("foreach ($requiredLua in @($stockLua, $selectedLua))"):
    raise RuntimeError("V3.19 stock Lua bootstrap occurs after runtime identity check")
if text.index("$rootLuaHash = (Get-FileHash") > text.index("Copy-Item -LiteralPath $rootLua -Destination $stockLua -Force"):
    raise RuntimeError("V3.19 stock Lua copy occurs before root hash verification")

launcher.write_text(text, encoding="utf-8", newline="\n")
print("V3.19 CPU causal modes 102-108 and verified stock-Lua bootstrap added")
