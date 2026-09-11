#ifndef GAME_RENDER_LOCALMAP_H
#define GAME_RENDER_LOCALMAP_H

#include <array>
#include <cstdint>
#include <map>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <MyGUI_Types.h>
#include <osg/BoundingBox>
#include <osg/Quat>
#include <osg/ref_ptr>

namespace MWWorld
{
    class CellStore;
}

namespace ESM
{
    struct FogTexture;
}

namespace osg
{
    class Texture2D;
    class Image;
    class Camera;
    class Group;
    class Node;
}

namespace MWRender
{
    class LocalMapRenderToTexture;

    ///
    /// \brief Local map rendering
    ///
    class LocalMap
    {
    public:
        struct NativeMapSurface
        {
            std::uint32_t stableSlot = 0;
            std::string_view logicalIdentity;
            std::string_view mapTextureName;
            std::string_view fogTextureName;
            int segmentX = 0;
            int segmentY = 0;
            int resolution = 0;
            int worldSize = 0;
            float centerX = 0.f;
            float centerY = 0.f;
            std::array<double, 3> upVector{ 0.0, 1.0, 0.0 };
            float zMin = 0.f;
            float zMax = 0.f;
            bool needsRender = false;
            bool mapReady = false;
            std::span<const std::uint8_t> fogRgba;
            std::uint64_t fogRevision = 0;
            bool fogReady = false;
        };

        // V4 configures the neutral native-auxiliary route before LocalMap is
        // constructed. OpenGL never enables it, so AUTO remains entirely on the
        // established OSG RTT path. The active pointer is non-owning and exists
        // only to let the backend bridge consume logical map-surface requests.
        static void configureNativeAuxiliaryRoute(bool enabled) noexcept;
        [[nodiscard]] static LocalMap* activeInstance() noexcept;

        LocalMap(osg::Group* root);
        ~LocalMap();

        /**
         * Clear all savegame-specific data (i.e. fog of war textures)
         */
        void clear();

        /**
         * Request a map render for the given cell. Render textures will be immediately created and can be retrieved
         * with the getMapTexture function.
         */
        void requestMap(const MWWorld::CellStore* cell);

        void addCell(MWWorld::CellStore* cell);
        void removeExteriorCell(int x, int y);

        void removeCell(MWWorld::CellStore* cell);

        osg::ref_ptr<osg::Texture2D> getMapTexture(int x, int y);

        osg::ref_ptr<osg::Texture2D> getFogOfWarTexture(int x, int y);

        [[nodiscard]] bool nativeAuxiliaryRouteEnabled() const noexcept { return mNativeAuxiliaryRoute; }
        [[nodiscard]] std::vector<NativeMapSurface> nativeMapSurfaces();
        [[nodiscard]] bool markNativeMapRendered(std::uint32_t stableSlot) noexcept;
        [[nodiscard]] bool markNativeFogPublished(std::uint32_t stableSlot, std::uint64_t revision) noexcept;
        [[nodiscard]] bool storeNativeMapRgba(std::uint32_t stableSlot, std::vector<std::uint8_t> rgba) noexcept;
        [[nodiscard]] std::span<const std::uint8_t> nativeMapRgba(int x, int y) const noexcept;
        [[nodiscard]] int nativeMapResolution() const noexcept { return mMapResolution; }
        [[nodiscard]] std::string_view nativeMapTextureName(int x, int y) const noexcept;
        [[nodiscard]] std::string_view nativeFogTextureName(int x, int y) const noexcept;

        /**
         * Removes cameras that have already been rendered. Should be called every frame to ensure that
         * we do not render the same map more than once. Note, this cleanup is difficult to implement in an
         * automated fashion, since we can't alter the scene graph structure from within an update callback.
         */
        void cleanupCameras();

        /**
         * Set the position & direction of the player, and returns the position in map space through the reference
         * parameters.
         * @remarks This is used to draw a "fog of war" effect
         * to hide areas on the map the player has not discovered yet.
         */
        void updatePlayer(const osg::Vec3f& position, const osg::Quat& orientation, float& u, float& v, int& x, int& y,
            osg::Vec3f& direction);

        /**
         * Save the fog of war for this cell to its CellStore.
         * @remarks This should be called when unloading a cell, and for all active cells prior to saving the game.
         */
        void saveFogOfWar(MWWorld::CellStore* cell) const;

        /**
         * Get the interior map texture index and normalized position on this texture, given a world position
         */
        void worldToInteriorMapPosition(osg::Vec2f pos, float& nX, float& nY, int& x, int& y) const;

        osg::Vec2f interiorMapToWorldPosition(float nX, float nY, int x, int y) const;

        /**
         * Check if a given position is explored by the player (i.e. not obscured by fog of war)
         */
        bool isPositionExplored(float nX, float nY, int x, int y);

        osg::Group* getRoot();

        MyGUI::IntRect getInteriorGrid() const;

    private:
        osg::ref_ptr<osg::Group> mRoot;
        osg::ref_ptr<osg::Node> mSceneRoot;

        typedef std::vector<osg::ref_ptr<LocalMapRenderToTexture>> RTTVector;
        RTTVector mLocalMapRTTs;

        enum NeighbourCellFlag : std::uint8_t
        {
            NeighbourCellTopLeft = 1,
            NeighbourCellTopCenter = 1 << 1,
            NeighbourCellTopRight = 1 << 2,
            NeighbourCellMiddleLeft = 1 << 3,
            NeighbourCellMiddleRight = 1 << 4,
            NeighbourCellBottomLeft = 1 << 5,
            NeighbourCellBottomCenter = 1 << 6,
            NeighbourCellBottomRight = 1 << 7,
        };

        struct MapSegment
        {
            void initFogOfWar();
            void loadFogOfWar(const ESM::FogTexture& fog);
            void saveFogOfWar(ESM::FogTexture& fog) const;
            void createFogOfWarTexture();

            std::uint8_t mLastRenderNeighbourFlags = 0;
            bool mHasFogState = false;
            osg::ref_ptr<osg::Texture2D> mMapTexture;
            osg::ref_ptr<osg::Texture2D> mFogOfWarTexture;
            osg::ref_ptr<osg::Image> mFogOfWarImage;

            std::uint32_t mNativeStableSlot = 0;
            std::string mNativeLogicalIdentity;
            std::string mNativeMapTextureName;
            std::string mNativeFogTextureName;
            bool mNativeMapRequested = false;
            bool mNativeMapNeedsRender = false;
            bool mNativeMapReady = false;
            bool mNativeFogReady = false;
            const osg::Texture2D* mNativeObservedMapTexture = nullptr;
            std::vector<std::uint8_t> mNativeMapRgba;
            std::uint64_t mNativeFogContentHash = 0;
            std::uint64_t mFogRevision = 0;
            std::uint64_t mPublishedFogRevision = 0;
        };

        typedef std::map<std::pair<int, int>, MapSegment> SegmentMap;
        SegmentMap mExteriorSegments;
        SegmentMap mInteriorSegments;

        int mMapResolution;

        // the dynamic texture is a bottleneck, so don't set this too high
        static const int sFogOfWarResolution = 32;

        // size of a map segment (for exteriors, 1 cell)
        int mMapWorldSize;

        int mCellDistance;

        float mAngle;
        const osg::Vec2f rotatePoint(const osg::Vec2f& point, const osg::Vec2f& center, const float angle) const;

        void requestExteriorMap(const MWWorld::CellStore* cell, MapSegment& segment);
        void requestInteriorMap(const MWWorld::CellStore* cell);

        void setupRenderToTexture(
            int segmentX, int segmentY, float left, float top, const osg::Vec3d& upVector, float zmin, float zmax);
        [[nodiscard]] const MapSegment* findSegment(int x, int y) const noexcept;
        [[nodiscard]] MapSegment* findNativeSegment(std::uint32_t stableSlot) noexcept;

        struct NativeRegistration
        {
            explicit NativeRegistration(LocalMap* owner) noexcept;
            ~NativeRegistration();
            NativeRegistration(const NativeRegistration&) = delete;
            NativeRegistration& operator=(const NativeRegistration&) = delete;

            LocalMap* mOwner = nullptr;
        };

        osg::BoundingBox mBounds;
        osg::Vec2f mCenter;
        bool mInterior;
        bool mNativeAuxiliaryRoute = false;
        NativeRegistration mNativeRegistration{ this };

        std::uint8_t getExteriorNeighbourFlags(int cellX, int cellY) const;
    };

}
#endif