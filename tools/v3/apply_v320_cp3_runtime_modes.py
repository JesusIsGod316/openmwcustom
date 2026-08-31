import os
import re
from pathlib import Path

ROOT = Path(os.environ.get("OPENMW_PATCH_ROOT", Path(__file__).resolve().parents[2])).resolve()
launcher = ROOT / "tools/v3/launchers/V3_Lab.ps1"
text = launcher.read_text(encoding="utf-8")

menu117 = [line for line in text.splitlines() if line.startswith("Write-Host '117 = V3.20 CP2")]
if len(menu117) != 1:
    raise RuntimeError(f"V3.20 CP3 menu117 anchor mismatch: {len(menu117)}")
extra = "\n".join((
    "Write-Host '118 = V3.20 CP3 exact P0 sound-query control'",
    "Write-Host '119 = V3.20 CP3 same-frame sound-query coalescing only'",
    "Write-Host '120 = V3.20 CP3 CP2-combined + sound-query coalescing'",
))
text = text.replace(menu117[0], menu117[0] + "\n" + extra, 1)
text, count = re.subn(
    r"do \{ \$choice = Read-Host 'Enter 1 through 117' \} until \(\$choice -in @\(([^\n]+)\)\)",
    lambda match: "do { $choice = Read-Host 'Enter 1 through 120' } until ($choice -in @(" + match.group(1)
    + ",'118','119','120'))",
    text,
    count=1,
)
if count != 1:
    raise RuntimeError("V3.20 CP3 choice-range anchor mismatch")

default_anchor = """$V320FocusAdaptive = '0'
$V320EngineLuaFastPaths = '0'
$V320SoundConversionCache = '0'"""
if text.count(default_anchor) != 1:
    raise RuntimeError("V3.20 CP3 default anchor mismatch")
text = text.replace(default_anchor, default_anchor + "\n$V320SoundQueryCoalescing = '0'", 1)

line117 = next(line for line in text.splitlines() if line.lstrip().startswith("'117'"))
line114 = next(line for line in text.splitlines() if line.lstrip().startswith("'114'"))
control = line114[line114.index("{") + 1 : line114.rindex("}")].strip()
combined = line117[line117.index("{") + 1 : line117.rindex("}")].strip()
new_lines = (
    "        '118' { " + control.replace("v320-cp2-p0-control", "v320-cp3-p0-control", 1)
    + "; $V320SoundQueryCoalescing = '0' }",
    "        '119' { " + control.replace("v320-cp2-p0-control", "v320-cp3-sound-query", 1)
    + "; $V320SoundQueryCoalescing = '1' }",
    "        '120' { " + combined.replace("v320-cp2-combined", "v320-cp3-combined", 1)
    + "; $V320SoundQueryCoalescing = '1' }",
)
text = text.replace(line117 + "\n", line117 + "\n" + "\n".join(new_lines) + "\n", 1)

manifest_anchor = '    "v320_sound_conversion_cache=$V320SoundConversionCache",'
text = text.replace(manifest_anchor, manifest_anchor + '\n    "v320_sound_query_coalescing=$V320SoundQueryCoalescing",', 1)
launch_anchor = "    $env:OPENMW_V320_SOUND_CONVERSION_CACHE = $V320SoundConversionCache"
text = text.replace(launch_anchor, launch_anchor + "\n    $env:OPENMW_V320_SOUND_QUERY_COALESCING = $V320SoundQueryCoalescing", 1)
finally_anchor = "finally {\n    Remove-Item Env:OPENMW_V320_ENGINE_LUA_FASTPATHS"
text = text.replace(
    finally_anchor,
    "finally {\n    Remove-Item Env:OPENMW_V320_SOUND_QUERY_COALESCING -ErrorAction SilentlyContinue\n"
    "    Remove-Item Env:OPENMW_V320_ENGINE_LUA_FASTPATHS",
    1,
)

for marker in ("Enter 1 through 120", "v320-cp3-p0-control", "v320-cp3-sound-query", "v320-cp3-combined",
               "OPENMW_V320_SOUND_QUERY_COALESCING"):
    if marker not in text:
        raise RuntimeError(f"V3.20 CP3 launcher missing marker: {marker}")
launcher.write_text(text, encoding="utf-8", newline="\n")
print("V3.20 CP3 causal modes 118-120 added")
