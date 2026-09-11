#!/usr/bin/env python3
"""Source-level CP3E route guard.

This is deliberately structural: production compilation remains the Windows
gate, while this catches accidental loss of the explicit Vulkan route, the
OpenGL control, GUI injection, lifecycle ownership, headless OSG boundary, and
public compile-definition propagation required by Engine's conditional ABI.
"""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]


def text(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def require(path: str, *needles: str) -> None:
    source = text(path)
    missing = [needle for needle in needles if needle not in source]
    if missing:
        raise SystemExit(f"{path}: missing CP3E contract: {missing}")


require(
    "apps/openmw/engine.cpp",
    ".vsgVulkan = true",
    "mUseVulkanRenderer = renderBackend.backend == RenderCore::RenderBackendKind::VsgVulkan",
    "V4EngineRenderBridge::createConfigured(*mVFS)",
    "mWindow = mV4RenderBridge->sdlWindow()",
    "if (!mUseVulkanRenderer)\n        createWindow();",
    "mV4RenderBridge->takeSceneRenderLifecycle()",
    "presentVulkanFrame(frametime, false)",
    "mViewer->updateTraversal();",
)
require(
    "apps/openmw/CMakeLists.txt",
    "target_compile_definitions(openmw-lib PUBLIC OPENMW_ENABLE_V4_VULKAN_RUNTIME=1)",
)
require(
    "components/rendercore/backendselection.hpp",
    "if (request.preference == RenderBackendPreference::Auto)",
    "AutomaticCompatibilityControl",
    "RequiredAutomaticVsgCompatibility",
)
require(
    "apps/openmw/mwgui/windowmanagerimp.cpp",
    "if (guiPlatform)",
    "mPresentCallback()",
)
require(
    "apps/openmw/mwrender/v4enginerenderbridge.cpp",
    "V4EngineRenderBridge::createGuiPlatform",
    "V4EngineRenderBridge::renderGuiFrame",
    "input.invalidateHistory = true",
)
require(
    "components/sdlutil/sdlinputwrapper.cpp",
    "if (osg::GraphicsContext* context = mViewer->getCamera()->getGraphicsContext())",
)
require(
    "components/vsgmygui/rendermanager.cpp",
    "skipping a foreign render-target texture",
    "setExternalTexture",
    "texture->identity()",
    "texture->revision()",
)
require(
    "components/render/backend/vsg/vsgruntimehost.cpp",
    "synchronizeGui()",
    "mGuiRetirements.queue",
    "incremental VSG MyGUI compilation failed before overlay publication",
)
require(
    "apps/openmw/main.cpp",
    'if (std::string_view(argv[i]) == "--version")',
    "Keep the version probe independent of SDL video initialization",
)
require(
    ".github/workflows/v4-cp3b3.yml",
    "$openmwDeps/installed/x64-windows/bin/Release",
    "stage-v4-cp3b4/openmw-vulkan-nif-conformance.exe",
    "stage-v4-vulkan-openmw",
    "Validate production executable before packaging",
    "function Add-RuntimeDllRoot",
    "Conflicting runtime DLL",
    "MyGUIEngine.dll",
    "SDL3.dll",
    "CP3E-RUNTIME-PROVENANCE.csv",
    "CP3E-PACKAGE-SHA256.txt",
    "Validate uploaded-package executable surface",
    "openmw-v4-cp3e-vulkan-windows-${{ github.sha }}",
)

print("CP3E application route contract: PASS")
