#include <components/terrain/compositemaprenderer.hpp>
#include <osg/GraphicsContext>
#include <osg/Texture2D>
#include <osg/RenderInfo>
#include <osgViewer/Viewer>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

void require(bool v,const char* m) { if(!v) throw std::runtime_error(m); }
class Paint final : public osg::Drawable
{
public:
    float red; explicit Paint(float r):red(r) {}
    void drawImplementation(osg::RenderInfo&) const override {
        glClearColor(red,0,0,1); glClear(GL_COLOR_BUFFER_BIT);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
};
int main()
try
{
    osgViewer::Viewer anchor;
    osg::ref_ptr<osg::GraphicsContext::Traits> traits=new osg::GraphicsContext::Traits;
    traits->readDISPLAY(); traits->setUndefinedScreenDetailsToDefaultScreen();
    traits->width=32; traits->height=32; traits->windowDecoration=false; traits->doubleBuffer=false;
    osg::ref_ptr<osg::GraphicsContext> context=osg::GraphicsContext::createGraphicsContext(traits);
    require(context && context->realize() && context->makeCurrent(),"no OpenGL context");
    osg::RenderInfo info(context->getState(),nullptr);
    osg::ref_ptr<Terrain::CompositeMapRenderer> renderer=new Terrain::CompositeMapRenderer;
    renderer->setCooperativeBackgroundCompile(true);
    renderer->setTargetFrameRate(1000);
    renderer->setMinimumTimeAvailableForCompile(.001);
    auto makeMap=[] {
        osg::ref_ptr<Terrain::CompositeMap> map=new Terrain::CompositeMap;
        map->mTexture=new osg::Texture2D;
        map->mTexture->setTextureSize(32,32);
        map->mTexture->setInternalFormat(GL_RGBA8);
        map->mTexture->setFilter(osg::Texture::MIN_FILTER,osg::Texture::LINEAR);
        map->mTexture->setFilter(osg::Texture::MAG_FILTER,osg::Texture::LINEAR);
        map->mDrawables={new Paint(.25f),new Paint(.5f),new Paint(1.f)};
        return map;
    };
    auto map=makeMap();
    osg::ref_ptr<osg::Texture2D> consumer=map->mTexture; // real external consumer ownership
    renderer->addCompositeMap(map);
    renderer->drawImplementation(info);
    require(map->mCompiled<3,"background map ignored per-drawable budget");
    require(renderer->getCompileSetSize()==1,"partially compiled map was lost");
    require(renderer->backgroundYields()>0,"no cooperative yield recorded");
    renderer->setImmediate(map);
    require(map->mRequired.load(),"required state not retained on resource");
    renderer->drawImplementation(info);
    require(map->mCompiled==3 && map->mDrawables.empty(),"required map did not fully finish");
    require(renderer->getCompileSetSize()==0,"finished map left queued");
    map->mTexture->apply(*context->getState());
    unsigned char pixels[32*32*4]{};
    glGetTexImage(GL_TEXTURE_2D,0,GL_RGBA,GL_UNSIGNED_BYTE,pixels);
    require(pixels[0]==255 && pixels[1]==0 && pixels[3]==255,"completed composite pixel differs");
    auto legacy=makeMap(); consumer=legacy->mTexture;
    renderer->setCooperativeBackgroundCompile(false);
    renderer->addCompositeMap(legacy);
    renderer->drawImplementation(info);
    require(legacy->mCompiled==3,"disabled control no longer completes whole map");
    require(glGetError()==GL_NO_ERROR,"OpenGL error");
    context->releaseContext(); context->close(true);
    std::cout<<"PASS: real GL background slicing, retained partial output, required promotion, final pixels, off control\n";
}
catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
