#!/usr/bin/env python3
"""Fail closed if VulkanMW P3B compacted indirect ownership regresses."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def text(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def require(source: str, token: str, label: str) -> None:
    if token not in source:
        raise SystemExit(f"P3B compact contract missing {label}: {token}")


def main() -> None:
    implementation = text("components/render/backend/vsg/gpupopulationcull.cpp")
    header = text("components/render/backend/vsg/gpupopulationcull.hpp")
    host = text("components/render/backend/vsg/vsgruntimehost.cpp")
    host_header = text("components/render/backend/vsg/vsgruntimehost.hpp")
    cohort = text("tools/vulkanmw/run-publication-cohort.py")
    launcher = text("tools/vulkanmw/run-producer-queues.py")
    bat = text("tools/vulkanmw/START-VulkanMW-P3B-Test.bat")
    cmake = text("apps/openmw/CMakeLists.txt")

    require(header, "bool compacted = false", "compaction result telemetry")
    require(header, "bool compactCommands", "explicit P3B builder control")

    for token, label in (
        ("compactPopulationSafe", "order-safety classifier"),
        ("material->alphaBlendEnabled", "blend-order fallback"),
        ("material->stencil.enabled", "stencil-order fallback"),
        ("atomicAdd(visibleCount[0], 1u)", "GPU visible-slot compaction"),
        ("translations[slot * 3u + 0u]", "GPU compact translation stream"),
        ("rotations[slot * 4u + 0u]", "GPU compact rotation stream"),
        ("scales[slot * 3u + 0u]", "GPU compact scale stream"),
        ("commands[draw].instanceCount = visibleCount[0]", "one batch command visible count"),
        ("commands[draw].firstInstance = 0u", "compacted transform base instance"),
        ("VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT",
         "compute-written vertex buffers"),
        ("VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT", "vertex-input synchronization"),
        ("compactTranslations->buffer->vk", "main-view compact translation binding"),
        ("compactRotations->buffer->vk", "main-view compact rotation binding"),
        ("compactScales->buffer->vk", "main-view compact scale binding"),
        ("replacement->indirectDrawCount = compacted ? 1u : placementCount",
         "one-command compact path with P3A fallback"),
        ("result.stats.indirectCommands = draws.size()", "compacted command-count accounting"),
        ("result.stats.compacted = true", "compacted-path accounting"),
    ):
        require(implementation, token, label)

    for forbidden in ("vkGetQueryPoolResults", "vkMapMemory"):
        if forbidden in implementation:
            raise SystemExit(f"P3B hot path contains forbidden CPU visibility readback: {forbidden}")

    require(host, 'std::getenv("OPENMW_VK_GPU_CULL_COMPACT")', "runtime P3B feature gate")
    require(host, "mGpuPopulationCompactEnabled && !mGpuPopulationCullEnabled", "P3A dependency")
    require(host, "mGpuPopulationCompactEnabled, gpuCullFallback", "explicit compact builder routing")
    require(host_header, "bool mGpuPopulationCompactEnabled = false", "host P3B state")
    require(host, '"gpu_compact_groups"', "runtime compact coverage telemetry")
    require(host, "const bool p3GroupedPopulation = mGpuPopulationCullEnabled && plan.placements.size() > 1",
            "eligible multi-placement GPU grouping policy")
    require(host, '"gpu_grouped_eligible"', "grouping-reason telemetry")
    require(host, '"gpu_grouped_singleton"', "singleton fallback telemetry")
    require(host, '"gpu_grouped_mixed_masks"', "mixed-mask fallback telemetry")
    require(host, '"gpu_grouped_order_sensitive"', "order-sensitive fallback telemetry")

    require(cohort, "'gpu-cull-compact': 'OPENMW_VK_GPU_CULL_COMPACT'", "benchmark feature mapping")
    require(launcher, "GPU_COMPACT_BASE = GPU_CULL_CANDIDATE", "P3A common baseline")
    require(launcher, "GPU_COMPACT_CANDIDATE = GPU_COMPACT_BASE + '+gpu-cull-compact'",
            "single-variable P3B candidate")
    require(launcher, "--gpu-cull-compact", "matched P3B AB mode")
    require(bat, "OPENMW_VK_GPU_CULL_COMPACT", "public P3B launcher explanation")
    require(cmake, "START-VulkanMW-P3B-Test.bat", "installed P3B launcher")

    print("VulkanMW P3B compacted indirect source contract: PASS")


if __name__ == "__main__":
    main()
