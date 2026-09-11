#ifndef GAME_RENDER_GLOBALMAP_H
#define GAME_RENDER_GLOBALMAP_H

#include <cstdint>
#include <map>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <osg/ref_ptr>

namespace osg
{
    class Texture2D;
    class Image;
    class Group;
    class Camera;
}

namespace ESM
{
    struct GlobalMap;
}

namespace SceneUtil
{
    class WorkQueue;
}

namespace MWRender
{

    class CreateMapWorkItem;

    class GlobalMap
    {
    public:
        GlobalMap(osg::Group* root, SceneUtil::WorkQueue* workQueue);
        ~GlobalMap();

        void render();

        int getWidth() const { return mWidth; }
        int getHeight() const { return mHeight; }

        void worldPosToImageSpace(float x, float z, float& imageX, float& imageY);

        void exploreCell(int cellX, int cellY, osg::ref_ptr<osg::Texture2D> localMapTexture);

#if defined(OPENMW_ENABLE_V4_VULKAN_RUNTIME)
        // Explicit Vulkan keeps global-map composition CPU/save authoritative:
        // local-map RGBA readbacks are resampled into the same overlay image
        // saved by OpenMW, while MyGUI receives native VSG textures by name.
        [[nodiscard]] static GlobalMap* activeInstance() noexcept;
        void flushNativeExploration();
        [[nodiscard]] bool exploreCellNative(int cellX, int cellY, std::span<const std::uint8_t> localMapRgba,
            int sourceWidth, int sourceHeight);
        [[nodiscard]] bool publishNativeTextures();
        [[nodiscard]] std::string_view nativeBaseTextureName() const noexcept;
        [[nodiscard]] std::string_view nativeOverlayTextureName() const noexcept;
        void clearNative();
        void readNative(ESM::GlobalMap& map);
#endif

        /// Clears the overlay
        void clear();

        /**
         * Removes cameras that have already been rendered. Should be called every frame to ensure that
         * we do not render the same map more than once. Note, this cleanup is difficult to implement in an
         * automated fashion, since we can't alter the scene graph structure from within an update callback.
         */
        void cleanupCameras();

        void removeCamera(osg::Camera* cam);

        bool copyResult(osg::Camera* cam, unsigned int frame);

        /**
         * Mark a camera for cleanup in the next update. For internal use only.
         */
        void markForRemoval(osg::Camera* camera);

        void write(ESM::GlobalMap& map);
        void read(ESM::GlobalMap& map);

        osg::ref_ptr<osg::Texture2D> getBaseTexture();
        osg::ref_ptr<osg::Texture2D> getOverlayTexture();

        void ensureLoaded();

        void asyncWritePng();

    private:
        struct WritePng;

        /**
         * Request rendering a 2d quad onto mOverlayTexture.
         * x, y, width and height are the destination coordinates (top-left coordinate origin)
         * @param cpuCopy copy the resulting render onto mOverlayImage as well?
         */
        void requestOverlayTextureUpdate(int x, int y, int width, int height, osg::ref_ptr<osg::Texture2D> texture,
            bool clear, bool cpuCopy, float srcLeft = 0.f, float srcTop = 0.f, float srcRight = 1.f,
            float srcBottom = 1.f);

        osg::ref_ptr<osg::Group> mRoot;

        typedef std::vector<osg::ref_ptr<osg::Camera>> CameraVector;
        CameraVector mActiveCameras;

        CameraVector mCamerasPendingRemoval;

        struct ImageDest
        {
            ImageDest()
                : mX(0)
                , mY(0)
                , mFrameDone(0)
            {
            }

            osg::ref_ptr<osg::Image> mImage;
            int mX, mY;
            unsigned int mFrameDone;
        };

        typedef std::map<osg::ref_ptr<osg::Camera>, ImageDest> ImageDestMap;

        ImageDestMap mPendingImageDest;

        osg::ref_ptr<osg::Texture2D> mBaseTexture;
        osg::ref_ptr<osg::Texture2D> mAlphaTexture;

        // GPU copy of overlay
        // Note, uploads are pushed through a Camera, instead of through mOverlayImage
        osg::ref_ptr<osg::Texture2D> mOverlayTexture;

        // CPU copy of overlay
        osg::ref_ptr<osg::Image> mOverlayImage;

        osg::ref_ptr<SceneUtil::WorkQueue> mWorkQueue;
        osg::ref_ptr<CreateMapWorkItem> mWorkItem;
        osg::ref_ptr<WritePng> mWritePng;

        int mWidth;
        int mHeight;

        int mMinX, mMaxX, mMinY, mMaxY;

#if defined(OPENMW_ENABLE_V4_VULKAN_RUNTIME)
        struct NativeRegistration
        {
            explicit NativeRegistration(GlobalMap* owner) noexcept;
            ~NativeRegistration();
            NativeRegistration(const NativeRegistration&) = delete;
            NativeRegistration& operator=(const NativeRegistration&) = delete;
            GlobalMap* mOwner = nullptr;
        };

        NativeRegistration mNativeRegistration{ this };
        std::set<std::pair<int, int>> mNativePendingExploredCells;
        std::uint64_t mNativeOverlayRevision = 1;
        std::uint64_t mNativePublishedOverlayRevision = 0;
        bool mNativeBasePublished = false;
#endif
    };

}

#endif