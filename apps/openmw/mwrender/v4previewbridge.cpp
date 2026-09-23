#include "v4enginerenderbridge.hpp"
#include "characterpreview.hpp"
#include "v4effectcapture.hpp"
#include "v4skycapture.hpp"
#include "renderingmanager.hpp"
#include "sky.hpp"
#include <components/debug/debuglog.hpp>

#include <components/debug/runtimediagnostics.hpp>
#include <components/vsgmygui/rendermanager.hpp>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/quaternion.hpp>
#include <cstdlib>
#include <set>

namespace MWRender
{
    namespace
    {
        // LocalMap slots grow from zero. Reserve a disjoint namespace for live
        // preview instances; the producer rejects any conflicting view request.
        constexpr std::uint32_t PreviewSlotBase = 1u << 30;
        RenderCore::RenderTargetHandle previewTarget(std::uint32_t slot)
        {
            return RenderCore::RenderTargetHandle::fromParts(slot + 4, 1);
        }
    }

    bool V4EngineRenderBridge::prepareNativePreviewFrame(V4MainFrameSource& source)
    {
        source.previewViews.clear();
        if (std::getenv("OPENMW_V4_LEGACY_PREVIEW_CONTROL")) return true;
        auto& host = mSession->bootstrap().renderer();
        auto* gui = host.guiRenderer();
        if (!gui) return true;
        const auto previews = CharacterPreview::nativeSnapshots();
        std::set<std::uint32_t> live;
        for (const auto& preview : previews)
            live.insert(PreviewSlotBase + static_cast<std::uint32_t>(preview.identity));
        for (auto entry = mNativePreviewEntries.begin(); entry != mNativePreviewEntries.end();)
        {
            if (live.contains(entry->first)) { ++entry; continue; }
            gui->removeTexture(entry->second.textureName);
            if (!host.retireAuxiliarySurface(previewTarget(entry->first)))
            {
                mLastDiagnostic = host.lastDiagnostic();
                return false;
            }
            entry = mNativePreviewEntries.erase(entry);
        }
        for (const auto& preview : previews)
        {
            const auto slot = PreviewSlotBase + static_cast<std::uint32_t>(preview.identity);
            auto& entry = mNativePreviewEntries[slot];
            entry.textureName = preview.textureName;
            // First publication follows successful submission/presentation. Do
            // not sample a merely allocated image after a skipped acquire.
            if (entry.renderedRevision && !entry.published)
            {
                auto image = host.auxiliaryColorImage(previewTarget(slot));
                if (image)
                {
                    // Existing widgets invert UVs for their OSG RTT facade.
                    // Native images are Y-down, so undo that per-texture.
                    entry.published = gui->setExternalTexture(preview.textureName, image,
                        preview.width, preview.height, MyGUI::PixelFormat::R8G8B8A8, true, true) != nullptr;
                    Debug::RuntimeDiagnostics::recordEvent("preview", "native_image_published", preview.textureName,
                        {{"revision", entry.renderedRevision}, {"target", slot + 4}});
                }
            }
            if (!preview.ready || preview.viewportWidth <= 0 || preview.viewportHeight <= 0
                || entry.renderedRevision == preview.revision)
                continue;
            v4_effect_detail::CaptureVisitor visitor(preview.textureName, true, mVfs, &mTextureIdentities, false, true);
            visitor.setTraversalMode(osg::NodeVisitor::TRAVERSE_ACTIVE_CHILDREN);
            visitor.setTraversalMask(~Mask_UpdateVisitor);
            visitor.setTraversalNumber(preview.traversalNumber);
            preview.root->accept(visitor);
            auto captured = visitor.take();
            if (!captured.valid())
            {
                mLastDiagnostic = "native character preview " + preview.textureName + ": " + captured.diagnostic;
                return false;
            }
            auto scene = std::make_shared<RenderCore::IsolatedSceneSnapshot>();
            scene->identity = preview.identity;
            scene->revision = preview.revision;
            scene->viewportExtent = {static_cast<std::uint32_t>(preview.viewportWidth),
                static_cast<std::uint32_t>(preview.viewportHeight)};
            scene->ambient = v4_effect_detail::toGlm(preview.ambient);
            scene->directionalDiffuse = v4_effect_detail::toGlm(preview.diffuse);
            scene->directionalRay = v4_effect_detail::toGlm(preview.directionalRay);
            scene->draws = std::move(captured.draws);
            RenderCore::SingleViewFrameInput::AuxiliaryView view;
            view.kind = RenderCore::ViewKind::Preview;
            view.stableSlot = slot;
            view.extent = {static_cast<std::uint32_t>(preview.width), static_cast<std::uint32_t>(preview.height)};
            view.sampledByMain = true;
            view.camera.view = v4_effect_detail::toGlm(osg::Matrixd(preview.view));
            const glm::mat4 inverseView = glm::inverse(view.camera.view);
            view.camera.worldPosition = glm::dvec3(inverseView[3]);
            view.camera.worldOrientation = glm::quat_cast(glm::mat3(inverseView));
            view.camera.projection.nearPlane = 4.0;
            view.camera.projection.farPlane = 10000.0;
            view.camera.projection.matrix = glm::perspectiveRH_ZO(glm::radians(12.3f),
                static_cast<float>(preview.width) / static_cast<float>(preview.height), 10000.0f, 4.0f);
            view.camera.projection.matrix[1][1] *= -1.0f;
            view.isolatedScene = std::move(scene);
            source.previewViews.push_back(std::move(view));
            Debug::RuntimeDiagnostics::recordEvent("preview", "native_scene_captured", preview.textureName,
                {{"revision", preview.revision}, {"draws", source.previewViews.back().isolatedScene->draws.size()}});
        }
        return true;
    }

    void V4EngineRenderBridge::nativePreviewFramePresented(const V4MainFrameSource& source)
    {
        for (const auto& request : source.previewViews)
        {
            if (!request.stableSlot || !request.isolatedScene) continue;
            const auto entry = mNativePreviewEntries.find(*request.stableSlot);
            if (entry != mNativePreviewEntries.end())
                entry->second.renderedRevision = request.isolatedScene->revision;
        }
    }
    bool V4EngineRenderBridge::prepareNativeSkyFrame(const RenderingManager& rendering, V4MainFrameSource& frame)
    {
        frame.nativeSky.reset();
        if (std::getenv("OPENMW_V4_LEGACY_SKY_CONTROL") || frame.environment.interior) return true;
        const SkyManager* sky = rendering.getSkyManager();
        osg::Group* root = sky ? sky->nativeRenderRoot() : nullptr;
        if (!root) return true; // sky not yet created/enabled by authoritative weather
        if (!mNativeSkyCapture) mNativeSkyCapture = std::make_shared<V4SkyCapture>();
        auto captured = mNativeSkyCapture->capture(*root, mTextureIdentities);
        if (!captured) { mLastDiagnostic = captured.diagnostic; return false; }
        if (captured.snapshot->deferredOcclusionDraws && !mReportedSkyOcclusionDeferral)
        {
            Log(Debug::Warning) << "Native Vulkan sky: sun occlusion-query/glare passes are not yet implemented; "
                               << "atmosphere, night, clouds, moons and solar texture are published";
            mReportedSkyOcclusionDeferral = true;
        }
        frame.nativeSky = std::move(captured.snapshot);
        return true;
    }
}
