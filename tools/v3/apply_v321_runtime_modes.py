import os
import re
from pathlib import Path

ROOT = Path(os.environ.get("OPENMW_PATCH_ROOT", Path(__file__).resolve().parents[2])).resolve()
launcher = ROOT / "tools/v3/launchers/V3_Lab.ps1"
text = launcher.read_text(encoding="utf-8")

menu124 = [line for line in text.splitlines() if line.startswith("Write-Host '124 = V3.20 CP6")]
if len(menu124) != 1:
    raise RuntimeError(f"V3.21 menu124 anchor mismatch: {len(menu124)}")
extra = "\n".join(
    (
        "Write-Host '125 = V3.21 CP1 exact final V3.20 foundation control'",
        "Write-Host '126 = V3.21 CP1 fixed completed-work admission governor'",
        "Write-Host '127 = V3.21 CP1 adaptive slack/debt completed-work governor'",
    )
)
text = text.replace(menu124[0], menu124[0] + "\n" + extra, 1)

text, count = re.subn(
    r"do \{ \$choice = Read-Host 'Enter 1 through 124' \} until \(\$choice -in @\(([^\n]+)\)\)",
    lambda match: "do { $choice = Read-Host 'Enter 1 through 127' } until ($choice -in @(" + match.group(1)
    + ",'125','126','127'))",
    text,
    count=1,
)
if count != 1:
    raise RuntimeError("V3.21 choice-range anchor mismatch")

# Match the final V3.20 launcher defaults as a multiline block. The single
# V320FocusAdaptive assignment also appears inside many mode bodies, so it is
# intentionally not used as a global one-line anchor.
default_anchor = """$V319OsgThreading = ''
$V320FocusAdaptive = '0'
$V320EngineLuaFastPaths = '0'"""
if text.count(default_anchor) != 1:
    raise RuntimeError(f"V3.21 default governor anchor mismatch: {text.count(default_anchor)}")
text = text.replace(
    default_anchor,
    """$V319OsgThreading = ''
$V320FocusAdaptive = '0'
$V321CompletionGovernor = '0'
$V320EngineLuaFastPaths = '0'""",
    1,
)

line123 = next((line for line in text.splitlines() if line.lstrip().startswith("'123'")), None)
line124 = next((line for line in text.splitlines() if line.lstrip().startswith("'124'")), None)
if not line123 or not line124:
    raise RuntimeError("V3.21 mode123/124 anchors missing")

control_body = line123[line123.index("{") + 1 : line123.rindex("}")].strip()
if "v320-cp6-combined-stock" not in control_body:
    raise RuntimeError("V3.21 expected final normal V3.20 Mode123 body")
control_body = control_body.replace("v320-cp6-combined-stock", "v321-cp1-v320-control", 1)

new_lines = (
    "        '125' { " + control_body + "; $V321CompletionGovernor = '0' }",
    "        '126' { "
    + control_body.replace("v321-cp1-v320-control", "v321-cp1-fixed-completion-governor", 1)
    + "; $V321CompletionGovernor = '1' }",
    "        '127' { "
    + control_body.replace("v321-cp1-v320-control", "v321-cp1-adaptive-completion-governor", 1)
    + "; $V321CompletionGovernor = '2' }",
)
anchor = line124 + "\n"
if text.count(anchor) != 1:
    raise RuntimeError("V3.21 mode insertion anchor mismatch")
text = text.replace(anchor, line124 + "\n" + "\n".join(new_lines) + "\n", 1)

manifest_anchor = '    "v320_focus_adaptive=$V320FocusAdaptive",'
if text.count(manifest_anchor) != 1:
    raise RuntimeError("V3.21 manifest anchor mismatch")
text = text.replace(
    manifest_anchor,
    manifest_anchor + '\n    "v321_completion_governor=$V321CompletionGovernor",',
    1,
)

launch_anchor = "    $env:OPENMW_V320_FOCUS_ADAPTIVE = $V320FocusAdaptive"
if text.count(launch_anchor) != 1:
    raise RuntimeError("V3.21 environment anchor mismatch")
text = text.replace(
    launch_anchor,
    launch_anchor + "\n    $env:OPENMW_V321_COMPLETION_GOVERNOR = $V321CompletionGovernor",
    1,
)

# Final V3.20 cleanup contains several V3.20 env removals before the focus line.
# Anchor to the unique focus cleanup itself rather than assuming it is first in
# the finally block.
cleanup_anchor = "    Remove-Item Env:OPENMW_V320_FOCUS_ADAPTIVE -ErrorAction SilentlyContinue"
if text.count(cleanup_anchor) != 1:
    raise RuntimeError(f"V3.21 cleanup anchor mismatch: {text.count(cleanup_anchor)}")
text = text.replace(
    cleanup_anchor,
    "    Remove-Item Env:OPENMW_V321_COMPLETION_GOVERNOR -ErrorAction SilentlyContinue\n" + cleanup_anchor,
    1,
)

for marker in (
    "Enter 1 through 127",
    "125 = V3.21 CP1 exact final V3.20 foundation control",
    "126 = V3.21 CP1 fixed completed-work admission governor",
    "127 = V3.21 CP1 adaptive slack/debt completed-work governor",
    "v321-cp1-v320-control",
    "v321-cp1-fixed-completion-governor",
    "v321-cp1-adaptive-completion-governor",
    "v321_completion_governor=$V321CompletionGovernor",
    "OPENMW_V321_COMPLETION_GOVERNOR",
    "Remove-Item Env:OPENMW_V321_COMPLETION_GOVERNOR -ErrorAction SilentlyContinue",
):
    if marker not in text:
        raise RuntimeError(f"V3.21 launcher missing marker: {marker}")

launcher.write_text(text, encoding="utf-8", newline="\n")
print("V3.21 CP1 causal modes 125-127 added")
