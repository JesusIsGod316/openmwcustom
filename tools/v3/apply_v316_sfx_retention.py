import os
from pathlib import Path

ROOT = Path(os.environ.get("OPENMW_PATCH_ROOT", Path(__file__).resolve().parents[2])).resolve()
launcher = ROOT / "tools/v3/launchers/V3_Lab.ps1"
text = launcher.read_text(encoding="utf-8")

# Buffered Lua ambient SFX go through SoundBufferPool/OpenALOutput::loadSound,
# not the streamed FFmpeg head-cache path. The stock cache is only 56/64 MB and
# can therefore churn in large sound mods. V3.16 uses system RAM aggressively to
# keep already-decoded OpenAL Soft buffers resident and avoid repeat decode/I/O.
# Mode87 remains the clean head-cache isolation run. Modes88/89 add retention.

def patch_mode(mode, min_mb, max_mb):
    global text
    prefix = f"        '{mode}' {{"
    lines = text.splitlines()
    matches = [i for i, line in enumerate(lines) if line.startswith(prefix)]
    if len(matches) != 1:
        raise RuntimeError(f"V3.16 SFX retention expected one Mode{mode} launcher line, found {len(matches)}")
    i = matches[0]
    line = lines[i]
    if not line.rstrip().endswith("}"):
        raise RuntimeError(f"V3.16 Mode{mode} launcher line has unexpected layout")
    line = line.rstrip()[:-1].rstrip()
    line += f"; $V316BufferCacheMin = '{min_mb}'; $V316BufferCacheMax = '{max_mb}' }}"
    lines[i] = line
    text = "\n".join(lines) + "\n"

patch_mode("88", 256, 384)
patch_mode("89", 512, 768)

old_defaults = "$V316HeadCacheSize = '0'\n$RendererProfiling"
new_defaults = "$V316HeadCacheSize = '0'\n$V316BufferCacheMin = ''\n$V316BufferCacheMax = ''\n$RendererProfiling"
if text.count(old_defaults) != 1:
    raise RuntimeError("V3.16 SFX retention default anchor mismatch")
text = text.replace(old_defaults, new_defaults, 1)

old_setting = "        Set-IniValue $SettingsPath 'Sound' 'head cache size' $V316HeadCacheSize"
new_setting = old_setting + "\n" + r'''        if ($V316BufferCacheMin -ne '') {
            Set-IniValue $SettingsPath 'Sound' 'buffer cache min' $V316BufferCacheMin
            Set-IniValue $SettingsPath 'Sound' 'buffer cache max' $V316BufferCacheMax
        }'''
if text.count(old_setting) != 1:
    raise RuntimeError("V3.16 SFX retention settings anchor mismatch")
text = text.replace(old_setting, new_setting, 1)

text = text.replace(
    "Write-Host ' 88 = V3.16 balanced general-play hitch candidate'",
    "Write-Host ' 88 = V3.16 balanced hitch: audio64 + 256/384MB decoded SFX retention'",
    1,
)
text = text.replace(
    "Write-Host ' 89 = V3.16 aggressive general-play hitch candidate'",
    "Write-Host ' 89 = V3.16 aggressive hitch: audio128 + 512/768MB decoded SFX retention'",
    1,
)

launcher.write_text(text, encoding="utf-8", newline="\n")

marker = ROOT / "V3.16-HITCH-LAYER.txt"
with marker.open("a", encoding="utf-8", newline="\n") as f:
    f.write("mode88_buffered_sfx_cache_mb=256/384\n")
    f.write("mode89_buffered_sfx_cache_mb=512/768\n")
    f.write("buffered_sfx_retention_scope=repeat-load-thrash-not-first-decode\n")

print("V3.16 buffered SFX high-retention profiles applied")
