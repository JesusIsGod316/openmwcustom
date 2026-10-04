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
            const bool stream=ticket && ticket->purpose==Resource::TexturePurpose::MainDraw
                && (ticket->role==Resource::TextureRole::Diffuse || ticket->role==Resource::TextureRole::DiffuseSpecular)
                && Resource::AssetPolicy::enabled(Resource::AssetPolicy::TextureStreaming);
            const bool sourceLane=ticket && (ticket->path.ends_with(".dds")
                || (ticket->purpose==Resource::TexturePurpose::MainDraw && ticket->role==Resource::TextureRole::Diffuse
                    && (ticket->path.ends_with(".tga") || ticket->path.ends_with(".targa"))));
            const bool full=sourceLane && Resource::AssetPolicy::enabled(Resource::AssetPolicy::TexturePreparation);
            // A pending wrapper keeps the exact worker completion handle in the
            // cached producer. Workers never replace this image or StateSet.
            if(ticket && sourceLane && selected.supported && (stream || full)
                && ticket->outcome()!=Resource::NativeTextureOutcome::TerminalDecline)
                texture=new Resource::TextureStreaming::Texture(image,ticket,stream,bool(selected.image));
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
        Resource::NativeTexturePreparation mPreparation;
    };
}
#endif
