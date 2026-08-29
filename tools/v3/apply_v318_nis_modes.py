import os
import re
from pathlib import Path

ROOT = Path(os.environ.get("OPENMW_PATCH_ROOT", Path(__file__).resolve().parents[2])).resolve()
launcher = ROOT / "tools/v3/launchers/V3_Lab.ps1"
text = launcher.read_text(encoding="utf-8")

for marker in ("v318-native-control", "v318-bilinear-77", "Enter 1 through 98"):
    if marker not in text:
        raise RuntimeError(f"V3.18 NIS mode layer expected P0 launcher marker: {marker}")

menu98 = [line for line in text.splitlines() if line.startswith("Write-Host ' 98 = V3.18")]
if len(menu98) != 1:
    raise RuntimeError(f"V3.18 NIS menu anchor mismatch: {len(menu98)}")
old_menu = menu98[0]
text = text.replace(
    old_menu,
    old_menu
    + "\nWrite-Host ' 99 = V3.18 internal render scale 85% + NVIDIA Image Scaling'"
    + "\nWrite-Host '100 = V3.18 internal render scale 77% + NVIDIA Image Scaling (first NIS test)'"
    + "\nWrite-Host '101 = V3.18 internal render scale 66.7% + NVIDIA Image Scaling'",
    1,
)

text, n = re.subn(
    r"do \{ \$choice = Read-Host 'Enter 1 through 98' \} until \(\$choice -in @\(([^\n]+)\)\)",
    lambda m: "do { $choice = Read-Host 'Enter 1 through 101' } until ($choice -in @(" + m.group(1)
    + ",'99','100','101'))",
    text,
    count=1,
)
if n != 1:
    raise RuntimeError("V3.18 NIS choice-range anchor mismatch")

mode95 = [line for line in text.splitlines() if line.lstrip().startswith("'95'")]
mode98 = [line for line in text.splitlines() if line.lstrip().startswith("'98'")]
if len(mode95) != 1 or len(mode98) != 1:
    raise RuntimeError(f"V3.18 NIS switch anchors mismatch: mode95={len(mode95)}, mode98={len(mode98)}")

# Use the already-generated P0 mode bodies as exact controls so only scaler choice
# and the matching internal scale differ.
def make_nis(base_line: str, old_id: str, new_id: str, scale: str) -> str:
    body = base_line[base_line.index("{") + 1 : base_line.rindex("}")].strip()
    if old_id not in body:
        raise RuntimeError(f"Expected experiment id {old_id} in P0 body")
    body = body.replace(old_id, new_id, 1)
    body = re.sub(r"\$V318RenderScale = '[^']+'", f"$V318RenderScale = '{scale}'", body, count=1)
    body = body.replace("$V318Upscaler = 'bilinear'", "$V318Upscaler = 'nis'", 1)
    return body + "; $V318UpscalerSharpness = '0.20'"

line96 = next(line for line in text.splitlines() if line.lstrip().startswith("'96'"))
line97 = next(line for line in text.splitlines() if line.lstrip().startswith("'97'"))
line98 = mode98[0]
new_modes = (
    line98 + "\n"
    + "        '99' { " + make_nis(line96, "v318-bilinear-85", "v318-nis-85", "0.85") + " }\n"
    + "        '100' { " + make_nis(line97, "v318-bilinear-77", "v318-nis-77", "0.77") + " }\n"
    + "        '101' { " + make_nis(line98, "v318-bilinear-667", "v318-nis-667", "0.6666667") + " }\n"
)
anchor = line98 + "\n"
if text.count(anchor) != 1:
    raise RuntimeError("V3.18 NIS Mode98 switch insertion anchor mismatch")
text = text.replace(anchor, new_modes, 1)

# Sharpness is a managed V3.18 setting only for V3.18 scaler modes. P0 bilinear
# modes get the same value for manifest/config determinism even though bilinear
# ignores it.
default_anchor = "$V318Upscaler = 'bilinear'\n$V318RenderScaleManaged = 'false'"
if text.count(default_anchor) != 1:
    raise RuntimeError("V3.18 NIS sharpness default anchor mismatch")
text = text.replace(
    default_anchor,
    "$V318Upscaler = 'bilinear'\n$V318UpscalerSharpness = '0.20'\n$V318RenderScaleManaged = 'false'",
    1,
)

manifest_anchor = '    "v318_upscaler=$V318Upscaler",'
if text.count(manifest_anchor) != 1:
    raise RuntimeError("V3.18 NIS manifest anchor mismatch")
text = text.replace(
    manifest_anchor,
    manifest_anchor + '\n    "v318_upscaler_sharpness=$V318UpscalerSharpness",',
    1,
)

settings_anchor = "        Set-IniValue $SettingsPath 'Video' 'upscaler' $V318Upscaler\n        $changedSettings = $true"
if text.count(settings_anchor) != 1:
    raise RuntimeError("V3.18 NIS managed settings anchor mismatch")
text = text.replace(
    settings_anchor,
    "        Set-IniValue $SettingsPath 'Video' 'upscaler' $V318Upscaler\n"
    "        Set-IniValue $SettingsPath 'Video' 'upscaler sharpness' $V318UpscalerSharpness\n"
    "        $changedSettings = $true",
    1,
)

for required in (
    "100 = V3.18 internal render scale 77% + NVIDIA Image Scaling",
    "v318-nis-85",
    "v318-nis-77",
    "v318-nis-667",
    "Enter 1 through 101",
    "v318_upscaler_sharpness=$V318UpscalerSharpness",
    "Set-IniValue $SettingsPath 'Video' 'upscaler sharpness' $V318UpscalerSharpness",
):
    if required not in text:
        raise RuntimeError(f"V3.18 NIS launcher missing marker: {required}")

launcher.write_text(text, encoding="utf-8", newline="\n")
print("V3.18 NIS causal modes 99-101 added")
