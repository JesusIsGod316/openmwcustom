import os
from pathlib import Path

ROOT = Path(os.environ.get("OPENMW_PATCH_ROOT", Path(__file__).resolve().parents[2])).resolve()


def replace_once(path: Path, old: str, new: str, label: str) -> None:
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"V3.18 {label} anchor mismatch in {path}: found {count}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8", newline="\n")


# ---------------------------------------------------------------------------
# Settings: make render resolution a first-class Video setting. P0 intentionally
# exposes bilinear only. NIS is added by a later V3.18 layer after the OpenGL
# compute path is integrated and validated, so selecting an unimplemented scaler
# can never silently fall back.
# ---------------------------------------------------------------------------
video = ROOT / "components/settings/categories/video.hpp"
replace_once(
    video,
    '        SettingValue<float> mFramerateLimit{ mIndex, "Video", "framerate limit", makeMaxSanitizerFloat(0) };\n'
    '        SettingValue<float> mContrast{ mIndex, "Video", "contrast", makeMaxStrictSanitizerFloat(0) };',
    '        SettingValue<float> mFramerateLimit{ mIndex, "Video", "framerate limit", makeMaxSanitizerFloat(0) };\n'
    '        SettingValue<float> mRenderScale{ mIndex, "Video", "render scale", makeClampSanitizerFloat(0.5f, 1.0f) };\n'
    '        SettingValue<std::string> mUpscaler{ mIndex, "Video", "upscaler", makeEnumSanitizerString({ "bilinear" }) };\n'
    '        SettingValue<float> mUpscalerSharpness{ mIndex, "Video", "upscaler sharpness", makeClampSanitizerFloat(0.0f, 1.0f) };\n'
    '        SettingValue<float> mContrast{ mIndex, "Video", "contrast", makeMaxStrictSanitizerFloat(0) };',
    "video settings",
)

settings_default = ROOT / "files/settings-default.cfg"
replace_once(
    settings_default,
    '# Maximum frames per second. 0.0 is unlimited, or >0.0 to limit.\n'
    'framerate limit = 300\n\n'
    '# Game video contrast.  (>0.0).  No effect in Linux.',
    '# Maximum frames per second. 0.0 is unlimited, or >0.0 to limit.\n'
    'framerate limit = 300\n\n'
    '# Internal 3D render resolution as a fraction of the window/display resolution.\n'
    '# The HUD/UI remains at the native output resolution. Range: 0.5 to 1.0.\n'
    'render scale = 1.0\n\n'
    '# Spatial upscaler used when render scale is below 1.0. V3.18 P0 supports bilinear.\n'
    'upscaler = bilinear\n\n'
    '# Reserved scaler sharpness control. Bilinear ignores this; NIS will consume it later.\n'
    'upscaler sharpness = 0.2\n\n'
    '# Game video contrast.  (>0.0).  No effect in Linux.',
    "default settings",
)

# ---------------------------------------------------------------------------
# PostProcessor: output size remains mWidth/mHeight. renderWidth/renderHeight are
# the low-resolution 3D/PostFX dimensions. Stereo remains native for P0 so the
# existing multiview path is not accidentally changed by the first scaler layer.
# ---------------------------------------------------------------------------
post_hpp = ROOT / "apps/openmw/mwrender/postprocessor.hpp"
replace_once(
    post_hpp,
    '        int renderWidth() const;\n        int renderHeight() const;\n\n        void triggerShaderReload();',
    '        int renderWidth() const;\n        int renderHeight() const;\n        int outputWidth() const { return mWidth; }\n        int outputHeight() const { return mHeight; }\n        bool renderScalingActive() const;\n\n        void triggerShaderReload();',
    "postprocessor declarations",
)

post_cpp = ROOT / "apps/openmw/mwrender/postprocessor.cpp"
replace_once(
    post_cpp,
    '#include <algorithm>\n#include <chrono>\n#include <thread>',
    '#include <algorithm>\n#include <chrono>\n#include <cmath>\n#include <thread>',
    "postprocessor cmath include",
)
replace_once(
    post_cpp,
    '        mStateUpdater->setResolution(osg::Vec2f(\n'
    '            static_cast<float>(cv->getViewport()->width()), static_cast<float>(cv->getViewport()->height())));',
    '        // V3.18: PostFX/shader resolution follows the internal 3D render target,\n'
    '        // while the HUD camera and final presentation stay at native output size.\n'
    '        mStateUpdater->setResolution(\n'
    '            osg::Vec2f(static_cast<float>(renderWidth()), static_cast<float>(renderHeight())));',
    "postprocessor state resolution",
)
replace_once(
    post_cpp,
    '    int PostProcessor::renderWidth() const\n'
    '    {\n'
    '        if (Stereo::getStereo())\n'
    '            return Stereo::Manager::instance().eyeResolution().x();\n'
    '        return mWidth;\n'
    '    }\n\n'
    '    int PostProcessor::renderHeight() const\n'
    '    {\n'
    '        if (Stereo::getStereo())\n'
    '            return Stereo::Manager::instance().eyeResolution().y();\n'
    '        return mHeight;\n'
    '    }',
    '    int PostProcessor::renderWidth() const\n'
    '    {\n'
    '        if (Stereo::getStereo())\n'
    '            return Stereo::Manager::instance().eyeResolution().x();\n'
    '        const float scale = Settings::video().mRenderScale;\n'
    '        return std::max(1, static_cast<int>(std::lround(static_cast<double>(mWidth) * scale)));\n'
    '    }\n\n'
    '    int PostProcessor::renderHeight() const\n'
    '    {\n'
    '        if (Stereo::getStereo())\n'
    '            return Stereo::Manager::instance().eyeResolution().y();\n'
    '        const float scale = Settings::video().mRenderScale;\n'
    '        return std::max(1, static_cast<int>(std::lround(static_cast<double>(mHeight) * scale)));\n'
    '    }\n\n'
    '    bool PostProcessor::renderScalingActive() const\n'
    '    {\n'
    '        return !Stereo::getStereo() && renderWidth() != outputWidth() && renderHeight() != outputHeight();\n'
    '    }',
    "postprocessor scaled dimensions",
)

# RenderingManager can refresh projection/FOV outside PostProcessor::resize().
# Keep shader screen-resolution uniforms on the internal render size whenever a
# PostProcessor exists; projection aspect remains based on the native output and
# therefore does not change image geometry.
rendering_cpp = ROOT / "apps/openmw/mwrender/renderingmanager.cpp"
replace_once(
    rendering_cpp,
    '        else\n        {\n            setScreenRes(width, height);\n        }\n\n'
    '        // Since our fog is not radial yet, we should take FOV in account, otherwise terrain near viewing distance may',
    '        else\n        {\n            if (mPostProcessor)\n                setScreenRes(mPostProcessor->renderWidth(), mPostProcessor->renderHeight());\n            else\n                setScreenRes(width, height);\n        }\n\n'
    '        // Since our fog is not radial yet, we should take FOV in account, otherwise terrain near viewing distance may',
    "rendering manager screen resolution",
)

# ---------------------------------------------------------------------------
# Main scene viewport: the viewer camera retains the native output viewport for
# projection/UI/event semantics. PingPongCull pushes an internal-sized viewport
# only while culling/drawing the 3D scene into the low-resolution FBO.
# ---------------------------------------------------------------------------
ping_cull = ROOT / "apps/openmw/mwrender/pingpongcull.cpp"
replace_once(
    ping_cull,
    '    PingPongCull::PingPongCull(PostProcessor* pp)\n'
    '        : mViewportStateset(nullptr)\n'
    '        , mPostProcessor(pp)\n'
    '    {\n'
    '        if (Stereo::getStereo())\n'
    '        {\n'
    '            mViewportStateset = new osg::StateSet();\n'
    '            mViewport = new osg::Viewport;\n'
    '            mViewportStateset->setAttribute(mViewport);\n'
    '        }\n'
    '    }',
    '    PingPongCull::PingPongCull(PostProcessor* pp)\n'
    '        : mViewportStateset(new osg::StateSet())\n'
    '        , mViewport(new osg::Viewport)\n'
    '        , mPostProcessor(pp)\n'
    '    {\n'
    '        // V3.18 uses this state for both stereo and low-resolution 3D rendering.\n'
    '        // HUD/final presentation is a separate native-resolution camera/pass.\n'
    '        mViewportStateset->setAttribute(mViewport);\n'
    '    }',
    "pingpong cull viewport state",
)

# ---------------------------------------------------------------------------
# PostFX canvas: all effect passes render at internal resolution. If the final
# output viewport is larger, the last post-process result is kept in an internal
# ping-pong texture and a single native-output bilinear resolve is performed.
# This is the key architectural separation needed before NIS/DLSS providers.
# ---------------------------------------------------------------------------
canvas_cpp = ROOT / "apps/openmw/mwrender/pingpongcanvas.cpp"
replace_once(
    canvas_cpp,
    '        auto* resolveViewport = state.getCurrentViewport();\n\n'
    '        if (filtered.empty() || !mPostprocessing)',
    '        auto* resolveViewport = state.getCurrentViewport();\n'
    '        const bool scaledOutput = !Stereo::getStereo() && resolveViewport\n'
    '            && (mTextureScene->getTextureWidth() != static_cast<int>(resolveViewport->width())\n'
    '                || mTextureScene->getTextureHeight() != static_cast<int>(resolveViewport->height()));\n\n'
    '        if (filtered.empty() || !mPostprocessing)',
    "canvas scaled-output detection",
)
replace_once(
    canvas_cpp,
    '            if (Stereo::getStereo())\n'
    '                mRenderViewport\n'
    '                    = new osg::Viewport(0, 0, mTextureScene->getTextureWidth(), mTextureScene->getTextureHeight());\n'
    '            else\n'
    '                mRenderViewport = nullptr;',
    '            // V3.18: every scene/PostFX intermediate pass uses the internal\n'
    '            // render-target size. The final presentation pass restores the\n'
    '            // native output viewport.\n'
    '            mRenderViewport\n'
    '                = new osg::Viewport(0, 0, mTextureScene->getTextureWidth(), mTextureScene->getTextureHeight());',
    "canvas internal viewport",
)
replace_once(
    canvas_cpp,
    '                else if (pass.mResolve && index == filtered.back())\n'
    '                {\n'
    '                    bindDestinationFbo();\n'
    '                    if (!destinationFbo && !Stereo::getMultiview())\n'
    '                    {\n'
    '                        resolveViewport->apply(state);\n'
    '                    }\n'
    '                }',
    '                else if (pass.mResolve && index == filtered.back() && !scaledOutput)\n'
    '                {\n'
    '                    bindDestinationFbo();\n'
    '                    if (!destinationFbo && !Stereo::getMultiview())\n'
    '                    {\n'
    '                        resolveViewport->apply(state);\n'
    '                    }\n'
    '                }',
    "canvas defer final resolve while scaled",
)
replace_once(
    canvas_cpp,
    '        if (Stereo::getMultiview())\n'
    '        {\n'
    '            ext->glBindFramebuffer(GL_DRAW_FRAMEBUFFER_EXT, 0);',
    '        if (scaledOutput)\n'
    '        {\n'
    '            // All user/internal PostFX has completed at internal resolution.\n'
    '            // Present exactly once at native resolution using the existing\n'
    '            // linear-clamp fullscreen path. NIS replaces only this stage.\n'
    '            bindDestinationFbo();\n'
    '            resolveViewport->apply(state);\n'
    '            state.pushStateSet(mFallbackStateSet);\n'
    '            state.apply();\n'
    '            osg::Texture* finalTexture = mTextureScene;\n'
    '            if (lastShader != 0)\n'
    '                finalTexture = (osg::Texture*)mFbos[lastShader - GL_COLOR_ATTACHMENT0_EXT]\n'
    '                                   ->getAttachment(osg::Camera::COLOR_BUFFER0)\n'
    '                                   .getTexture();\n'
    '            state.applyTextureAttribute(0, finalTexture);\n'
    '            drawGeometry(renderInfo);\n'
    '            state.popStateSet();\n'
    '            state.apply();\n'
    '            lastApplied = destinationHandle;\n'
    '        }\n\n'
    '        if (Stereo::getMultiview())\n'
    '        {\n'
    '            ext->glBindFramebuffer(GL_DRAW_FRAMEBUFFER_EXT, 0);',
    "canvas native bilinear presentation",
)

# Build identity marker guaranteed to survive in the executable and make the
# artifact gate/version inspection unambiguous even before NIS is added.
engine = ROOT / "apps/openmw/engine.cpp"
engine_text = engine.read_text(encoding="utf-8")
identity_anchor = 'openmw-custom-v3.17'
if identity_anchor not in engine_text:
    raise RuntimeError("V3.18 expected final V3.17 executable identity marker")
engine.write_text(
    engine_text.replace(identity_anchor, identity_anchor + ' / openmw-custom-v3.18-render-scale-p0', 1),
    encoding="utf-8",
    newline="\n",
)

# Sanity checks on the generated result.
checks = {
    video: ("mRenderScale", 'mUpscaler{ mIndex, "Video", "upscaler"'),
    post_cpp: ("renderScalingActive", "Settings::video().mRenderScale"),
    ping_cull: ("V3.18 uses this state",),
    canvas_cpp: ("scaledOutput", "Present exactly once at native resolution"),
    engine: ("openmw-custom-v3.18-render-scale-p0",),
}
for path, markers in checks.items():
    text = path.read_text(encoding="utf-8")
    for marker in markers:
        if marker not in text:
            raise RuntimeError(f"V3.18 generated source missing {marker!r} in {path}")

print("V3.18 P0 internal render-resolution + native-UI + bilinear presentation layer applied")
