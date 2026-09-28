#include <components/resource/preparedterraintexture.hpp>
#include <osgViewer/Viewer>
#include <iostream>
#include <stdexcept>
void require(bool v,const char* m){if(!v)throw std::runtime_error(m);}
int main()
try
{
    osgViewer::Viewer anchor;
    osg::ref_ptr<osg::GraphicsContext::Traits> t=new osg::GraphicsContext::Traits;
    t->readDISPLAY();t->setUndefinedScreenDetailsToDefaultScreen();
    t->width=32;t->height=32;t->doubleBuffer=false;t->windowDecoration=false;
    osg::ref_ptr<osg::GraphicsContext> context=osg::GraphicsContext::createGraphicsContext(t);
    require(context && context->realize() && context->makeCurrent(),"no real GL context");
    osg::ref_ptr<osg::Image> image=new osg::Image;
    image->allocateImage(4,4,1,GL_RGBA,GL_UNSIGNED_BYTE);
    std::fill(image->data(),image->data()+64,255);
    osg::ref_ptr<Resource::PreparedTerrainTexture> texture=new Resource::PreparedTerrainTexture(image);
    texture->setFilter(osg::Texture::MIN_FILTER,osg::Texture::LINEAR);
    texture->setFilter(osg::Texture::MAG_FILTER,osg::Texture::LINEAR);
    texture->setResizeNonPowerOfTwoHint(false);
    osg::ref_ptr<osgUtil::IncrementalCompileOperation> ico=new osgUtil::IncrementalCompileOperation;
    osgUtil::IncrementalCompileOperation::CompileInfo info(context,ico);
    osg::ref_ptr<Resource::PreparedTerrainTextureCompileOp> a=new Resource::PreparedTerrainTextureCompileOp(texture);
    osg::ref_ptr<Resource::PreparedTerrainTextureCompileOp> b=new Resource::PreparedTerrainTextureCompileOp(texture);
    require(!a->reusable(info),"new texture falsely ready");
    require(a->compile(info),"initial compile failed");
    require(b->reusable(info),"duplicate producer did not reuse preparation");
    require(b->compile(info),"duplicate compile failed");
    image->data()[0]=3;image->dirty();
    require(!a->reusable(info),"image revision not invalidated");
    a->compile(info);
    texture->apply(*context->getState());
    unsigned char output[64]{};glGetTexImage(GL_TEXTURE_2D,0,GL_RGBA,GL_UNSIGNED_BYTE,output);
    require(output[0]==3,"dirty image failed real GL update");
    texture->setFilter(osg::Texture::MAG_FILTER,osg::Texture::NEAREST);
    require(!a->reusable(info),"filter change not invalidated");a->compile(info);
    texture->allocateMipmapLevels();require(!a->reusable(info),"manual mip allocation ignored");a->compile(info);
    texture->releaseGLObjects(context->getState());require(!a->reusable(info),"released GL resource still ready");a->compile(info);
    osg::ref_ptr<Resource::PreparedTerrainTexture> clone=new Resource::PreparedTerrainTexture(*texture);
    require(!clone->preparationUnchanged(*context->getState()),"clone inherited proof");
    texture->setDataVariance(osg::Object::DYNAMIC);require(!a->reusable(info),"dynamic texture reused");
    texture->setDataVariance(osg::Object::STATIC);
    ico->assignForceTextureDownloadGeometry();require(!a->reusable(info),"force-download side effect bypassed");
    require(glGetError()==GL_NO_ERROR,"OpenGL error");
    context->releaseContext();context->close(true);
    std::cout<<"PASS: real GL initial/duplicate, image revision/pixels, filter/mipmap invalidation, release, clone, dynamic and force-download fallback\n";
}
catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
