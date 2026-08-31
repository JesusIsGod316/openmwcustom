import os
import re
from pathlib import Path

ROOT = Path(os.environ.get("OPENMW_PATCH_ROOT", Path(__file__).resolve().parents[2])).resolve()
launcher = ROOT / "tools/v3/launchers/V3_Lab.ps1"
text = launcher.read_text(encoding="utf-8")

menu108 = [line for line in text.splitlines() if line.startswith("Write-Host '108 = V3.19")]
if len(menu108) != 1:
    raise RuntimeError(f"V3.20 menu108 anchor mismatch: {len(menu108)}")
extra_menu = "\n".join(
    (
        "Write-Host '109 = V3.20 CP1 exact P0/off fallback: focus every frame'",
        "Write-Host '110 = V3.20 CP1 promoted fixed focus cadence 2'",
        "Write-Host '111 = V3.20 CP1 aggressive fixed focus cadence 3'",
        "Write-Host '112 = V3.20 CP1 adaptive camera-dirty focus, cadence bound 2'",
        "Write-Host '113 = V3.20 CP1 adaptive camera-dirty focus, cadence bound 3'",
    )
)
text = text.replace(menu108[0], menu108[0] + "\n" + extra_menu, 1)

text, count = re.subn(
    r"do \{ \$choice = Read-Host 'Enter 1 through 108' \} until \(\$choice -in @\(([^\n]+)\)\)",
    lambda match: "do { $choice = Read-Host 'Enter 1 through 113' } until ($choice -in @(" + match.group(1)
    + ",'109','110','111','112','113'))",
    text,
    count=1,
)
if count != 1:
    raise RuntimeError("V3.20 choice-range anchor mismatch")

default_anchor = """$V318RenderScaleManaged = 'false'
$V319FocusCadence = '1'
$V319OsgThreading = ''"""
if text.count(default_anchor) != 1:
    raise RuntimeError("V3.20 default anchor mismatch")
text = text.replace(default_anchor, default_anchor + "\n$V320FocusAdaptive = '0'", 1)

line108 = next((line for line in text.splitlines() if line.lstrip().startswith("'108'")), None)
line102 = next((line for line in text.splitlines() if line.lstrip().startswith("'102'")), None)
if not line108 or not line102:
    raise RuntimeError("V3.20 mode anchors missing")
base_body = line102[line102.index("{") + 1 : line102.rindex("}")].strip()
if "v319-cpu-control" not in base_body:
    raise RuntimeError("V3.20 expected V3.19 CPU control body")


def mode_body(experiment: str, cadence: str, adaptive: str) -> str:
    body = base_body.replace("v319-cpu-control", experiment, 1)
    body = re.sub(r"\$V319FocusCadence = '[123]'", f"$V319FocusCadence = '{cadence}'", body, count=1)
    return body + f"; $V320FocusAdaptive = '{adaptive}'"


new_lines = (
    "        '109' { " + mode_body("v320-cp1-p0-control", "1", "0") + " }",
    "        '110' { " + mode_body("v320-cp1-fixed2", "2", "0") + " }",
    "        '111' { " + mode_body("v320-cp1-fixed3", "3", "0") + " }",
    "        '112' { " + mode_body("v320-cp1-adaptive2", "2", "1") + " }",
    "        '113' { " + mode_body("v320-cp1-adaptive3", "3", "1") + " }",
)
anchor = line108 + "\n"
if text.count(anchor) != 1:
    raise RuntimeError("V3.20 mode insertion anchor mismatch")
text = text.replace(anchor, line108 + "\n" + "\n".join(new_lines) + "\n", 1)

manifest_anchor = '    "v319_osg_threading=$V319OsgThreading",'
if text.count(manifest_anchor) != 1:
    raise RuntimeError("V3.20 manifest anchor mismatch")
text = text.replace(manifest_anchor, manifest_anchor + '\n    "v320_focus_adaptive=$V320FocusAdaptive",', 1)

launch_anchor = "    $env:OPENMW_V319_FOCUS_CADENCE = $V319FocusCadence"
if text.count(launch_anchor) != 1:
    raise RuntimeError("V3.20 launch environment anchor mismatch")
text = text.replace(
    launch_anchor,
    "    $env:OPENMW_V320_FOCUS_ADAPTIVE = $V320FocusAdaptive\n" + launch_anchor,
    1,
)

finally_anchor = "finally {\n    Remove-Item Env:OPENMW_V317_LUA_OPT -ErrorAction SilentlyContinue"
if text.count(finally_anchor) != 1:
    raise RuntimeError("V3.20 finally anchor mismatch")
text = text.replace(
    finally_anchor,
    "finally {\n    Remove-Item Env:OPENMW_V320_FOCUS_ADAPTIVE -ErrorAction SilentlyContinue\n"
    "    Remove-Item Env:OPENMW_V317_LUA_OPT -ErrorAction SilentlyContinue",
    1,
)

for required in (
    "Enter 1 through 113",
    "v320-cp1-p0-control",
    "v320-cp1-fixed2",
    "v320-cp1-fixed3",
    "v320-cp1-adaptive2",
    "v320-cp1-adaptive3",
    "v320_focus_adaptive=$V320FocusAdaptive",
    "OPENMW_V320_FOCUS_ADAPTIVE",
):
    if required not in text:
        raise RuntimeError(f"V3.20 launcher missing marker: {required}")

launcher.write_text(text, encoding="utf-8", newline="\n")
print("V3.20 CP1 causal focus modes 109-113 added")
