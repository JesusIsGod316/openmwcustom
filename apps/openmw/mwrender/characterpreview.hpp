#ifndef MWRENDER_CHARACTERPREVIEW_H
#define MWRENDER_CHARACTERPREVIEW_H

#include <memory>
#include <cstdint>
#include <vector>
#include <osg/Matrixf>
#include <osg/ref_ptr>

#include <osg/PositionAttitudeTransform>

#include <components/esm3/loadnpc.hpp>

#include <components/resource/resourcesystem.hpp>

#include "../mwworld/ptr.hpp"

namespace osg
{
    class Texture2D;
    class Camera;
    class Group;
    class Viewport;
    class StateSet;
}

namespace MWRender
{

    class NpcAnimation;
    class DrawOnceCallback;
    class CharacterPreviewRTTNode;

    class CharacterPreview
    {
    public:
        CharacterPreview(osg::Group* parent, Resource::ResourceSystem* resourceSystem, const MWWorld::Ptr& character,
            int sizeX, int sizeY, const osg::Vec3f& position, const osg::Vec3f& lookAt);
        virtual ~CharacterPreview();

        int getTextureWidth() const;
        int getTextureHeight() const;

        void redraw();

        struct NativeSnapshot
        {
            std::uint64_t identity = 0;
            std::uint64_t revision = 0;
            std::string textureName;
            osg::ref_ptr<osg::Node> root;
            osg::Matrixf view;
            osg::Vec4f ambient;
            osg::Vec4f diffuse;
            osg::Vec3f directionalRay;
            int width = 0, height = 0, viewportWidth = 0, viewportHeight = 0;
            unsigned int traversalNumber = 0;
            bool ready = false;
        };
        // Main-thread-only observation after the canonical update traversal.
        // Returned OSG references are consumed immediately by the producer;
        // only copied neutral geometry reaches the renderer/Lua-release boundary.
        static std::vector<NativeSnapshot> nativeSnapshots();

        void rebuild();

        osg::ref_ptr<osg::Texture2D> getTexture();
        /// Get the osg::StateSet required to render the texture correctly, if any.
        osg::StateSet* getTextureStateSet() { return mTextureStateSet; }

    private:
        CharacterPreview(const CharacterPreview&);
        CharacterPreview& operator=(const CharacterPreview&);

    protected:
        virtual bool renderHeadOnly() { return false; }
        void setBlendMode();
        virtual void onSetup();

        osg::ref_ptr<osg::Group> mParent;
        Resource::ResourceSystem* mResourceSystem;
        osg::ref_ptr<osg::StateSet> mTextureStateSet;
        osg::ref_ptr<DrawOnceCallback> mDrawOnceCallback;
        osg::ref_ptr<CharacterPreviewRTTNode> mRTTNode;

        osg::Vec3f mPosition;
        osg::Vec3f mLookAt;

        MWWorld::Ptr mCharacter;

        osg::ref_ptr<MWRender::NpcAnimation> mAnimation;
        osg::ref_ptr<osg::PositionAttitudeTransform> mNode;
        std::string mCurrentAnimGroup;

        int mSizeX;
        int mSizeY;
        std::uint64_t mNativeIdentity = 0;
        std::uint64_t mNativeRevision = 1;
        std::string mNativeTextureName;
        osg::Vec4f mNativeAmbient;
        osg::Vec4f mNativeDiffuse;
        osg::Vec3f mNativeDirectionalRay;
    };

    class InventoryPreview : public CharacterPreview
    {
    public:
        InventoryPreview(osg::Group* parent, Resource::ResourceSystem* resourceSystem, const MWWorld::Ptr& character);

        void updatePtr(const MWWorld::Ptr& ptr);

        void update(); // Render preview again, e.g. after changed equipment
        void setViewport(int sizeX, int sizeY);

        int getSlotSelected(int posX, int posY);

    protected:
        osg::ref_ptr<osg::Viewport> mViewport;

        void onSetup() override;
    };

    class UpdateCameraCallback;

    class RaceSelectionPreview : public CharacterPreview
    {
        ESM::NPC mBase;
        MWWorld::LiveCellRef<ESM::NPC> mRef;

    protected:
        bool renderHeadOnly() override { return true; }
        void onSetup() override;

    public:
        RaceSelectionPreview(osg::Group* parent, Resource::ResourceSystem* resourceSystem);
        virtual ~RaceSelectionPreview();

        void setAngle(float angleRadians);

        const ESM::NPC& getPrototype() const { return mBase; }

        void setPrototype(const ESM::NPC& proto);

    private:
        osg::ref_ptr<UpdateCameraCallback> mUpdateCameraCallback;

        float mPitchRadians;
    };

}

#endif
