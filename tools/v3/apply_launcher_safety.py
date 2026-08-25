from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def replace_once(rel, old, new):
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{rel}: expected exactly one match, found {count}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8", newline="\n")
    print(f"launcher-safety patched {rel}")


# Mark settings as needing restoration before the first temporary mutation.
# Use explicit newline strings here instead of a triple-quoted marker ending
# immediately after a PowerShell single quote; the previous form accidentally
# required a literal trailing space after 'overdrive' and failed in CI.
old = (
    "$changedSettings = $false\n"
    "try {\n"
    "    if ($Mode -ne 'Render') {\n"
    "        Set-IniValue $SettingsPath 'Cells' 'ram cache mode' 'overdrive'"
)
new = (
    "$changedSettings = $false\n"
    "try {\n"
    "    if ($Mode -ne 'Render') {\n"
    "        $changedSettings = $true\n"
    "        Set-IniValue $SettingsPath 'Cells' 'ram cache mode' 'overdrive'"
)
replace_once("tools/v3/launchers/V3_Lab.ps1", old, new)

print("V3 profiling-launcher safety pass completed successfully.")
