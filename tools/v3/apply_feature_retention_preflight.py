from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


required = {
    "components/settings/ramcache.hpp": [
        ("return std::max(configured, 1800.f);", "Overdrive 30-minute retention"),
        ("return 65536;", "Overdrive Bullet shape-instance pool"),
        ("return value == \"adaptive\";", "adaptive predictive-preload toggle"),
    ],
    "files/settings-default.cfg": [
        ("v3 streaming scheduler = off", "safe default: adaptive preload off"),
        ("v3 prepared instance cache = false", "safe default: prepared instances off"),
        ("v3 prepared instance cache max = 8192", "prepared-instance test bound"),
    ],
    "apps/openmw/mwworld/scene.cpp": [
        ("V3InsertionAccumulatorScope insertionScope", "exception-safe deep insertion profiler"),
        ("insertionWriter.writeLine(row.str())", "deep insertion CSV output"),
        ("cell_preload_schedule", "predictive preload profiling"),
        ("Settings::RamCache::adaptiveStreamingEnabled()", "adaptive preload pacing"),
        ("preloadCells(duration);", "normal predictive preload execution"),
    ],
    "components/resource/scenemanager.cpp": [
        ("setPreparedInstanceCacheLimit", "prepared-instance cache"),
        ("mPreparedInstanceEnabled.load(std::memory_order_acquire)", "disabled prepared-cache fast path"),
        ("mPreparedInstanceGeneration", "prepared-cache stale-work invalidation"),
        ("scene_clone", "scene clone timing"),
        ("Prepared Instance Hit", "prepared-instance telemetry"),
    ],
    "components/sceneutil/workqueue.cpp": [
        ("workQueueWriter()", "work-queue CSV telemetry"),
        ("TraceScope trace(\"workqueue\"", "cross-thread work trace"),
        ("mV3ActiveThreads", "shutdown-safe active-worker count"),
    ],
    "apps/openmw/mwrender/objectpaging.cpp": [
        ("object_chunk_collect_refs", "object-page reference collection timing"),
        ("object_chunk_template_analysis", "object-page template timing"),
        ("object_chunk_build_instances", "object-page instance timing"),
        ("object_chunk_merge_optimize", "object-page merge timing"),
        ("object_chunk_compile_map", "object-page compile-map timing"),
        ("object_chunk_summary", "object-page workload summary"),
    ],
    "apps/openmw/mwrender/groundcover.cpp": [
        ("groundcover_chunk_create", "groundcover chunk timing"),
        ("groundcover_chunk_summary", "groundcover workload summary"),
    ],
    "apps/openmw/mwrender/pingpongcanvas.cpp": [
        ("v3PostFxWriter", "per-pass PostFX profiler"),
        ("drawGeometry(renderInfo);", "PostFX draw submission"),
        ("node.mHandle ? node.mHandle->getName()", "null-safe PostFX technique labeling"),
    ],
    "components/shader/shadermanager.cpp": [
        ("shader_template_load", "shader template timing"),
        ("shader_source_create", "shader variant timing"),
        ("program_link_apply", "actual lazy GL link timing"),
        ("v3ProgramDetail", "local null-safe shader link detail"),
    ],
    "components/debug/v3hitchtelemetry.hpp": [
        ("OPENMW_V3_FRAME_FILE", "every-frame telemetry"),
        ("AllFrameFlushInterval = 600", "low-perturbation frame buffering"),
    ],
    "components/sceneutil/mwshadowtechnique.cpp": [
        ("OPENMW_V3_SHADOW_FILE", "shadow profiler"),
    ],
    "components/sceneutil/occlusionculling.hpp": [
        ("OPENMW_V3_MSOC_DETAIL_FILE", "detailed MSOC profiler"),
    ],
}

problems = []
for rel, checks in required.items():
    text = read(rel)
    for needle, feature in checks:
        if needle not in text:
            problems.append(f"MISSING {feature}: {rel} lacks {needle!r}")

# Safety invariants: these were considered/implemented during lab development
# but must not survive in the fully generated source.
for rel in ("apps/openmw/mwrender/objectpaging.cpp", "apps/openmw/mwrender/groundcover.cpp"):
    text = read(rel)
    if "streamingDistantObjectChunkLimit" in text or "streamingGroundcoverChunkLimit" in text:
        problems.append(f"UNSAFE page-skipping limiter returned in {rel}")

postfx = read("apps/openmw/mwrender/pingpongcanvas.cpp")
if 'setName("V3 PostFX' in postfx:
    problems.append("UNSAFE diagnostic PostFX StateSet mutation returned")

shader = read("components/shader/shadermanager.cpp")
if "program->setName(v3ProgramName)" in shader or "vertexShader->getName() + \" + \" + fragmentShader->getName()" in shader:
    problems.append("UNSAFE shader-program diagnostic mutation/dereference returned")

if problems:
    raise RuntimeError("V3 feature-retention/safety preflight failed:\n" + "\n".join(problems))

print("V3 feature-retention preflight passed: requested lab functionality is present and known unsafe variants are absent.")
