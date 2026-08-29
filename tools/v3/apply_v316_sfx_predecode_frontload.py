import os
from pathlib import Path

ROOT = Path(os.environ.get("OPENMW_PATCH_ROOT", Path(__file__).resolve().parents[2])).resolve()


def replace_exact(rel, old, new, expected=1):
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != expected:
        raise RuntimeError(f"{rel}: expected {expected} SFX-predecode-frontload match(es), found {count}")
    path.write_text(text.replace(old, new, expected), encoding="utf-8", newline="\n")
    print(f"V3.16 SFX predecode frontload patched {rel} ({count} match(es))")


# The aggressive predecode layer originally enumerated every ESM-backed sound
# resource on the first gameplay SoundManager::update. The decode itself was
# off-thread, but building the unique resource vector and queue could still put
# a large allocation/hash burst on that first gameplay frame. Give the engine a
# narrow idempotent entry point so Mode89 can do that enumeration while the
# startup loading screen is still active.
replace_exact(
    "apps/openmw/mwsound/soundmanagerimp.hpp",
    '''        void update(float duration);

        // V3.16: move one-time ESM sound-record map construction under the''',
    '''        void queueSfxPredecode();
        void update(float duration);

        // V3.16: move one-time ESM sound-record map construction under the''',
)

replace_exact(
    "apps/openmw/mwsound/soundmanagerimp.cpp",
    '''    void SoundManager::update(float duration)
    {
        if (!mOutput->isInitialized() || mPlaybackPaused)
            return;''',
    '''    void SoundManager::queueSfxPredecode()
    {
        if (mV316SfxPrewarmQueued || Settings::sound().mSfxPredecodeCacheSize == 0
            || Settings::sound().mSfxPredecodeWorkers == 0)
            return;

        // getResourceNamesForPredecode() performs only main-thread metadata/map
        // traversal. The actual VFS + FFmpeg work starts on the cache's idle
        // worker after this vector is handed off.
        mOutput->queueSoundPredecode(mSoundBuffers.getResourceNamesForPredecode());
        mV316SfxPrewarmQueued = true;
    }

    void SoundManager::update(float duration)
    {
        if (!mOutput->isInitialized() || mPlaybackPaused)
            return;''',
)

replace_exact(
    "apps/openmw/mwsound/soundmanagerimp.cpp",
    '''        if (state != MWBase::StateManager::State_NoGame && !mV316SfxPrewarmQueued
            && Settings::sound().mSfxPredecodeCacheSize != 0 && Settings::sound().mSfxPredecodeWorkers != 0)
        {
            mOutput->queueSoundPredecode(mSoundBuffers.getResourceNamesForPredecode());
            mV316SfxPrewarmQueued = true;
        }

        updateSounds(duration);''',
    '''        // Fallback for custom profiles that enable predecode without the
        // V3.16 loading-screen frontload. Mode89 normally queues this earlier.
        if (state != MWBase::StateManager::State_NoGame)
            queueSfxPredecode();

        updateSounds(duration);''',
)

replace_exact(
    "apps/openmw/engine.cpp",
    '''    if (static_cast<bool>(Settings::cells().mV316SfxMetadataFrontload))
        mSoundManager->prepareSfxMetadata();
    listener->loadingOff();''',
    '''    if (static_cast<bool>(Settings::cells().mV316SfxMetadataFrontload))
    {
        mSoundManager->prepareSfxMetadata();
        // Mode89 has the PCM predecode reservoir enabled. Queue its ESM-backed
        // resource list now so neither enumeration nor queue construction lands
        // on the first ordinary gameplay frame. Modes without predecode return
        // immediately from this call.
        mSoundManager->queueSfxPredecode();
    }
    listener->loadingOff();''',
)

marker = ROOT / "V3.16-HITCH-LAYER.txt"
with marker.open("a", encoding="utf-8", newline="\n") as f:
    f.write("mode89_sfx_predecode_queue=loading_screen\n")
    f.write("sfx_predecode_first_gameplay_enumeration=eliminated_when_frontload_enabled\n")

print("V3.16 aggressive SFX predecode queue moved under loading screen")
