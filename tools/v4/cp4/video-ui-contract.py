#!/usr/bin/env python3
from pathlib import Path
import re


def source(path: str) -> str:
    value = Path(path).read_text(encoding="utf-8")
    if not value:
        raise SystemExit(f"empty source file: {path}")
    return value


def require(text: str, token: str, label: str) -> None:
    if token not in text:
        raise SystemExit(f"{label}: missing required source contract: {token}")


def forbid(text: str, token: str, label: str) -> None:
    if token in text:
        raise SystemExit(f"{label}: forbidden source contract returned: {token}")


def require_regex(text: str, pattern: str, label: str) -> re.Match[str]:
    match = re.search(pattern, text, re.DOTALL | re.MULTILINE)
    if not match:
        raise SystemExit(f"{label}: required source pattern was not found")
    return match


video = source("apps/openmw/mwgui/videowidget.cpp")
menu = source("apps/openmw/mwgui/mainmenu.cpp")
menu_header = source("apps/openmw/mwgui/mainmenu.hpp")
window = source("apps/openmw/mwgui/windowmanagerimp.cpp")
loading = source("apps/openmw/mwgui/loadingscreen.cpp")
background = source("apps/openmw/mwgui/backgroundimage.cpp")
buttons = source("components/widgets/imagebutton.cpp")
fonts = source("components/fontloader/fontloader.cpp")
render = source("components/vsgmygui/rendermanager.cpp")
texture = source("components/vsgmygui/texture.cpp")
decoder = source("components/vsgmygui/vfsimagedecoder.cpp")
ui = source("components/render/backend/vsg/uipipeline.cpp")
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

# MyGUI RenderItem stores a raw ITexture pointer. Replaying the startup logo or
# looping menu video must therefore retain the same native Texture object and
# replace only its revisioned VSG data backing.
require(video, "auto* nativeTexture = dynamic_cast<VsgMyGui::Texture*>(mTexture.get())", "stable native video texture")
require(video, "mTexture = std::move(created);", "initial native video texture ownership")
forbid(video, "mTexture = std::make_unique<VsgMyGui::Texture>", "per-play native video texture replacement")

# Startup/company/logo/credits videos are synchronous MyGUI presentation. The
# committed FFmpeg frame must be published before the Vulkan GUI-only present.
require(window, "mVideoWidget->commitFrame();", "startup video commit")
require(window, "mPresentCallback();", "startup Vulkan GUI-only present")
require(engine, "presentCallback = [this] { presentVulkanFrame(0.0f, true); };", "engine Vulkan GUI present callback")

# Animated main-menu playback used to run VideoWidget::update()/playVideo() from
# a wrapper thread while the main thread called commitFrame() and MyGUI collected
# the same widget tree. FFmpeg already owns decode workers; all VideoWidget/MyGUI
# publication and loop restart must stay on the UI thread.
require(menu, 'video/menu_background.bik', "animated menu asset detection")
commit = require_regex(menu, r"void\s+MenuVideo::commitFrame\(\)\s*\{(.*?)\n\s*\}", "animated menu UI-thread loop")
commit_body = commit.group(1)
require(commit_body, "mVideo->update()", "animated menu frame advance")
require(commit_body, 'mVideo->playVideo("video\\\\menu_background.bik")', "animated menu loop restart")
require(commit_body, "mVideo->commitFrame()", "animated menu committed frame")
if not (commit_body.find("mVideo->update()") < commit_body.find("mVideo->playVideo") < commit_body.find("mVideo->commitFrame()")):
    raise SystemExit("animated menu UI-thread loop: expected update -> loop restart -> commit ordering")
forbid(menu, "MenuVideo::run", "animated menu background thread")
forbid(menu, "mThread", "animated menu background thread")
forbid(menu_header, "std::thread", "animated menu background thread")
forbid(menu_header, "mRunning", "animated menu background thread")

# Loading-screen wallpaper assets are ordinary MyGUI/VFS images on Vulkan. The
# legacy OSG framebuffer-copy texture is reachable only when there is no Vulkan
# present callback, so it must not contaminate the startup loading route.
require(loading, "if (!mPresentCallback && !mShowWallpaper && mLastRenderTime < mLoadingOnTime)",
        "loading-screen OSG capture quarantine")
require(loading, "setupCopyFramebufferToTextureCallback();", "legacy loading-screen framebuffer copy")
require(loading, "mPresentCallback();", "Vulkan loading-screen present")

# Static menu fallback, button images, font atlases and the black letterbox texture
# all resolve through the active MyGUI RenderManager. Under Vulkan that is
# VsgMyGui::RenderManager, whose VFS decoder returns TOP_LEFT VSG data and whose
# manual textures are native VSG data after unlock().
require(menu, 'mBackground->setBackgroundImage("textures\\\\menu_morrowind.dds", true, stretch)',
        "static main-menu fallback")
require(menu, 'button->setProperty("ImageNormal", "textures\\\\menu_" + buttonId + ".dds")',
        "main-menu normal button texture")
require(menu, 'button->setProperty("ImageHighlighted", "textures\\\\menu_" + buttonId + "_over.dds")',
        "main-menu highlighted button texture")
require(menu, 'button->setProperty("ImagePushed", "textures\\\\menu_" + buttonId + "_pressed.dds")',
        "main-menu pressed button texture")
require(window, 'MyGUI::RenderManager::getInstance().createTexture("black")', "video/menu black background texture")
require(background, "setImageTexture(image);", "background image active-render-manager route")
require(buttons, "MyGUI::RenderManager::getInstance().getTexture(mImageNormal)", "button active-render-manager route")
require(fonts, "MyGUI::RenderManager::getInstance().createTexture(bitmapPath)", "font atlas active-render-manager route")
require(fonts, "texture->createManual", "font atlas native manual texture")
require(texture, "mData = rgba;", "manual MyGUI texture VSG publication")
require(render, "MyGUI::ITexture* tex = createTexture(name);", "VSG VFS texture allocation")
require(render, "tex->loadFromFile(name);", "VSG VFS texture decode")
require(decoder, "vsgXchange::images::create()", "VSG MyGUI image decoder")
require(decoder, "data->properties.origin = vsg::TOP_LEFT", "VSG MyGUI image orientation")

# Menu buttons, text, loading wallpaper and video are composited through the same
# Vulkan UI pipeline. The source minimum is straight-alpha blending, no culling,
# and no depth test/write, with MyGUI's packed ABGR vertex colour interpreted as
# RGBA8. This is the composition contract needed by button edges and font atlases.
require(ui, "VK_FORMAT_R8G8B8A8_UNORM, 12", "MyGUI vertex colour format")
require(ui, "rasterization->cullMode = VK_CULL_MODE_NONE", "MyGUI two-sided composition")
require(ui, "blendEnable = VK_TRUE", "MyGUI alpha blending")
require(ui, "srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA", "MyGUI source alpha")
require(ui, "dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA", "MyGUI destination alpha")
require(ui, "depth->depthTestEnable = VK_FALSE", "MyGUI depth-test disable")
require(ui, "depth->depthWriteEnable = VK_FALSE", "MyGUI depth-write disable")

# Keep fail-closed handling for genuinely unresolved OSG/foreign render-target
# textures. Video compatibility must not be implemented by silently accepting
# arbitrary legacy GPU objects at the VSG/MyGUI boundary.
require(render, "skipping unresolved foreign render-target texture", "foreign-texture fail-closed guard")

# If the engine advertises this compatibility facet, the native video bridge and
# source-level menu/loading composition proof are mandatory. Runtime acceptance
# remains a separate hardware gate.
if "RenderCompatibilityFacet::UiVideoAndComposition" in engine:
    require(video, "VsgMyGui::Texture", "advertised UI/video compatibility")
    require(video, "updateVulkanVideoTexture", "advertised UI/video compatibility")
    require(menu, "mVideo->update()", "advertised animated-menu compatibility")
    require(loading, "mPresentCallback();", "advertised loading-screen compatibility")
    require(ui, "blendEnable = VK_TRUE", "advertised UI composition compatibility")

print("V4 Vulkan startup/video/menu source contract: PASS")
