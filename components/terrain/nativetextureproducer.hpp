#ifndef OPENMW_TERRAIN_NATIVETEXTUREPRODUCER_H
#define OPENMW_TERRAIN_NATIVETEXTUREPRODUCER_H

#include <components/resource/assetpolicy.hpp>
#include <components/resource/nativetexture.hpp>
#include <components/resource/preparedterraintexture.hpp>
#include <components/resource/texturestreaming.hpp>
#include <components/debug/runtimediagnostics.hpp>

namespace Terrain
{
    // This is the producer seam shared by terrain pass preparation and
    // realization. It owns the same pending ticket from prefetch to first draw;
    // image loading/filter settings remain the ordinary TextureManager policy.
    class NativeTextureProducer
    {
    public:
        struct Selection
        {
            std::shared_ptr<Resource::NativeTextureTicket> ticket;
            std::shared_ptr<const Resource::NativeMipPayload> payload;
            osg::ref_ptr<osg::Image> image;
            bool supported=false;
        };
        explicit NativeTextureProducer(const VFS::Manager* vfs) : mPreparation(vfs) {}
        void prepare(VFS::Path::NormalizedView name,Resource::TextureRole role,Resource::TexturePurpose purpose)
        { mPreparation.request(name,role,purpose); }
        Selection select(VFS::Path::NormalizedView name,Resource::TextureRole role,
            Resource::TexturePurpose purpose,bool supported)
        {
            Selection result;result.supported=supported;
            result.ticket=mPreparation.request(name,role,purpose);
            result.payload=result.ticket ? result.ticket->payload() : nullptr;
            if(result.payload && supported) result.image=Resource::nativeTextureImage(result.payload);
            return result;
        }
        void clear() { mPreparation.clear(); }

        static osg::ref_ptr<osg::Texture2D> realize(const Selection& selected,osg::Image* image,bool canonical)
        {
            osg::ref_ptr<osg::Texture2D> texture;
            const auto& ticket=selected.ticket;
            if(ticket && selected.supported && ticket->purpose==Resource::TexturePurpose::MainDraw
                && (ticket->role==Resource::TextureRole::Diffuse || ticket->role==Resource::TextureRole::DiffuseSpecular)
                && Resource::AssetPolicy::enabled(Resource::AssetPolicy::TextureStreaming))
                texture=new Resource::TextureStreaming::Texture(image,ticket);
            else if(selected.payload && selected.supported) texture=new FullNativeTexture(image);
            else texture=canonical ? new Resource::PreparedTerrainTexture(image) : new osg::Texture2D(image);
            if(selected.payload && selected.supported)
            {
                texture->setResizeNonPowerOfTwoHint(false);
                texture->setUseHardwareMipMapGeneration(false);
                texture->setUnRefImageDataAfterApply(false);
                ++Resource::nativeTextureStats().nativeImages;
                Debug::RuntimeDiagnostics::recordEvent("native_texture_created","terrain",ticket->path,
                    {{"converted",selected.payload->converted},{"native_bytes",selected.payload->bytes.size()},
                     {"role",static_cast<unsigned>(ticket->role)},{"purpose",static_cast<unsigned>(ticket->purpose)}});
            }
            return texture;
        }
    private:
        class FullNativeTexture final : public Resource::PreparedTerrainTexture
        {
        public:
            FullNativeTexture()=default;
            explicit FullNativeTexture(osg::Image* image):PreparedTerrainTexture(image) {}
            FullNativeTexture(const FullNativeTexture& other,const osg::CopyOp& copy=osg::CopyOp::SHALLOW_COPY)
                : PreparedTerrainTexture(other,copy) {}
            META_StateAttribute(Terrain,FullNativeTexture,TEXTURE)
            void apply(osg::State& state) const override
            {
                Resource::PreparedTerrainTexture::apply(state);
                const auto context=state.getContextID();
                auto* object=getTextureObject(context);
                if(context<mApplied.size() && object && object->isAllocated() && !mApplied[context])
                {
                    mApplied[context]=1;++Resource::nativeTextureStats().fullApplies;
                    Debug::RuntimeDiagnostics::recordEvent("native_texture_full_apply","terrain",getImage()->getFileName(),
                        {{"context",context},{"name",object->id()},{"bytes",getImage()->getTotalSizeInBytesIncludingMipmaps()}});
                }
            }
            void resizeGLObjectBuffers(unsigned size) override
            { Resource::PreparedTerrainTexture::resizeGLObjectBuffers(size);mApplied.resize(size); }
            void releaseGLObjects(osg::State* state=nullptr) const override
            {
                if(state && state->getContextID()<mApplied.size()) mApplied[state->getContextID()]=0;
                else mApplied.setAllElementsTo(0);
                Resource::PreparedTerrainTexture::releaseGLObjects(state);
            }
        private:
            mutable osg::buffered_value<unsigned char> mApplied;
        };
        Resource::NativeTexturePreparation mPreparation;
    };
}
#endif
