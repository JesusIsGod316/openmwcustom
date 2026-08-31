import os
import re
from pathlib import Path

ROOT = Path(os.environ.get("OPENMW_PATCH_ROOT", Path(__file__).resolve().parents[2])).resolve()
launcher = ROOT / "tools/v3/launchers/V3_Lab.ps1"
text = launcher.read_text(encoding="utf-8")

menu120 = [line for line in text.splitlines() if line.startswith("Write-Host '120 = V3.20 CP3")]
if len(menu120) != 1:
    raise RuntimeError(f"V3.20 CP6 menu120 anchor mismatch: {len(menu120)}")
extra = "\n".join((
    "Write-Host '121 = V3.20 CP6 exact P0 stock-LuaJIT control'",
    "Write-Host '122 = V3.20 CP6 safe-JIT-only causal mode'",
    "Write-Host '123 = V3.20 CP6 combined stack with stock LuaJIT'",
    "Write-Host '124 = V3.20 CP6 combined stack with safe LuaJIT'",
))
text = text.replace(menu120[0], menu120[0] + "\n" + extra, 1)
text, count = re.subn(
    r"do \{ \$choice = Read-Host 'Enter 1 through 120' \} until \(\$choice -in @\(([^\n]+)\)\)",
    lambda match: "do { $choice = Read-Host 'Enter 1 through 124' } until ($choice -in @(" + match.group(1)
    + ",'121','122','123','124'))",
    text,
    count=1,
)
if count != 1:
    raise RuntimeError("V3.20 CP6 choice-range anchor mismatch")

line118 = next(line for line in text.splitlines() if line.lstrip().startswith("'118'"))
line120 = next(line for line in text.splitlines() if line.lstrip().startswith("'120'"))
control = line118[line118.index("{") + 1 : line118.rindex("}")].strip()
combined = line120[line120.index("{") + 1 : line120.rindex("}")].strip()

def runtime(body: str, experiment: str, lua_runtime: str, focus_cadence: str) -> str:
    body = re.sub(r"v320-cp[23]-[^']+", experiment, body, count=1)
    body = body.replace("$V317LuaRuntime = 'stock'", f"$V317LuaRuntime = '{lua_runtime}'", 1)
    body = body.replace("$V319FocusCadence = '1'", f"$V319FocusCadence = '{focus_cadence}'", 1)
    return body

new_lines = (
    "        '121' { " + runtime(control, "v320-cp6-p0-stock-control", "stock", "1") + " }",
    "        '122' { " + runtime(control, "v320-cp6-safejit-only", "safejit", "1") + " }",
    "        '123' { " + runtime(combined, "v320-cp6-combined-stock", "stock", "2") + " }",
    "        '124' { " + runtime(combined, "v320-cp6-combined-safejit", "safejit", "2") + " }",
)
text = text.replace(line120 + "\n", line120 + "\n" + "\n".join(new_lines) + "\n", 1)

old_runtime = r'''    $rubiconLua = Join-Path $runtimeRoot 'rubicon\lua51.dll'
    $rootLua = Join-Path $GameDir 'lua51.dll'
    $selectedLua = if ($V317LuaRuntime -eq 'rubicon') { $rubiconLua } else { $stockLua }'''
new_runtime = r'''    $rubiconLua = Join-Path $runtimeRoot 'rubicon\lua51.dll'
    $safeJitLua = Join-Path $runtimeRoot 'safejit\lua51.dll'
    $rootLua = Join-Path $GameDir 'lua51.dll'
    $selectedLua = if ($V317LuaRuntime -eq 'rubicon') { $rubiconLua }
        elseif ($V317LuaRuntime -eq 'safejit') { $safeJitLua }
        else { $stockLua }'''
if text.count(old_runtime) != 1:
    raise RuntimeError("V3.20 CP6 runtime-selection anchor mismatch")
text = text.replace(old_runtime, new_runtime, 1)

manifest_anchor = r'''    if (Test-Path -LiteralPath (Join-Path $runtimeRoot 'V317-LUAJIT-RUNTIME.txt')) {
        Copy-Item -LiteralPath (Join-Path $runtimeRoot 'V317-LUAJIT-RUNTIME.txt') -Destination (Join-Path $ProfileDir 'V317-LUAJIT-RUNTIME.txt') -Force
    }'''
manifest_replacement = manifest_anchor + r'''
    if (Test-Path -LiteralPath (Join-Path $runtimeRoot 'V320-SAFE-LUAJIT-RUNTIME.txt')) {
        Copy-Item -LiteralPath (Join-Path $runtimeRoot 'V320-SAFE-LUAJIT-RUNTIME.txt') -Destination (Join-Path $ProfileDir 'V320-SAFE-LUAJIT-RUNTIME.txt') -Force
    }'''
if text.count(manifest_anchor) != 1:
    raise RuntimeError("V3.20 CP6 runtime-manifest anchor mismatch")
text = text.replace(manifest_anchor, manifest_replacement, 1)

capability_anchor = "$choice -notin @('114','118')"
if text.count(capability_anchor) != 1:
    raise RuntimeError("V3.20 CP6 exact-control capability anchor mismatch")
text = text.replace(capability_anchor, "$choice -notin @('114','118','121')", 1)

for marker in (
    "Enter 1 through 124",
    "v320-cp6-p0-stock-control",
    "v320-cp6-safejit-only",
    "v320-cp6-combined-stock",
    "v320-cp6-combined-safejit",
    "safejit\\lua51.dll",
    "V320-SAFE-LUAJIT-RUNTIME.txt",
    "$choice -notin @('114','118','121')",
):
    if marker not in text:
        raise RuntimeError(f"V3.20 CP6 launcher missing marker: {marker}")

launcher.write_text(text, encoding="utf-8", newline="\n")
print("V3.20 CP6 stock/safe-JIT causal modes 121-124 added")
