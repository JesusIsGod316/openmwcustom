#include "v4enginerenderbridge.hpp"

#include "globalmap.hpp"
#include "localmap.hpp"

#include <components/render/backend/vsg/vsgruntimehost.hpp>
#include <components/vsgmygui/rendermanager.hpp>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace MWRender
{
    namespace
    {
        [[nodiscard]] RenderCore::RenderTargetHandle localMapTarget(std::uint32_t stableSlot) noexcept
        {
            return RenderCore::RenderTargetHandle::fromParts(stableSlot + 4u, 1u);
        }

        [[nodiscard]] std::optional<RenderCore::CameraState> makeLocalMapCamera(
            const LocalMap::NativeMapSurface& surface)
        {
            if (surface.resolution <= 0 || surface.worldSize <= 0 || !std::isfinite(surface.centerX)
                || !std::isfinite(surface.centerY) || !std::isfinite(surface.zMin) || !std::isfinite(surface.zMax)
                || surface.zMax <= surface.zMin)
                return std::nullopt;

            const glm::dvec3 up(surface.upVector[0], surface.upVector[1], surface.upVector[2]);
            if (!std::isfinite(up.x) || !std::isfinite(up.y) || !std::isfinite(up.z)
                || glm::length(up) < 0.5)
                return std::nullopt;

            constexpr double nearPlane = 5.0;
            const double farPlane = static_cast<double>(surface.zMax - surface.zMin) + 10.0;
            if (!std::isfinite(farPlane) || farPlane <= nearPlane)
                return std::nullopt;

            const glm::dvec3 position(surface.centerX, surface.centerY, static_cast<double>(surface.zMax) + nearPlane);
            const glm::dvec3 target(surface.centerX, surface.centerY, surface.zMin);

            RenderCore::CameraState result;
            result.view = glm::mat4(glm::lookAtRH(position, target, glm::normalize(up)));
            result.worldPosition = position;
            const glm::mat4 cameraWorld = glm::inverse(result.view);
            result.worldOrientation = glm::normalize(glm::quat_cast(glm::mat3(cameraWorld)));

            const float halfWorld = static_cast<float>(surface.worldSize) * 0.5f;
            result.projection.matrix = glm::orthoRH_ZO(
                -halfWorld, halfWorld, -halfWorld, halfWorld, static_cast<float>(farPlane), static_cast<float>(nearPlane));
            // OpenMW's Vulkan contract uses down-Y clip space and reversed Z.
            result.projection.matrix[1][1] *= -1.0f;
            result.projection.depthRange = RenderCore::ClipDepthRange::ZeroToOne;
            result.projection.depthDirection = RenderCore::DepthDirection::Reversed;
            result.projection.yDirection = RenderCore::ClipYDirection::Down;
            result.projection.nearPlane = nearPlane;
            result.projection.farPlane = farPlane;
            return result;
        }
    }

    RenderCore::RenderFrameResult V4EngineRenderBridge::renderMainFrameWithNativeLocalMap(
        const V4MainFrameSource& source)
    {
        mLastDiagnostic.clear();
        if (!mRouteStatus->healthy())
        {
            mLastDiagnostic = mRouteStatus->firstDiagnostic();
            if (mLastDiagnostic.empty())
                mLastDiagnostic = "V4 scene publication route is unhealthy";
            return RenderCore::RenderFrameResult::Failed;
        }
        if (!mLifecycleTaken)
        {
            mLastDiagnostic = "scene lifecycle must be attached before rendering a V4 frame";
            return RenderCore::RenderFrameResult::Failed;
        }

        const std::optional<RenderCore::Extent2D> extent = outputExtent();
        if (!extent)
        {
            mLastDiagnostic = "V4 output extent is temporarily unavailable";
            return RenderCore::RenderFrameResult::Skipped;
        }

        // This method is entered only by the explicit Vulkan gameplay route.
        // Updating an already-constructed LocalMap is intentional: early loading
        // UI may create it before the first world frame, while AUTO/OpenGL never
        // calls this path.
        LocalMap::configureNativeAuxiliaryRoute(true);
        LocalMap* const localMap = LocalMap::activeInstance();
        std::vector<LocalMap::NativeMapSurface> surfaces;
        if (localMap)
        {
            try
            {
                surfaces = localMap->nativeMapSurfaces();
            }
            catch (const std::exception& error)
            {
                mLastDiagnostic = std::string("native local-map lifecycle rejected its logical surfaces: ") + error.what();
                return RenderCore::RenderFrameResult::Failed;
            }
        }

        RenderVsg::VsgRuntimeHost& host = mSession->bootstrap().renderer();
        VsgMyGui::RenderManager* const gui = host.guiRenderer();
        if (!surfaces.empty() && !gui)
        {
            mLastDiagnostic = "native local-map surfaces require the attached VSG MyGUI renderer";
            return RenderCore::RenderFrameResult::Failed;
        }

        std::set<std::uint32_t> desiredSlots;
        for (const LocalMap::NativeMapSurface& surface : surfaces)
        {
            if (!desiredSlots.insert(surface.stableSlot).second)
            {
                mLastDiagnostic = "native local-map frame contains duplicate stable slots";
                return RenderCore::RenderFrameResult::Failed;
            }
            auto [entry, inserted] = mNativeMapUiEntries.try_emplace(surface.stableSlot);
            if (inserted)
            {
                entry->second.logicalIdentity = surface.logicalIdentity;
                entry->second.mapTextureName = surface.mapTextureName;
                entry->second.fogTextureName = surface.fogTextureName;
            }
            else if (entry->second.logicalIdentity != surface.logicalIdentity
                || entry->second.mapTextureName != surface.mapTextureName
                || entry->second.fogTextureName != surface.fogTextureName)
            {
                mLastDiagnostic = "native local-map stable slot changed logical identity or UI texture names";
                return RenderCore::RenderFrameResult::Failed;
            }
        }

        for (auto it = mNativeMapUiEntries.begin(); it != mNativeMapUiEntries.end();)
        {
            if (desiredSlots.contains(it->first))
            {
                ++it;
                continue;
            }
            if (!host.retireAuxiliarySurface(localMapTarget(it->first)))
            {
                mLastDiagnostic = host.lastDiagnostic().empty()
                    ? "native local-map auxiliary target retirement failed"
                    : host.lastDiagnostic();
                return RenderCore::RenderFrameResult::Failed;
            }
            if (gui)
            {
                if (it->second.mapPublished)
                    gui->removeTexture(it->second.mapTextureName);
                if (it->second.fogRevision != 0)
                    gui->removeTexture(it->second.fogTextureName);
            }
            it = mNativeMapUiEntries.erase(it);
        }

        RenderCore::SingleViewFrameInput input;
        input.camera = source.camera;
        input.renderExtent = *extent;
        input.outputExtent = *extent;
        input.environment = source.environment;
        input.simulationTime = source.simulationTime;
        input.frameDelta = source.frameDelta;
        input.lodScale = source.lodScale;
        input.dynamicTransforms = source.dynamicTransforms;
        input.skeletonPoses = source.skeletonPoses;
        input.morphWeights = source.morphWeights;
        input.immediateEffectDraws = source.immediateEffectDraws;
        input.invalidateHistory = source.invalidateHistory || mGuiOnlyFramePresented;

        std::vector<RenderCore::RenderTargetHandle> renderedMapTargets;
        renderedMapTargets.reserve(surfaces.size());
        for (const LocalMap::NativeMapSurface& surface : surfaces)
        {
            if (!surface.needsRender)
                continue;
            const std::optional<RenderCore::CameraState> camera = makeLocalMapCamera(surface);
            if (!camera)
            {
                mLastDiagnostic = "native local-map surface could not produce a finite legacy-compatible map camera";
                return RenderCore::RenderFrameResult::Failed;
            }

            RenderCore::SingleViewFrameInput::AuxiliaryView request;
            request.kind = RenderCore::ViewKind::Map;
            request.camera = *camera;
            request.extent = { static_cast<std::uint32_t>(surface.resolution),
                static_cast<std::uint32_t>(surface.resolution) };
            request.colorFormat = RenderCore::RenderTargetFormat::Rgba8Srgb;
            request.depthFormat = RenderCore::RenderTargetFormat::Depth32Float;
            request.lodScale = 1.0f;
            request.temporal = false;
            request.transient = false;
            // The UI may still reference the previous image while a changed map
            // segment is rerendered. Express the write->sample dependency even
            // though first publication is intentionally delayed until Presented.
            request.sampledByMain = true;
            request.stableSlot = surface.stableSlot;
            renderedMapTargets.push_back(localMapTarget(surface.stableSlot));
            input.auxiliaryViews.push_back(std::move(request));
        }

        const RenderCore::RenderFrameResult result = mSession->renderFrame(input);
        mLastDiagnostic = mSession->lastDiagnostic();
        if (result != RenderCore::RenderFrameResult::Presented)
            return result;

        mGuiOnlyFramePresented = false;
        if (!localMap)
            return result;

        std::optional<std::vector<RenderVsg::VsgRuntimeHost::AuxiliaryRgba8Readback>> mapReadbacks;
        if (!renderedMapTargets.empty())
        {
            mapReadbacks = host.readbackAuxiliaryRgba8(renderedMapTargets);
            if (!mapReadbacks || mapReadbacks->size() != renderedMapTargets.size())
            {
                mLastDiagnostic = host.lastDiagnostic().empty()
                    ? "presented native local-map targets could not be retained for global-map persistence"
                    : host.lastDiagnostic();
                return RenderCore::RenderFrameResult::Failed;
            }
        }

        for (const LocalMap::NativeMapSurface& surface : surfaces)
        {
            NativeMapUiEntry& entry = mNativeMapUiEntries.at(surface.stableSlot);
            if (surface.needsRender)
            {
                const RenderCore::RenderTargetHandle target = localMapTarget(surface.stableSlot);
                const vsg::ref_ptr<vsg::ImageView> image = host.auxiliaryColorImage(target);
                if (!image || !gui
                    || !gui->setExternalTexture(std::string(surface.mapTextureName), image, surface.resolution,
                        surface.resolution))
                {
                    mLastDiagnostic = "presented native local-map target could not be exposed to VSG MyGUI";
                    return RenderCore::RenderFrameResult::Failed;
                }
                entry.mapPublished = true;
                if (!localMap->markNativeMapRendered(surface.stableSlot))
                {
                    mLastDiagnostic = "presented native local-map target lost its logical LocalMap segment";
                    return RenderCore::RenderFrameResult::Failed;
                }

                auto readback = std::find_if(mapReadbacks->begin(), mapReadbacks->end(),
                    [&](const auto& value) { return value.target == target; });
                const RenderCore::Extent2D expected{ static_cast<std::uint32_t>(surface.resolution),
                    static_cast<std::uint32_t>(surface.resolution) };
                if (readback == mapReadbacks->end() || readback->extent != expected
                    || !localMap->storeNativeMapRgba(surface.stableSlot, std::move(readback->rgba)))
                {
                    mLastDiagnostic = "presented native local-map readback did not match its logical segment";
                    return RenderCore::RenderFrameResult::Failed;
                }
            }
            else if (surface.mapReady && !entry.mapPublished)
            {
                const vsg::ref_ptr<vsg::ImageView> image = host.auxiliaryColorImage(localMapTarget(surface.stableSlot));
                if (!image || !gui
                    || !gui->setExternalTexture(std::string(surface.mapTextureName), image, surface.resolution,
                        surface.resolution))
                {
                    mLastDiagnostic = "resident native local-map target could not be restored to VSG MyGUI";
                    return RenderCore::RenderFrameResult::Failed;
                }
                entry.mapPublished = true;
            }

            if (surface.fogRevision != 0 && entry.fogRevision != surface.fogRevision)
            {
                if (!gui || surface.fogRgba.empty()
                    || !gui->setRgba8Texture(std::string(surface.fogTextureName), surface.fogRgba, 32, 32))
                {
                    mLastDiagnostic = "native local-map fog-of-war could not be published to VSG MyGUI";
                    return RenderCore::RenderFrameResult::Failed;
                }
                if (!localMap->markNativeFogPublished(surface.stableSlot, surface.fogRevision))
                {
                    mLastDiagnostic = "native local-map fog publication lost its logical LocalMap segment";
                    return RenderCore::RenderFrameResult::Failed;
                }
                entry.fogRevision = surface.fogRevision;
            }
        }

#if defined(OPENMW_ENABLE_V4_VULKAN_RUNTIME)
        if (GlobalMap* const globalMap = GlobalMap::activeInstance())
            globalMap->flushNativeExploration();
#endif
        return result;
    }
}