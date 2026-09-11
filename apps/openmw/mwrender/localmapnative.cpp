#include "localmap.hpp"

#include <limits>
#include <stdexcept>
#include <unordered_map>

#include <osg/Image>
#include <osg/Node>

#if defined(OPENMW_ENABLE_V4_VULKAN_RUNTIME)
#include <MyGUI_RenderManager.h>
#include <components/vsgmygui/rendermanager.hpp>
#endif

#include "../mwbase/environment.hpp"
#include "../mwbase/world.hpp"

#include "../mwworld/cell.hpp"
#include "../mwworld/cellstore.hpp"
#include "../mwworld/ptr.hpp"

namespace MWRender
{
    namespace
    {
        bool sNativeAuxiliaryRoute = false;
        LocalMap* sActiveLocalMap = nullptr;

        constexpr std::uint64_t Fnv64Offset = 1469598103934665603ull;
        constexpr std::uint64_t Fnv64Prime = 1099511628211ull;
        constexpr std::uint32_t StableSlotMask = 0x3fffffffu;

        [[nodiscard]] std::uint64_t hashBytes(std::span<const std::uint8_t> bytes) noexcept
        {
            std::uint64_t result = Fnv64Offset;
            for (const std::uint8_t value : bytes)
            {
                result ^= value;
                result *= Fnv64Prime;
            }
            return result;
        }

        [[nodiscard]] std::uint32_t stableSlot(std::string_view identity) noexcept
        {
            std::uint64_t hash = Fnv64Offset;
            for (const unsigned char value : identity)
            {
                hash ^= value;
                hash *= Fnv64Prime;
            }
            return static_cast<std::uint32_t>(hash ^ (hash >> 32)) & StableSlotMask;
        }

        [[nodiscard]] const MWWorld::Cell* currentCell()
        {
            MWBase::World* const world = MWBase::Environment::get().getWorld();
            if (!world)
                return nullptr;
            const MWWorld::Ptr player = world->getPlayerPtr();
            if (player.isEmpty() || !player.getCell())
                return nullptr;
            return player.getCell()->getCell();
        }

        [[nodiscard]] std::string surfaceIdentity(
            const MWWorld::Cell& cell, bool interior, int segmentX, int segmentY)
        {
            std::string result = "local-map/world:" + cell.getWorldSpace().serializeText();
            if (interior)
                result += "/interior:" + cell.getId().serializeText();
            else
                result += "/exterior";
            result += "/segment:" + std::to_string(segmentX) + "," + std::to_string(segmentY);
            return result;
        }
    }

    void LocalMap::configureNativeAuxiliaryRoute(bool enabled) noexcept
    {
        sNativeAuxiliaryRoute = enabled;
        if (sActiveLocalMap)
            sActiveLocalMap->mNativeAuxiliaryRoute = enabled;
    }

    LocalMap* LocalMap::activeInstance() noexcept
    {
        return sActiveLocalMap;
    }

    LocalMap::NativeRegistration::NativeRegistration(LocalMap* owner) noexcept
        : mOwner(owner)
    {
        if (!owner)
            return;

        bool enabled = sNativeAuxiliaryRoute;
#if defined(OPENMW_ENABLE_V4_VULKAN_RUNTIME)
        // LocalMap is constructed after WindowManager has installed its actual
        // MyGUI platform. Detecting the concrete renderer here makes explicit
        // Vulkan native from the first HUD/map update while an AUTO/OpenGL route
        // in the same V4-capable executable remains entirely on OSG.
        enabled = dynamic_cast<VsgMyGui::RenderManager*>(&MyGUI::RenderManager::getInstance()) != nullptr;
#endif
        sNativeAuxiliaryRoute = enabled;
        owner->mNativeAuxiliaryRoute = enabled;
        sActiveLocalMap = owner;
    }

    LocalMap::NativeRegistration::~NativeRegistration()
    {
        if (sActiveLocalMap == mOwner)
            sActiveLocalMap = nullptr;
    }

    const LocalMap::MapSegment* LocalMap::findSegment(int x, int y) const noexcept
    {
        const SegmentMap& segments = mInterior ? mInteriorSegments : mExteriorSegments;
        const auto found = segments.find({ x, y });
        return found == segments.end() ? nullptr : &found->second;
    }

    LocalMap::MapSegment* LocalMap::findNativeSegment(std::uint32_t slot) noexcept
    {
        const auto find = [slot](SegmentMap& segments) -> MapSegment* {
            for (auto& [coordinates, segment] : segments)
            {
                (void)coordinates;
                if (segment.mNativeMapRequested && segment.mNativeStableSlot == slot)
                    return &segment;
            }
            return nullptr;
        };
        if (MapSegment* result = find(mExteriorSegments))
            return result;
        return find(mInteriorSegments);
    }

    std::vector<LocalMap::NativeMapSurface> LocalMap::nativeMapSurfaces()
    {
        std::vector<NativeMapSurface> result;
        if (!mNativeAuxiliaryRoute)
            return result;

        const MWWorld::Cell* const cell = currentCell();
        if (!cell || cell->isExterior() == mInterior)
            return result;

        SegmentMap& segments = mInterior ? mInteriorSegments : mExteriorSegments;
        result.reserve(segments.size());
        std::unordered_map<std::uint32_t, std::string_view> slots;
        slots.reserve(segments.size());

        osg::BoundingSphere exteriorBounds;
        if (!mInterior && mSceneRoot)
            exteriorBounds = mSceneRoot->getBound();

        for (auto& [coordinates, segment] : segments)
        {
            const osg::Texture2D* const currentMapTexture = segment.mMapTexture.get();
            if (!currentMapTexture && !segment.mNativeMapRequested)
                continue;

            const std::string identity = surfaceIdentity(*cell, mInterior, coordinates.first, coordinates.second);
            if (segment.mNativeLogicalIdentity != identity)
            {
                segment.mNativeLogicalIdentity = identity;
                segment.mNativeStableSlot = stableSlot(identity);
                segment.mNativeMapTextureName = "openmw-v4-local-map-" + std::to_string(segment.mNativeStableSlot);
                segment.mNativeFogTextureName = "openmw-v4-local-map-fog-" + std::to_string(segment.mNativeStableSlot);
                segment.mNativeMapRequested = true;
                segment.mNativeMapNeedsRender = true;
                segment.mNativeMapReady = false;
                segment.mNativeFogReady = false;
                segment.mNativeObservedMapTexture = nullptr;
                segment.mNativeMapRgba.clear();
                segment.mNativeFogContentHash = 0;
                segment.mFogRevision = 0;
                segment.mPublishedFogRevision = 0;
            }

            if (currentMapTexture && currentMapTexture != segment.mNativeObservedMapTexture)
            {
                segment.mNativeObservedMapTexture = currentMapTexture;
                segment.mNativeMapRequested = true;
                segment.mNativeMapNeedsRender = true;
                segment.mNativeMapReady = false;
                segment.mNativeMapRgba.clear();
            }

            if (!segment.mNativeMapRequested)
                continue;

            const auto [collision, inserted]
                = slots.emplace(segment.mNativeStableSlot, std::string_view(segment.mNativeLogicalIdentity));
            if (!inserted && collision->second != segment.mNativeLogicalIdentity)
                throw std::runtime_error("LocalMap native stable-slot collision between persistent logical surfaces");

            std::span<const std::uint8_t> fog;
            if (segment.mFogOfWarImage && segment.mFogOfWarImage->data())
            {
                constexpr std::size_t FogBytes = static_cast<std::size_t>(sFogOfWarResolution)
                    * static_cast<std::size_t>(sFogOfWarResolution) * sizeof(std::uint32_t);
                fog = { reinterpret_cast<const std::uint8_t*>(segment.mFogOfWarImage->data()), FogBytes };
                const std::uint64_t contentHash = hashBytes(fog);
                if (contentHash != segment.mNativeFogContentHash)
                {
                    if (segment.mFogRevision == std::numeric_limits<std::uint64_t>::max())
                        throw std::runtime_error("LocalMap native fog revision exhausted");
                    segment.mNativeFogContentHash = contentHash;
                    ++segment.mFogRevision;
                    segment.mNativeFogReady = false;
                }
            }

            float centerX = 0.f;
            float centerY = 0.f;
            std::array<double, 3> up{ 0.0, 1.0, 0.0 };
            float zMin = 0.f;
            float zMax = 0.f;
            if (!mInterior)
            {
                centerX = coordinates.first * mMapWorldSize + mMapWorldSize / 2.f;
                centerY = coordinates.second * mMapWorldSize + mMapWorldSize / 2.f;
                zMin = exteriorBounds.center().z() - exteriorBounds.radius();
                zMax = exteriorBounds.center().z() + exteriorBounds.radius();
            }
            else
            {
                const osg::Vec2f minimum(mBounds.xMin(), mBounds.yMin());
                const osg::Vec2f start = minimum
                    + osg::Vec2f(static_cast<float>(mMapWorldSize * coordinates.first),
                        static_cast<float>(mMapWorldSize * coordinates.second));
                const osg::Vec2f unrotatedCenter
                    = start + osg::Vec2f(mMapWorldSize / 2.f, mMapWorldSize / 2.f);
                const osg::Vec2f offset = unrotatedCenter - mCenter;
                const osg::Quat cameraOrientation(mAngle, osg::Vec3d(0, 0, -1));
                const osg::Vec3f rotated = cameraOrientation * osg::Vec3f(offset.x(), offset.y(), 0.f);
                centerX = rotated.x() + mCenter.x();
                centerY = rotated.y() + mCenter.y();
                up = { std::sin(mAngle), std::cos(mAngle), 0.0 };
                zMin = mBounds.zMin();
                zMax = mBounds.zMax();
            }

            result.push_back(NativeMapSurface{
                .stableSlot = segment.mNativeStableSlot,
                .logicalIdentity = segment.mNativeLogicalIdentity,
                .mapTextureName = segment.mNativeMapTextureName,
                .fogTextureName = segment.mNativeFogTextureName,
                .segmentX = coordinates.first,
                .segmentY = coordinates.second,
                .resolution = mMapResolution,
                .worldSize = mMapWorldSize,
                .centerX = centerX,
                .centerY = centerY,
                .upVector = up,
                .zMin = zMin,
                .zMax = zMax,
                .needsRender = segment.mNativeMapNeedsRender,
                .mapReady = segment.mNativeMapReady,
                .fogRgba = fog,
                .fogRevision = segment.mFogRevision,
                .fogReady = segment.mNativeFogReady && segment.mPublishedFogRevision == segment.mFogRevision,
            });
        }
        return result;
    }

    bool LocalMap::markNativeMapRendered(std::uint32_t stableSlotValue) noexcept
    {
        MapSegment* const segment = findNativeSegment(stableSlotValue);
        if (!segment)
            return false;
        segment->mNativeMapNeedsRender = false;
        segment->mNativeMapReady = true;
        return true;
    }

    bool LocalMap::markNativeFogPublished(std::uint32_t stableSlotValue, std::uint64_t revision) noexcept
    {
        MapSegment* const segment = findNativeSegment(stableSlotValue);
        if (!segment || revision == 0 || segment->mFogRevision != revision)
            return false;
        segment->mPublishedFogRevision = revision;
        segment->mNativeFogReady = true;
        return true;
    }

    bool LocalMap::storeNativeMapRgba(
        std::uint32_t stableSlotValue, std::vector<std::uint8_t> rgba) noexcept
    {
        MapSegment* const segment = findNativeSegment(stableSlotValue);
        if (!segment || !segment->mNativeMapReady || mMapResolution <= 0)
            return false;
        const std::size_t resolution = static_cast<std::size_t>(mMapResolution);
        if (resolution > std::numeric_limits<std::size_t>::max() / resolution
            || resolution * resolution > std::numeric_limits<std::size_t>::max() / 4u
            || rgba.size() != resolution * resolution * 4u)
            return false;
        segment->mNativeMapRgba = std::move(rgba);
        return true;
    }

    std::span<const std::uint8_t> LocalMap::nativeMapRgba(int x, int y) const noexcept
    {
        const MapSegment* const segment = findSegment(x, y);
        if (!segment || !segment->mNativeMapReady || segment->mNativeMapRgba.empty())
            return {};
        return segment->mNativeMapRgba;
    }

    std::string_view LocalMap::nativeMapTextureName(int x, int y) const noexcept
    {
        const MapSegment* const segment = findSegment(x, y);
        return segment && segment->mNativeMapReady ? std::string_view(segment->mNativeMapTextureName) : std::string_view{};
    }

    std::string_view LocalMap::nativeFogTextureName(int x, int y) const noexcept
    {
        const MapSegment* const segment = findSegment(x, y);
        return segment && segment->mNativeFogReady && segment->mPublishedFogRevision == segment->mFogRevision
            ? std::string_view(segment->mNativeFogTextureName)
            : std::string_view{};
    }
}
