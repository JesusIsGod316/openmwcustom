#!/usr/bin/env python3
from pathlib import Path


def source(path: str) -> str:
    value = Path(path).read_text(encoding="utf-8")
    if not value:
        raise SystemExit(f"empty source file: {path}")
    return value


def require(text: str, token: str, label: str) -> None:
    if token not in text:
        raise SystemExit(f"{label}: missing required source contract: {token}")


video = source("apps/openmw/mwgui/videowidget.cpp")
menu = source("apps/openmw/mwgui/mainmenu.cpp")
window = source("apps/openmw/mwgui/windowmanagerimp.cpp")
render = source("components/vsgmygui/rendermanager.cpp")
engine = source("apps/openmw/engine.cpp")

# The FFmpeg decoder may remain OSG-backed internally during migration, but an
# explicit Vulkan GUI route must publish decoded pixels through a native VSG
# MyGUI texture rather than handing the renderer an OSG Texture2D.
require(video, "updateVulkanVideoTexture", "Vulkan video bridge")
require(video, "dynamic_cast<VsgMyGui::RenderManager*>", "Vulkan backend selection")
require(video, "std::make_unique<VsgMyGui::Texture>", "native video texture")
require(video, "target.setData(std::move(rgba))", "native decoded-frame publication")
require(video, "VK_FORMAT_R8G8B8A8_UNORM", "decoded video format")
require(video, "rgba->properties.origin = vsg::TOP_LEFT", "decoded video orientation")
require(video, "V4 Vulkan video bridge: publishing decoded RGBA8 frames", "runtime video-route diagnostic")

# Both startup/credits videos and the animated main-menu background must flow
# through VideoWidget, so one native bridge covers the two observed black-screen
# points instead of maintaining a menu-only workaround.
require(window, "mVideoWidget->commitFrame();", "startup video commit")
require(window, "mPresentCallback();", "startup Vulkan GUI-only present")
require(menu, 'video/menu_background.bik', "animated menu asset detection")
require(menu, 'mVideo->playVideo("video\\\\menu_background.bik")', "animated menu playback")
require(menu, "mVideo->commitFrame();", "animated menu committed frame")

# Keep fail-closed handling for genuinely unresolved OSG/foreign render-target
# textures. Video compatibility must not be implemented by silently accepting
# arbitrary legacy GPU objects at the VSG/MyGUI boundary.
require(render, "skipping unresolved foreign render-target texture", "foreign-texture fail-closed guard")

# If the engine advertises this compatibility facet, the native video bridge is
# part of the source-level minimum. Runtime acceptance remains a separate gate.
if "RenderCompatibilityFacet::UiVideoAndComposition" in engine:
    require(video, "VsgMyGui::Texture", "advertised UI/video compatibility")
    require(video, "updateVulkanVideoTexture", "advertised UI/video compatibility")

print("V4 Vulkan video/menu source contract: PASS")
