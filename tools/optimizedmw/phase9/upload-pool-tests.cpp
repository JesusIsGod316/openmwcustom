#define OPENMW_UPLOAD_POOL_TEST_HOOKS
#include <components/resource/preparedterraintexture.hpp>
#include <osg/Texture>
#include <osgViewer/Viewer>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <thread>

namespace Pool=Resource::UploadPool;
void require(bool condition,const char* message) { if(!condition) throw std::runtime_error(message); }
Resource::OpenGlPressureSample sampleValue;
Resource::OpenGlPressureSample read(void*) { return sampleValue; }

void verifyMipPixels(osg::State& state, Resource::PreparedTerrainTexture& texture, const osg::Image& image)
{
    auto* object=texture.getTextureObject(state.getContextID());
    require(object && object->isAllocated(),"accepted texture object unavailable");
    object->bind();
    glPixelStorei(GL_PACK_ALIGNMENT,1);
    int width=image.s(),height=image.t();
    for(unsigned level=0;level<image.getNumMipmapLevels();++level)
    {
        std::vector<unsigned char> data(static_cast<std::size_t>(width)*height);
        glGetTexImage(GL_TEXTURE_2D,level,GL_ALPHA,GL_UNSIGNED_BYTE,data.data());
        for(unsigned i=0;i<data.size();++i)
            require(data[i]==image.data()[image.getMipmapOffset(level)+i],"ordinary fallback or pooled mip contents changed");
        width=(std::max)(1,width/2); height=(std::max)(1,height/2);
    }
}

void drainRetirement(osg::State& state)
{
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(Pool::liveBytes.load())
    {
        state.get<Pool::Retirement>()->flushAllDeletedGLObjects();
        require(std::chrono::steady_clock::now()<deadline,"nonwaiting retirement did not complete in fixture timeout");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
int main() try
{
    Pool::sample=&read;
    Resource::SpeculativeBudget ledger;
    Pool::ledger=&ledger;
    osgViewer::Viewer anchor;
    osg::ref_ptr<osg::GraphicsContext::Traits> traits=new osg::GraphicsContext::Traits;
    traits->readDISPLAY(); traits->setUndefinedScreenDetailsToDefaultScreen();
    traits->width=32; traits->height=32; traits->doubleBuffer=false;
    osg::ref_ptr<osg::GraphicsContext> context=osg::GraphicsContext::createGraphicsContext(traits);
    require(context && context->realize() && context->makeCurrent(),"real GL context unavailable");
    auto& state=*context->getState();
    auto* pool=Pool::find(state,true);
    require(!pool->acquire(state,85),"missing pressure sample admitted staging allocation");
    sampleValue.generation=1; sampleValue.decision.state=Resource::OpenGlPressure::Normal;
    sampleValue.growthWatermarkValid=true;
    sampleValue.decision.admissionsPerSample=1;
    sampleValue.memory.physicalValid=true; sampleValue.memory.physicalTotal=32ull*1024*1024*1024;
    sampleValue.memory.physicalAvailable=16ull*1024*1024*1024;
    sampleValue.memory.commitValid=true; sampleValue.memory.commitAvailable=16ull*1024*1024*1024;
    sampleValue.decision.limits.physicalReserve=4ull*1024*1024*1024;
    sampleValue.decision.limits.commitReserve=1024ull*1024*1024;
    osg::ref_ptr<osg::Image> image=new osg::Image;
    auto* bytes=new unsigned char[85];
    for(unsigned i=0;i<85;++i) bytes[i]=static_cast<unsigned char>(i+1);
    image->setImage(8,8,1,GL_ALPHA,GL_ALPHA,GL_UNSIGNED_BYTE,bytes,osg::Image::USE_NEW_DELETE,1,0);
    image->setMipmapLevels({64,80,84});
    require(Pool::eligible(*image),"exact supplied alpha chain rejected");
    osg::ref_ptr<Resource::PreparedTerrainTexture> texture=new Resource::PreparedTerrainTexture(image);
    texture->setFilter(osg::Texture::MIN_FILTER,osg::Texture::LINEAR_MIPMAP_LINEAR);
    texture->setResizeNonPowerOfTwoHint(false); texture->setUseHardwareMipMapGeneration(false);
    auto* ext=state.get<osg::GLExtensions>();
    GLuint previous=0; ext->glGenBuffers(1,&previous);
    ext->glBindBuffer(GL_PIXEL_UNPACK_BUFFER_ARB,previous);
    ext->glBufferData(GL_PIXEL_UNPACK_BUFFER_ARB,128,nullptr,GL_STATIC_DRAW_ARB);
    glPixelStorei(GL_UNPACK_ALIGNMENT,8); glPixelStorei(GL_UNPACK_ROW_LENGTH,17);
    glPixelStorei(GL_UNPACK_SKIP_ROWS,2); glPixelStorei(GL_UNPACK_SKIP_PIXELS,3);
    texture->apply(state);
    require(Pool::uploads==1,"production texture path did not perform pooled upload");
    GLint actual=0;
    glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING_ARB,&actual); require(actual==static_cast<GLint>(previous),"PBO binding lost");
    glGetIntegerv(GL_UNPACK_ALIGNMENT,&actual); require(actual==8,"unpack alignment lost");
    glGetIntegerv(GL_UNPACK_ROW_LENGTH,&actual); require(actual==17,"unpack row length lost");
    glGetIntegerv(GL_UNPACK_SKIP_ROWS,&actual); require(actual==2,"unpack rows lost");
    glGetIntegerv(GL_UNPACK_SKIP_PIXELS,&actual); require(actual==3,"unpack pixels lost");
    ext->glBindBuffer(GL_PIXEL_UNPACK_BUFFER_ARB,0);
    glPixelStorei(GL_UNPACK_ALIGNMENT,1); glPixelStorei(GL_UNPACK_ROW_LENGTH,0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS,0); glPixelStorei(GL_UNPACK_SKIP_PIXELS,0);
    glPixelStorei(GL_PACK_ALIGNMENT,1);
    int width=8,height=8;
    for(unsigned level=0;level<4;++level)
    {
        std::vector<unsigned char> data(width*height);
        glGetTexImage(GL_TEXTURE_2D,level,GL_ALPHA,GL_UNSIGNED_BYTE,data.data());
        for(unsigned i=0;i<data.size();++i)
            require(data[i]==image->data()[image->getMipmapOffset(level)+i],"pooled mip contents changed");
        width=(std::max)(1,width/2); height=(std::max)(1,height/2);
    }
    texture->apply(state); require(Pool::uploads==1,"clean apply reuploaded image");
    require(ext->glTexStorage2D,"immutable texture storage fixture unavailable");
    osg::ref_ptr<osg::Image> immutableImage=new osg::Image(*image,osg::CopyOp::DEEP_COPY_ALL);
    immutableImage->setInternalTextureFormat(GL_ALPHA8);
    osg::ref_ptr<osg::Texture2D> seed=new osg::Texture2D;
    auto* seedObject=seed->generateAndAssignTextureObject(state.getContextID(),GL_TEXTURE_2D,4,GL_ALPHA8,8,8,1,0);
    seedObject->bind();
    ext->glTexStorage2D(GL_TEXTURE_2D,4,GL_ALPHA8,8,8);
    std::vector<unsigned char> oldPixels(64,255);
    int oldWidth=8,oldHeight=8;
    for(unsigned level=0;level<4;++level)
    {
        glTexSubImage2D(GL_TEXTURE_2D,level,0,0,oldWidth,oldHeight,GL_ALPHA,GL_UNSIGNED_BYTE,oldPixels.data());
        oldWidth=(std::max)(1,oldWidth/2); oldHeight=(std::max)(1,oldHeight/2);
    }
    seedObject->setAllocated(4,GL_ALPHA8,8,8,1,0);
    const auto seedName=seedObject->id();
    require(seedObject->_set!=nullptr,"fixture immutable texture object was not managed by actual OSG pool");
    seed->dirtyTextureObject(); // actual OSG orphan then takeOrGenerate reuse
    osg::ref_ptr<Resource::PreparedTerrainTexture> immutable=new Resource::PreparedTerrainTexture(immutableImage);
    immutable->setFilter(osg::Texture::MIN_FILTER,osg::Texture::LINEAR_MIPMAP_LINEAR);
    immutable->setResizeNonPowerOfTwoHint(false); immutable->setUseHardwareMipMapGeneration(false);
    const auto attemptsBeforeImmutable=Pool::uploads.load();
    immutable->apply(state);
    require(immutable->getTextureObject(state.getContextID())->id()==seedName,"actual immutable orphan was not recycled");
    require(Pool::uploads==attemptsBeforeImmutable,"recycled immutable storage attempted pooled TexImage2D");
    verifyMipPixels(state,*immutable,*immutableImage);
    require(glGetError()==GL_NO_ERROR,"immutable OSG subload fallback produced a GL error");

    osg::ref_ptr<Resource::PreparedTerrainTexture> failedMip=new Resource::PreparedTerrainTexture(image);
    failedMip->setFilter(osg::Texture::MIN_FILTER,osg::Texture::LINEAR_MIPMAP_LINEAR);
    failedMip->setResizeNonPowerOfTwoHint(false); failedMip->setUseHardwareMipMapGeneration(false);
    const auto failuresBefore=Pool::uploadFailures.load();
    const auto attemptsBeforeFailure=Pool::uploads.load();
    Pool::failUploadMip=2;
    failedMip->apply(state);
    Pool::failUploadMip=-1;
    require(Pool::uploads==attemptsBeforeFailure+1 && Pool::uploadFailures==failuresBefore+1,
        "later-mip GL fault was not rejected as one optional upload attempt");
    verifyMipPixels(state,*failedMip,*image);
    require(glGetError()==GL_NO_ERROR,"later-mip ordinary fallback left GL errors");
    require(!pool->acquire(state,Pool::SlotBytes+1),"oversized upload admitted");
    sampleValue.decision.state=Resource::OpenGlPressure::Critical;
    osg::ref_ptr<osg::GraphicsContext::Traits> otherTraits=new osg::GraphicsContext::Traits(*traits);
    osg::ref_ptr<osg::GraphicsContext> other=osg::GraphicsContext::createGraphicsContext(otherTraits);
    require(other && other->realize() && other->makeCurrent(),"second context unavailable");
    auto* otherPool=Pool::find(*other->getState(),true);
    require(!otherPool->acquire(*other->getState(),85),"critical sample admitted new allocation");
    require(otherPool!=pool,"context-local staging aliased");
    other->releaseContext(); other->close(true); context->makeCurrent();
    image->setDataVariance(osg::Object::DYNAMIC);
    require(!Pool::eligible(*image),"dynamic image admitted");
    image->setDataVariance(osg::Object::STATIC);
    image->setMipmapLevels({64,81,84}); require(!Pool::eligible(*image),"malformed chain admitted");
    Pool::release(state); drainRetirement(state);
    require(Pool::liveBytes==0,"pool byte ownership did not retire");
    require(ledger.stats().knownOwnerBytes==0,"staging ledger charge did not retire");
    sampleValue.decision.state=Resource::OpenGlPressure::Normal;

    // Deferred release must not issue GL without the actual current owner. Keep
    // the pending manager intentionally unflushed, then reuse the same Slot.
    auto* pending=pool->acquire(state,85);
    require(pending && pending->identity && pending->charge,"pending identity fixture allocation failed");
    const auto firstIdentity=pending->identity;
    const std::weak_ptr<Resource::SpeculativeBudget::Charge> firstCharge=pending->charge;
    pending=nullptr;
    Pool::holdRetirement=true;
    context->releaseContext();
    Pool::release(state);
    require(Pool::liveBytes==Pool::SlotBytes && ledger.stats().pendingOwnerBytes==Pool::SlotBytes,
        "non-current release discarded pending capacity or charge");
    require(!pool->acquire(state,85),"non-current owner admitted a mapped slot");
    require(context->makeCurrent(),"deferred retirement owner could not be restored");
    auto* replacement=pool->acquire(state,85);
    require(replacement && replacement->identity.get()!=firstIdentity.get() && replacement->charge!=firstCharge.lock(),
        "reused Slot shared an old pending allocation identity/charge");
    require(Pool::liveBytes==Pool::SlotBytes*2 && ledger.stats().knownOwnerBytes==Pool::SlotBytes*2,
        "pending plus replacement backing allocations were not independently charged");
    Pool::release(state);
    Pool::holdRetirement=false;
    drainRetirement(state);

    std::vector<osg::ref_ptr<Pool::Pool>> pools;
    for(unsigned i=0;i<12;++i)
    {
        pools.push_back(new Pool::Pool(state.getContextID()));
        require(pools.back()->acquire(state,85)!=nullptr,"bounded global slot allocation rejected too early");
    }
    osg::ref_ptr<Pool::Pool> overflow=new Pool::Pool(state.getContextID());
    require(!overflow->acquire(state,85),"global staging capacity overflow admitted");
    require(Pool::liveBytes==Pool::GlobalBytes && ledger.stats().knownOwnerBytes==Pool::GlobalBytes,
        "exact pooled and ledger allocation bounds disagree");
    for(auto& value:pools) value->release(state);
    require(Pool::liveBytes==0 && ledger.stats().knownOwnerBytes==0,"global pool retirement lost ownership");
    texture->releaseGLObjects(&state); ext->glDeleteBuffers(1,&previous);
    require(glGetError()==GL_NO_ERROR,"pooled uploads produced a GL error");
    context->releaseContext(); context->close(true);
    std::cout<<"PASS pooled alpha upload: exact four-level contents, clean reuse, unpack restoration, bounded allocation, "
        "pressure and mutation fallback, actual recycled immutable OSG storage, later-mip GL fault fallback, "
        "non-current deferred retirement and independent pending/replacement allocation charges\n";
}
catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
