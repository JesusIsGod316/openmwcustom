#!/usr/bin/env python3
"""Fail closed if VulkanMW P3 GPU cull/indirect ownership regresses."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def text(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def require(source: str, token: str, label: str) -> None:
    if token not in source:
        raise SystemExit(f"P3 GPU-cull contract missing {label}: {token}")


def main() -> None:
    header = text("components/render/backend/vsg/gpupopulationcull.hpp")
    implementation = text("components/render/backend/vsg/gpupopulationcull.cpp")
    host = text("components/render/backend/vsg/vsgruntimehost.cpp")
    host_header = text("components/render/backend/vsg/vsgruntimehost.hpp")
    bootstrap = text("components/render/backend/vsg/vsgruntimebootstrap.cpp")
    sources = text("components/render/backend/vsg/runtime-sources.cmake")
    openmw_cmake = text("apps/openmw/CMakeLists.txt")
    cohort = text("tools/vulkanmw/run-publication-cohort.py")
    launcher = text("tools/vulkanmw/run-producer-queues.py")
    bat = text("tools/vulkanmw/START-VulkanMW-P3-Test.bat")

    require(header, "GpuPopulationCullBuild", "backend-private P3 build contract")
    require(header, "updateGpuPopulationCullViewData", "per-frame view-data update")

    for token, label in (
        ("vkCmdDrawIndexedIndirect", "actual Vulkan indirect draw"),
        ("vkCmdDrawIndexed(commands", "secondary-view direct fallback"),
        ("commandBuffer.viewID == indirectViewId", "main-view-only indirect selection"),
        ("VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT",
         "compute-written indirect buffer"),
        ("vsg::ComputePipeline::create", "compute pipeline"),
        ("vsg::Dispatch::create", "GPU cull dispatch"),
        ("VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT", "compute producer stage"),
        ("VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT", "indirect consumer stage"),
        ("VK_ACCESS_SHADER_WRITE_BIT", "compute write barrier"),
        ("VK_ACCESS_INDIRECT_COMMAND_READ_BIT", "indirect read barrier"),
        ("VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT",
         "cross-frame draw-read to compute-write dependency"),
        ("VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT",
         "same-frame compute-write to draw-read dependency"),
        ("commands[commandIndex].instanceCount = visible ? 1u : 0u",
         "GPU visibility decision"),
        ("commands[commandIndex].firstInstance = placement",
         "stable placement transform identity"),
        ("outsideNear", "Vulkan zero-to-one frustum near plane"),
        ("distance(lod.xyz, viewData[4].xyz)", "authored maximum-distance test"),
    ):
        require(implementation, token, label)

    for forbidden in ("vkGetQueryPoolResults", "vkMapMemory", "readback"):
        if forbidden in implementation:
            raise SystemExit(f"P3 GPU-cull hot path contains forbidden CPU result path: {forbidden}")

    require(host, 'std::getenv("OPENMW_VK_GPU_CULL_INDIRECT")', "runtime feature gate")
    require(host, "mGpuPopulationCullEnabled && !mGpuSceneTablesEnabled", "P2 dependency")
    require(host, "updateGpuPopulationCullViewData(*mGpuPopulationCullViewData", "current main-view upload")
    require(host, "enableGpuPopulationCull(world, plan", "static population integration")
    require(host, "mGpuPopulationCullRoot->children.swap(nextGpuCullChildren)",
            "atomic compute-graph publication")
    require(host_header, "vsg::ref_ptr<vsg::Group> gpuCullCompute", "resident compute ownership")

    compute_add = host.find("mCommandGraph->addChild(mGpuPopulationCullRoot)")
    main_add = host.find("mCommandGraph->addChild(mRenderGraph)", compute_add)
    post_add = host.find("mCommandGraph->addChild(mPostTarget.renderGraph)", compute_add)
    if compute_add < 0 or (main_add < 0 and post_add < 0):
        raise SystemExit("P3 compute graph is not attached before a main render graph")
    first_main = min(x for x in (main_add, post_add) if x >= 0)
    if compute_add >= first_main:
        raise SystemExit("P3 compute graph must record before the main-view render graph")

    require(bootstrap, 'std::getenv("OPENMW_VK_GPU_CULL_INDIRECT")',
            "pre-device queue capability gate")
    require(bootstrap, "traits->queueFlags |= VK_QUEUE_COMPUTE_BIT",
            "graphics+compute queue request")
    require(sources, "gpupopulationcull.cpp", "production source closure")
    require(openmw_cmake, "START-VulkanMW-P3-Test.bat", "installed P3 benchmark launcher")

    require(cohort, "'gpu-cull-indirect': 'OPENMW_VK_GPU_CULL_INDIRECT'",
            "benchmark feature mapping")
    require(launcher, "--gpu-cull-indirect", "matched A/B command-line mode")
    require(launcher, "GPU_CULL_BASE = GPU_TABLE_CANDIDATE", "P2 common baseline")
    require(bat, "OPENMW_VK_GPU_CULL_INDIRECT", "public P3 launcher explanation")

    print("VulkanMW P3 GPU cull/indirect source contract: PASS")


if __name__ == "__main__":
    main()
