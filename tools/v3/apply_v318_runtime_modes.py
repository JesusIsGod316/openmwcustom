import os
import re
from pathlib import Path

ROOT = Path(os.environ.get("OPENMW_PATCH_ROOT", Path(__file__).resolve().parents[2])).resolve()
launcher = ROOT / "tools/v3/launchers/V3_Lab.ps1"
text = launcher.read_text(encoding="utf-8")

for marker in ("v317-stock-control", "v317-combined-balanced", "Enter 1 through 94"):
    if marker not in text:
        raise RuntimeError(f"V3.18 runtime-mode layer expected V3.17 launcher marker: {marker}")

# P0 deliberately benchmarks resolution architecture before NIS. This separates
# the pixel-work gain from the cost/quality of the eventual scaler provider.
menu_lines = [line for line in text.splitlines() if line.startswith("Write-Host ' 94 = V3.17")]
if len(menu_lines) != 1:
    raise RuntimeError(f"V3.18 launcher menu anchor mismatch: found {len(menu_lines)} Mode94 label(s)")
old_menu = menu_lines[0]
new_menu = old_menu + "\n" + "\n".join(
    [
        "Write-Host ' 95 = V3.18 native-resolution control (100% / bilinear path inactive)'",
        "Write-Host ' 96 = V3.18 internal render scale 85% + bilinear upscale'",
        "Write-Host ' 97 = V3.18 internal render scale 77% + bilinear upscale'",
        "Write-Host ' 98 = V3.18 internal render scale 66.7% + bilinear upscale'",
    ]
)
text = text.replace(old_menu, new_menu, 1)

text, n = re.subn(
    r"do \{ \$choice = Read-Host 'Enter 1 through 94' \} until \(\$choice -in @\(([^\n]+)\)\)",
    lambda m: "do { $choice = Read-Host 'Enter 1 through 98' } until ($choice -in @(" + m.group(1)
    + ",'95','96','97','98'))",
    text,
    count=1,
)
if n != 1:
    raise RuntimeError("V3.18 launcher choice-range anchor mismatch")

mode90_candidates = [line for line in text.splitlines() if line.lstrip().startswith("'90'")]
mode94_candidates = [line for line in text.splitlines() if line.lstrip().startswith("'94'")]
if len(mode90_candidates) != 1 or len(mode94_candidates) != 1:
    raise RuntimeError(
        f"V3.18 launcher mode anchor mismatch: mode90={len(mode90_candidates)} mode94={len(mode94_candidates)}"
    )
mode90 = mode90_candidates[0]
mode94 = mode94_candidates[0]
body90 = mode90[mode90.index("{") + 1 : mode90.rindex("}")].strip()
if "v317-stock-control" not in body90:
    raise RuntimeError("V3.18 Mode90 no longer carries v317-stock-control identity")


def v318_body(experiment: str, scale: str) -> str:
    body = body90.replace("v317-stock-control", experiment, 1)
    return body + f"; $V318RenderScale = '{scale}'; $V318Upscaler = 'bilinear'; $V318RenderScaleManaged = 'true'"

insert_anchor = mode94 + "\n"
new_modes = (
    mode94 + "\n"
    + "        '95' { " + v318_body("v318-native-control", "1.0") + " }\n"
    + "        '96' { " + v318_body("v318-bilinear-85", "0.85") + " }\n"
    + "        '97' { " + v318_body("v318-bilinear-77", "0.77") + " }\n"
    + "        '98' { " + v318_body("v318-bilinear-667", "0.6666667") + " }\n"
)
if text.count(insert_anchor) != 1:
    raise RuntimeError("V3.18 Mode94 switch anchor mismatch")
text = text.replace(insert_anchor, new_modes, 1)

# Pre-V3.18 modes do not modify render scale. This is critical because older
# attribution modes must continue to use the user's existing setting unchanged.
default_anchor = "$V317EngineLuaOptimizations = 'false'\n$RendererProfiling"
if text.count(default_anchor) != 1:
    raise RuntimeError("V3.18 launcher defaults anchor mismatch")
text = text.replace(
    default_anchor,
    "$V317EngineLuaOptimizations = 'false'\n"
    "$V318RenderScale = '1.0'\n"
    "$V318Upscaler = 'bilinear'\n"
    "$V318RenderScaleManaged = 'false'\n"
    "$RendererProfiling",
    1,
)

manifest_anchor = '    "v317_engine_lua_optimizations=$V317EngineLuaOptimizations",'
if text.count(manifest_anchor) != 1:
    raise RuntimeError("V3.18 TEST_MODE manifest anchor mismatch")
text = text.replace(
    manifest_anchor,
    manifest_anchor + '\n    "v318_render_scale=$V318RenderScale",\n'
    '    "v318_upscaler=$V318Upscaler",\n'
    '    "v318_render_scale_managed=$V318RenderScaleManaged",',
    1,
)

settings_anchor = "    Copy-Item -LiteralPath $SettingsPath -Destination (Join-Path $ProfileDir 'settings-effective-test.cfg') -Force"
if text.count(settings_anchor) != 1:
    raise RuntimeError("V3.18 settings-effective anchor mismatch")
settings_block = r'''    if ($V318RenderScaleManaged -eq 'true') {
        Set-IniValue $SettingsPath 'Video' 'render scale' $V318RenderScale
        Set-IniValue $SettingsPath 'Video' 'upscaler' $V318Upscaler
        $changedSettings = $true
    }
    Copy-Item -LiteralPath $SettingsPath -Destination (Join-Path $ProfileDir 'settings-effective-test.cfg') -Force'''
text = text.replace(settings_anchor, settings_block, 1)

for required in (
    "95 = V3.18 native-resolution control",
    "v318-bilinear-85",
    "v318-bilinear-77",
    "v318-bilinear-667",
    "Enter 1 through 98",
    "v318_render_scale=$V318RenderScale",
    "Set-IniValue $SettingsPath 'Video' 'render scale' $V318RenderScale",
):
    if required not in text:
        raise RuntimeError(f"V3.18 generated launcher missing marker: {required}")

launcher.write_text(text, encoding="utf-8", newline="\n")

marker = ROOT / "V3.18-RENDER-LAYER.txt"
marker.write_text(
    "\n".join(
        [
            "V3.18 renderer efficiency / internal resolution foundation",
            "p0=display-resolution-separated-from-3d-render-resolution",
            "hud_ui=native-output-resolution",
            "postfx=internal-resolution-before-single-upscale",
            "upscaler_p0=bilinear",
            "mode95=100-percent-native-control",
            "mode96=85-percent-bilinear",
            "mode97=77-percent-bilinear",
            "mode98=66.7-percent-bilinear",
            "nis_status=next-layer-not-silently-emulated",
            "stereo_status=p0-native-only",
            "",
        ]
    ),
    encoding="utf-8",
    newline="\n",
)
print("V3.18 render-scale modes 95-98 added")
