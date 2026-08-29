import os
import re
from pathlib import Path

ROOT = Path(os.environ.get("OPENMW_PATCH_ROOT", Path(__file__).resolve().parents[2])).resolve()
launcher = ROOT / "tools/v3/launchers/V3_Lab.ps1"
text = launcher.read_text(encoding="utf-8")

# Apply only after the complete V3.16 stack has finished mutating Modes 88/89.
for marker in ("v316-balanced-hitch", "v316-aggressive-hitch", "Enter 1 through 89"):
    if marker not in text:
        raise RuntimeError(f"V3.17 runtime-mode layer expected V3.16 launcher marker: {marker}")

# Later V3.16 layers may extend the human-readable Mode89 label while leaving
# its semantic experiment ID stable. Anchor the menu on the unique Mode89 label
# instead of requiring the original V3.16 wording byte-for-byte.
menu_lines = [line for line in text.splitlines() if line.startswith("Write-Host ' 89 = V3.16")]
if len(menu_lines) != 1:
    raise RuntimeError(f"V3.17 launcher menu anchor mismatch: found {len(menu_lines)} Mode89 label(s)")
old_menu = menu_lines[0]
new_menu = old_menu + "\n" + "\n".join(
    [
        "Write-Host ' 90 = V3.17 control: V3.16 Mode88 + stock LuaJIT'",
        "Write-Host ' 91 = V3.17 Rubic0n runtime attribution'",
        "Write-Host ' 92 = V3.17 engine Lua/materialization attribution + stock LuaJIT'",
        "Write-Host ' 93 = V3.17 combined balanced candidate'",
        "Write-Host ' 94 = V3.17 combined + aggressive SFX predecode'",
    ]
)
text = text.replace(old_menu, new_menu, 1)

text, n = re.subn(
    r"do \{ \$choice = Read-Host 'Enter 1 through 89' \} until \(\$choice -in @\(([^\n]+)\)\)",
    lambda m: "do { $choice = Read-Host 'Enter 1 through 94' } until ($choice -in @(" + m.group(1)
    + ",'90','91','92','93','94'))",
    text,
    count=1,
)
if n != 1:
    raise RuntimeError("V3.17 launcher choice-range anchor mismatch")

mode88_candidates = [line for line in text.splitlines() if line.lstrip().startswith("'88'")]
mode89_candidates = [line for line in text.splitlines() if line.lstrip().startswith("'89'")]
if len(mode88_candidates) != 1 or len(mode89_candidates) != 1:
    raise RuntimeError(
        f"V3.17 launcher mode anchor mismatch: mode88={len(mode88_candidates)} mode89={len(mode89_candidates)}"
    )
mode88 = mode88_candidates[0]
mode89 = mode89_candidates[0]
body88 = mode88[mode88.index("{") + 1 : mode88.rindex("}")].strip()
body89 = mode89[mode89.index("{") + 1 : mode89.rindex("}")].strip()

# Build attribution modes from the *final generated* V3.16 bodies so later
# V3.16 SFX-retention/frontload/idle-sweep additions are inherited exactly.
def relabel(body, old, new):
    if old not in body:
        raise RuntimeError(f"V3.17 could not relabel inherited mode {old}")
    return body.replace(old, new, 1)

insert_anchor = mode89 + "\n"
new_modes = (
    mode89 + "\n"
    + "        '90' { " + relabel(body88, "v316-balanced-hitch", "v317-stock-control")
    + "; $V317LuaRuntime = 'stock'; $V317EngineLuaOptimizations = 'false' }\n"
    + "        '91' { " + relabel(body88, "v316-balanced-hitch", "v317-rubicon-only")
    + "; $V317LuaRuntime = 'rubicon'; $V317EngineLuaOptimizations = 'false' }\n"
    + "        '92' { " + relabel(body88, "v316-balanced-hitch", "v317-engine-lua-only")
    + "; $V317LuaRuntime = 'stock'; $V317EngineLuaOptimizations = 'true' }\n"
    + "        '93' { " + relabel(body88, "v316-balanced-hitch", "v317-combined-balanced")
    + "; $V317LuaRuntime = 'rubicon'; $V317EngineLuaOptimizations = 'true' }\n"
    + "        '94' { " + relabel(body89, "v316-aggressive-hitch", "v317-combined-aggressive-sfx")
    + "; $V317LuaRuntime = 'rubicon'; $V317EngineLuaOptimizations = 'true' }\n"
)
if text.count(insert_anchor) != 1:
    raise RuntimeError("V3.17 Mode89 switch anchor mismatch")
text = text.replace(insert_anchor, new_modes, 1)

# Defaults keep normal/direct execution and every pre-V3.17 lab mode on stock
# LuaJIT with engine-side V3.17 Lua optimizations disabled.
default_anchor = "$V316IdleResourceSweep = 'false'\n$RendererProfiling"
if text.count(default_anchor) != 1:
    raise RuntimeError("V3.17 launcher defaults anchor mismatch")
text = text.replace(
    default_anchor,
    "$V316IdleResourceSweep = 'false'\n$V317LuaRuntime = 'stock'\n$V317EngineLuaOptimizations = 'false'\n$RendererProfiling",
    1,
)

# Add runtime identity to every test manifest. The actual DLL hash is calculated
# after selecting the staged runtime and appended immediately before launch.
manifest_anchor = '    "openmw_exe_sha256=$exeHash",\n    "game_dir=$GameDir"'
if text.count(manifest_anchor) != 1:
    raise RuntimeError("V3.17 TEST_MODE manifest anchor mismatch")
text = text.replace(
    manifest_anchor,
    '    "openmw_exe_sha256=$exeHash",\n    "v317_lua_runtime=$V317LuaRuntime",\n'
    '    "v317_engine_lua_optimizations=$V317EngineLuaOptimizations",\n    "game_dir=$GameDir"',
    1,
)

# Engine-side V3.17 optimization selection is inherited by the OpenMW process
# through one explicit environment variable. Pre-V3.17 modes clear it.
allvars_anchor = "foreach ($name in $allVars) { Remove-Item \"Env:$name\" -ErrorAction SilentlyContinue }"
if text.count(allvars_anchor) != 1:
    raise RuntimeError("V3.17 environment-clear anchor mismatch")
text = text.replace(
    allvars_anchor,
    allvars_anchor + "\nRemove-Item Env:OPENMW_V317_LUA_OPT -ErrorAction SilentlyContinue\n"
    "if ($V317EngineLuaOptimizations -eq 'true') { $env:OPENMW_V317_LUA_OPT = '1' }",
    1,
)

# Select the runtime before OpenMW starts. Both files are staged by the V3.17
# Windows packaging workflow. Root lua51.dll is restored to stock on exit so a
# normal launch outside the lab remains deterministic and conservative.
launch_anchor = "    $process = Start-Process -FilePath $Exe -WorkingDirectory $GameDir -PassThru"
runtime_block = r'''    $runtimeRoot = Join-Path $GameDir 'v317-runtime'
    $stockLua = Join-Path $runtimeRoot 'stock\lua51.dll'
    $rubiconLua = Join-Path $runtimeRoot 'rubicon\lua51.dll'
    $rootLua = Join-Path $GameDir 'lua51.dll'
    $selectedLua = if ($V317LuaRuntime -eq 'rubicon') { $rubiconLua } else { $stockLua }
    foreach ($requiredLua in @($stockLua, $selectedLua)) {
        if (-not (Test-Path -LiteralPath $requiredLua)) {
            throw "V3.17 runtime identity failure: missing $requiredLua"
        }
    }
    Copy-Item -LiteralPath $selectedLua -Destination $rootLua -Force
    $selectedLuaHash = (Get-FileHash -LiteralPath $selectedLua -Algorithm SHA256).Hash
    Add-Content -LiteralPath (Join-Path $ProfileDir 'TEST_MODE.txt') -Value "lua51_sha256=$selectedLuaHash" -Encoding Ascii
    if (Test-Path -LiteralPath (Join-Path $runtimeRoot 'V317-LUAJIT-RUNTIME.txt')) {
        Copy-Item -LiteralPath (Join-Path $runtimeRoot 'V317-LUAJIT-RUNTIME.txt') -Destination (Join-Path $ProfileDir 'V317-LUAJIT-RUNTIME.txt') -Force
    }
    $V317RuntimeSwapped = $true
    $process = Start-Process -FilePath $Exe -WorkingDirectory $GameDir -PassThru'''
if text.count(launch_anchor) != 1:
    raise RuntimeError("V3.17 Start-Process anchor mismatch")
text = text.replace(launch_anchor, runtime_block, 1)

# Define swap state before try/finally and restore stock even if the run throws.
try_anchor = "$changedSettings = $false\ntry {"
if text.count(try_anchor) != 1:
    raise RuntimeError("V3.17 launcher try anchor mismatch")
text = text.replace(try_anchor, "$changedSettings = $false\n$V317RuntimeSwapped = $false\ntry {", 1)

finally_anchor = "finally {\n    if (Test-Path -LiteralPath (Join-Path $UserOpenMW 'openmw.log')) {"
finally_replacement = r'''finally {
    Remove-Item Env:OPENMW_V317_LUA_OPT -ErrorAction SilentlyContinue
    if ($V317RuntimeSwapped) {
        try { Copy-Item -LiteralPath $stockLua -Destination $rootLua -Force } catch {
            Write-Warning "Unable to restore stock V3.17 lua51.dll: $($_.Exception.Message)"
        }
    }
    if (Test-Path -LiteralPath (Join-Path $UserOpenMW 'openmw.log')) {'''
if text.count(finally_anchor) != 1:
    raise RuntimeError("V3.17 launcher finally anchor mismatch")
text = text.replace(finally_anchor, finally_replacement, 1)

for required in (
    "90 = V3.17 control",
    "'91' {",
    "'94' {",
    "v317-rubicon-only",
    "v317-combined-balanced",
    "OPENMW_V317_LUA_OPT",
    "v317-runtime",
    "V317-LUAJIT-RUNTIME.txt",
    "Enter 1 through 94",
):
    if required not in text:
        raise RuntimeError(f"V3.17 generated launcher missing marker: {required}")

launcher.write_text(text, encoding="utf-8", newline="\n")

marker = ROOT / "V3.17-RUNTIME-LAYER.txt"
marker.write_text(
    "\n".join(
        [
            "V3.17 Lua/runtime hitch consolidation",
            "mode90=v3.16-mode88-stock-luajit-control",
            "mode91=rubicon-runtime-only",
            "mode92=engine-lua-only-stock-runtime",
            "mode93=combined-balanced",
            "mode94=combined-plus-v3.16-mode89-sfx-predecode",
            "rubicon_source=f3ee18afcc8c029dc7e13c8c69fe119dbcbc4c50",
            "content_lua=current-openmw-retained",
            "",
        ]
    ),
    encoding="utf-8",
    newline="\n",
)
print("V3.17 launcher runtime attribution modes 90-94 added")
