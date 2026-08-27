import os
from pathlib import Path


ROOT = Path(os.environ.get("OPENMW_PATCH_ROOT", Path(__file__).resolve().parents[2])).resolve()


def replace_exact(rel, old, new, expected=1):
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != expected:
        raise RuntimeError(f"{rel}: expected {expected} V3.9 preload-priority match(es), found {count}")
    path.write_text(text.replace(old, new), encoding="utf-8", newline="\n")
    print(f"V3.9 preload-priority patched {rel} ({count} match(es))")


# QuadTreeWorld calls getChunk(..., compile=true) from its explicit preload path
# and compile=false when a chunk is demanded by ordinary render traversal. Use
# that already-existing distinction to make the policy robust:
# - preload/background work may use the requested strong V3.8 mode 2/3;
# - a chunk that still misses preload and is demanded synchronously falls back to
#   conservative mode 1 instead of performing a forced/deep merge on the frame
#   that needs it.
# Cached chunks do not include the compile flag in their ChunkId, so a strong
# chunk built by preload is reused unchanged by later rendering.
replace_exact(
    "apps/openmw/mwrender/objectpaging.cpp",
    '''            const int v38BatchingMode = static_cast<int>(Settings::cells().mV38WorldBatchingMode);
            const unsigned int v38InstanceCount = static_cast<unsigned int>(pair.second.mInstances.size());''',
    '''            const int v38ConfiguredBatchingMode
                = static_cast<int>(Settings::cells().mV38WorldBatchingMode);
            const bool v39OnDemandFallback
                = static_cast<int>(Settings::cells().mV39FrontloadMode) > 0 && !compile;
            const int v38BatchingMode
                = v39OnDemandFallback ? std::min(v38ConfiguredBatchingMode, 1) : v38ConfiguredBatchingMode;
            const unsigned int v38InstanceCount = static_cast<unsigned int>(pair.second.mInstances.size());''',
)

# Apply the same distinction to post-merge cleanup. On-demand fallback chunks use
# merge-only cleanup even if the configured V3.9 profile normally performs state
# compaction or post-transform optimization. This bounds synchronous construction
# cost without weakening already-preloaded strong chunks.
replace_exact(
    "apps/openmw/mwrender/objectpaging.cpp",
    '''            const int v38BatchingMode = static_cast<int>(Settings::cells().mV38WorldBatchingMode);
            const int v39BatchOptimizerMode = static_cast<int>(Settings::cells().mV39BatchOptimizerMode);

            if (v39BatchOptimizerMode == 0)''',
    '''            const int v38BatchingMode = static_cast<int>(Settings::cells().mV38WorldBatchingMode);
            const int v39ConfiguredBatchOptimizerMode
                = static_cast<int>(Settings::cells().mV39BatchOptimizerMode);
            const int v39BatchOptimizerMode
                = (static_cast<int>(Settings::cells().mV39FrontloadMode) > 0 && !compile)
                ? 1
                : v39ConfiguredBatchOptimizerMode;

            if (v39BatchOptimizerMode == 0)''',
)

print("V3.9 preload-priority / conservative on-demand fallback patched successfully.")
