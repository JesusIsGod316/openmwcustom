import os
from pathlib import Path

ROOT = Path(os.environ.get("OPENMW_PATCH_ROOT", Path(__file__).resolve().parents[2])).resolve()


def replace_exact(rel, old, new, expected=1):
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != expected:
        raise RuntimeError(f"{rel}: expected {expected} SFX-metadata-frontload match(es), found {count}")
    path.write_text(text.replace(old, new, expected), encoding="utf-8", newline="\n")
    print(f"V3.16 SFX metadata frontload patched {rel} ({count} match(es))")


# SoundBufferPool already has the idempotent prepareSoundRecords() helper from
# the aggressive predecode layer. Expose only a narrow main-thread wrapper so
# balanced/aggressive modes can pay the record-map construction cost while the
# loading screen is still active instead of on the first gameplay sound lookup.
replace_exact(
    "apps/openmw/mwsound/soundbuffer.hpp",
    '''        // Prepare ESM sound metadata on the main thread and return unique resource
        // names suitable for background PCM predecode.
        std::vector<VFS::Path::Normalized> getResourceNamesForPredecode();

        void use''',
    '''        // Prepare ESM sound metadata on the main thread and return unique resource
        // names suitable for background PCM predecode.
        std::vector<VFS::Path::Normalized> getResourceNamesForPredecode();

        // V3.16 balanced/aggressive startup frontload. This performs metadata
        // construction only; it does not decode audio or touch OpenAL buffers.
        void prepareSoundRecordsForGameplay() { prepareSoundRecords(); }

        void use''',
)

replace_exact(
    "apps/openmw/mwsound/soundmanagerimp.hpp",
    '''        void update(float duration);

        void setListenerPosDir(''',
    '''        void update(float duration);

        // V3.16: move one-time ESM sound-record map construction under the
        // loading screen for profiles that opt in. Safe to call repeatedly.
        void prepareSfxMetadata() { mSoundBuffers.prepareSoundRecordsForGameplay(); }

        void setListenerPosDir(''',
)

# Keep the control and audio-only isolation mode exact by gating this frontload
# behind a dedicated default-off V3 setting rather than inferring from user sound
# cache sizes or another unrelated experiment switch.
replace_exact(
    "components/settings/categories/cells.hpp",
    '''        SettingValue<int> mV315AdaptiveCompileGovernor{ mIndex, "V3", "v3.15 adaptive compile governor",
            makeClampSanitizerInt(0, 2) };''',
    '''        SettingValue<int> mV315AdaptiveCompileGovernor{ mIndex, "V3", "v3.15 adaptive compile governor",
            makeClampSanitizerInt(0, 2) };
        SettingValue<bool> mV316SfxMetadataFrontload{ mIndex, "V3", "v3.16 sfx metadata frontload" };''',
)

replace_exact(
    "files/settings-default.cfg",
    '''# V3.15 renderer/submission + traversal-tail controls.''',
    '''# V3.16 general-play hitch controls.
# Build the immutable ESM sound-id/resource metadata maps while the startup
# loading screen is still active instead of on the first gameplay SFX lookup.
v3.16 sfx metadata frontload = false

# V3.15 renderer/submission + traversal-tail controls.''',
)

replace_exact(
    "apps/openmw/engine.cpp",
    '''        dataLoading.get();
    }
    listener->loadingOff();

    mWorld->init''',
    '''        dataLoading.get();
    }
    if (static_cast<bool>(Settings::cells().mV316SfxMetadataFrontload))
        mSoundManager->prepareSfxMetadata();
    listener->loadingOff();

    mWorld->init''',
)

# Launcher: Mode88/89 opt in, while 86/87 remain unchanged controls.
launcher = ROOT / "tools/v3/launchers/V3_Lab.ps1"
text = launcher.read_text(encoding="utf-8")

old_defaults = "$V316SfxPredecodeWorkers = '0'\n$RendererProfiling"
new_defaults = "$V316SfxPredecodeWorkers = '0'\n$V316SfxMetadataFrontload = 'false'\n$RendererProfiling"
if text.count(old_defaults) != 1:
    raise RuntimeError("V3.16 SFX metadata launcher default anchor mismatch")
text = text.replace(old_defaults, new_defaults, 1)

lines = text.splitlines()
for mode in ("88", "89"):
    prefix = f"        '{mode}' {{"
    matches = [i for i, line in enumerate(lines) if line.startswith(prefix)]
    if len(matches) != 1:
        raise RuntimeError(f"V3.16 SFX metadata expected one Mode{mode} line, found {len(matches)}")
    i = matches[0]
    line = lines[i]
    if not line.rstrip().endswith("}"):
        raise RuntimeError(f"V3.16 Mode{mode} launcher line has unexpected layout")
    line = line.rstrip()[:-1].rstrip()
    line += "; $V316SfxMetadataFrontload = 'true' }"
    lines[i] = line
text = "\n".join(lines) + "\n"

setting_anchor = "        Set-IniValue $SettingsPath 'Sound' 'sfx predecode workers' $V316SfxPredecodeWorkers"
if text.count(setting_anchor) != 1:
    raise RuntimeError("V3.16 SFX metadata launcher setting anchor mismatch")
text = text.replace(
    setting_anchor,
    setting_anchor
    + "\n        Set-IniValue $SettingsPath 'V3' 'v3.16 sfx metadata frontload' $V316SfxMetadataFrontload",
    1,
)
launcher.write_text(text, encoding="utf-8", newline="\n")

marker = ROOT / "V3.16-HITCH-LAYER.txt"
with marker.open("a", encoding="utf-8", newline="\n") as f:
    f.write("mode88_sfx_metadata_frontload=1_loading_screen\n")
    f.write("mode89_sfx_metadata_frontload=1_loading_screen\n")
    f.write("sfx_metadata_frontload_decode_audio=0\n")
    f.write("sfx_metadata_frontload_openal_calls=0\n")

print("V3.16 SFX metadata loading-screen frontload applied")
