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
# If any later Set-IniValue call throws, the finally block still restores the
# untouched backup instead of leaving a partially modified settings.cfg.
replace_once(
    "tools/v3/launchers/V3_Lab.ps1",
    '''$changedSettings = $false
try {
    if ($Mode -ne 'Render') {
        Set-IniValue $SettingsPath 'Cells' 'ram cache mode' 'overdrive' ''',
    '''$changedSettings = $false
try {
    if ($Mode -ne 'Render') {
        $changedSettings = $true
        Set-IniValue $SettingsPath 'Cells' 'ram cache mode' 'overdrive' ''',
)

print("V3 profiling-launcher safety pass completed successfully.")
