#include <components/sceneutil/selectivehlod.hpp>
#include <components/sceneutil/generatedstaticlod.hpp>
#include <components/sceneutil/texturetype.hpp>
#include <components/sceneutil/pagingwork.hpp>

#include <osg/Geometry>
#include <osg/MatrixTransform>
#include <osg/Program>
#include <osg/Shader>
#include <osg/Texture2D>
#include <osgViewer/Viewer>

#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace
{
    void require(bool value,const char* message) {if(!value) throw std::runtime_error(message);}
    struct Pixels final : osg::Camera::DrawCallback
    {
        mutable std::array<unsigned char,64*64*4> data{};
        mutable GLenum error=GL_NO_ERROR;
        void operator()(osg::RenderInfo&) const override
        {glReadPixels(0,0,64,64,GL_RGBA,GL_UNSIGNED_BYTE,data.data());error=glGetError();}
    };
    struct Callback final : osg::NodeCallback {};
    struct CustomGroup final:osg::Group {};
    osg::ref_ptr<osg::Program> program()
    {
        osg::ref_ptr<osg::Program> p=new osg::Program;
        osg::ref_ptr<osg::Shader> v=new osg::Shader(osg::Shader::VERTEX,
            "#version 120\nvoid main(){gl_Position=gl_ModelViewProjectionMatrix*gl_Vertex;gl_TexCoord[0]=gl_MultiTexCoord0;}\n");
        osg::ref_ptr<osg::Shader> f=new osg::Shader(osg::Shader::FRAGMENT,
            "#version 120\nuniform sampler2D diffuseMap;void main(){gl_FragColor=texture2D(diffuseMap,gl_TexCoord[0].xy);}\n");
        v->setName("fixture objects.vert");f->setName("fixture objects.frag");p->addShader(v);p->addShader(f);return p;
    }
    osg::ref_ptr<osg::Program> roleProgram()
    {
        osg::ref_ptr<osg::Program> p=new osg::Program;
        p->addShader(new osg::Shader(osg::Shader::VERTEX,
            "#version 120\nvarying vec3 tangent;void main(){gl_Position=gl_ModelViewProjectionMatrix*gl_Vertex;"
            "gl_TexCoord[0]=gl_MultiTexCoord0;gl_TexCoord[1]=gl_MultiTexCoord1;gl_TexCoord[2]=gl_MultiTexCoord2;"
            "tangent=normalize(gl_NormalMatrix*gl_MultiTexCoord7.xyz);}\n"));
        p->addShader(new osg::Shader(osg::Shader::FRAGMENT,
            "#version 120\nuniform sampler2D diffuseMap;uniform sampler2D normalMap;uniform sampler2D specularMap;varying vec3 tangent;"
            "void main(){vec4 d=texture2D(diffuseMap,gl_TexCoord[0].xy);vec3 n=normalize(texture2D(normalMap,gl_TexCoord[1].xy).xyz*2.0-1.0);"
            "vec3 s=texture2D(specularMap,gl_TexCoord[2].xy).rgb;gl_FragColor=vec4(d.rgb*(0.4+0.5*n.z+0.1*abs(tangent.x))+s*0.1,d.a);}\n"));
        return p;
    }
    void roles(osg::Geometry& geometry,osg::Program& p,unsigned red)
    {
        auto* state=geometry.getStateSet();state->setAttribute(&p);
        const auto* diffuse=static_cast<const osg::Texture2D*>(state->getTextureAttribute(0,osg::StateAttribute::TEXTURE));
        for(unsigned unit=1;unit<=2;++unit)
        {
            osg::ref_ptr<osg::Image> image=new osg::Image;image->allocateImage(256,256,1,GL_RGBA,GL_UNSIGNED_BYTE);image->setInternalTextureFormat(GL_RGBA8);
            image->setDataVariance(osg::Object::STATIC);
            for(unsigned y=0;y<256;++y) for(unsigned x=0;x<256;++x)
            {auto* pixel=image->data(x,y);pixel[0]=unit==1?128:red;pixel[1]=unit==1?128:32;pixel[2]=unit==1?255:16;pixel[3]=173;}
            osg::ref_ptr<osg::Texture2D> texture=new osg::Texture2D(image);texture->setDataVariance(osg::Object::STATIC);
            texture->setFilter(osg::Texture::MIN_FILTER,osg::Texture::LINEAR_MIPMAP_LINEAR);texture->setFilter(osg::Texture::MAG_FILTER,osg::Texture::LINEAR);
            texture->setWrap(osg::Texture::WRAP_S,osg::Texture::REPEAT);texture->setWrap(osg::Texture::WRAP_T,osg::Texture::REPEAT);
            texture->setMaxAnisotropy(diffuse->getMaxAnisotropy());
            const char* name=unit==1?"normalMap":"specularMap";texture->setName(name);state->setTextureAttribute(unit,texture);
            state->setTextureAttribute(unit,new SceneUtil::TextureType(name));state->addUniform(new osg::Uniform(name,int(unit)));
            geometry.setTexCoordArray(unit,geometry.getTexCoordArray(0),osg::Array::BIND_PER_VERTEX);
        }
        osg::ref_ptr<osg::Vec4Array> tangents=new osg::Vec4Array;tangents->assign(4,osg::Vec4(1,0,0,1));
        geometry.setTexCoordArray(7,tangents,osg::Array::BIND_PER_VERTEX);
    }
    osg::ref_ptr<osg::Geometry> geometry(float x,osg::Program* p,unsigned char red,unsigned char blue)
    {
        osg::ref_ptr<osg::Image> image=new osg::Image;image->allocateImage(256,256,1,GL_RGBA,GL_UNSIGNED_BYTE);
        image->setInternalTextureFormat(GL_RGBA8);image->setDataVariance(osg::Object::STATIC);
        for(unsigned y=0;y<256;++y) for(unsigned u=0;u<256;++u)
        {auto* pixel=image->data(u,y);pixel[0]=red;pixel[1]=static_cast<unsigned char>((u+y)%64+32);pixel[2]=blue;pixel[3]=static_cast<unsigned char>(y/2+64);}
        osg::ref_ptr<osg::Texture2D> texture=new osg::Texture2D(image);
        texture->setName("diffuseMap");texture->setDataVariance(osg::Object::STATIC);
        texture->setFilter(osg::Texture::MIN_FILTER,osg::Texture::LINEAR_MIPMAP_LINEAR);
        texture->setFilter(osg::Texture::MAG_FILTER,osg::Texture::LINEAR);
        texture->setWrap(osg::Texture::WRAP_S,osg::Texture::REPEAT);texture->setWrap(osg::Texture::WRAP_T,osg::Texture::REPEAT);
        osg::ref_ptr<osg::StateSet> state=new osg::StateSet;
        state->setAttribute(p);state->setTextureAttribute(0,texture);state->setTextureAttribute(0,new SceneUtil::TextureType("diffuseMap"));
        state->addUniform(new osg::Uniform("diffuseMap",0));state->setDataVariance(osg::Object::STATIC);
        osg::ref_ptr<osg::Geometry> g=new osg::Geometry;
        osg::ref_ptr<osg::Vec3Array> vertices=new osg::Vec3Array;
        vertices->push_back({x,-.8f,0});vertices->push_back({x+.75f,-.8f,0});vertices->push_back({x+.75f,.8f,0});vertices->push_back({x,.8f,0});
        osg::ref_ptr<osg::Vec3Array> normals=new osg::Vec3Array;
        normals->assign(4,osg::Vec3(0,0,1));
        osg::ref_ptr<osg::Vec2Array> uv=new osg::Vec2Array;
        uv->push_back({0,0});uv->push_back({1,0});uv->push_back({1,1});uv->push_back({0,1});
        osg::ref_ptr<osg::DrawElementsUInt> indices=new osg::DrawElementsUInt(GL_TRIANGLES);
        for(unsigned i : {0u,1u,2u,0u,2u,3u}) indices->push_back(i);
        g->setVertexArray(vertices);g->setNormalArray(normals,osg::Array::BIND_PER_VERTEX);g->setTexCoordArray(0,uv);
        g->addPrimitiveSet(indices);g->setStateSet(state);g->setDataVariance(osg::Object::STATIC);
        g->setUseVertexBufferObjects(true);g->setUseDisplayList(false);return g;
    }
}
int main() try
{
    const auto p=program();auto a=geometry(-.9f,p,220,15),b=geometry(.1f,p,15,220);
    osg::ref_ptr<osg::Group> source=new osg::Group;source->setDataVariance(osg::Object::STATIC);source->addChild(a);source->addChild(b);
    require(SceneUtil::certifySelectiveHlodTemplate(*source),"positive original ancestry certificate");
    SceneUtil::HlodBuildOptions fixture;fixture.requireShippedObjectShader=false;fixture.reserveGpu=false;
    SceneUtil::HlodBuildOptions strict=fixture;strict.requireShippedObjectShader=true;
    const auto lookalike=SceneUtil::buildSelectiveHlod(*source,*source,strict);
    require(!lookalike && lookalike.rejection=="shader feature closure is not the shipped objects program","shader name lookalike is not a production shader certificate");
    require(!SceneUtil::buildSelectiveHlod(*source,*source),"production CPU/GPU preparation requires admitted optional worker scope");
    auto result=SceneUtil::buildSelectiveHlod(*source,*source,fixture);
    if(!result) std::cerr<<result.rejection<<'\n';
    require(bool(result),"production atlas helper must engage");
    require(result.controlSubmissions==2 && result.proxySubmissions==1 && result.distinctDiffuseImages==2,"true differing-texture submission reduction");
    const auto* group=result.representation->asGroup();
    auto* proxy=dynamic_cast<osg::Geometry*>(const_cast<osg::Node*>(group->getChild(0)));
    require(proxy && proxy->getVertexArray()!=a->getVertexArray(),"derived private vertex lifetime");
    const auto* atlasTexture=dynamic_cast<const osg::Texture2D*>(proxy->getStateSet()->getTextureAttribute(0,osg::StateAttribute::TEXTURE));
    require(atlasTexture && atlasTexture->getImage()->getNumMipmapLevels()==11 && atlasTexture->getMaxLOD()==7.f,"complete mip chain and safe chart sampling cap");
    const auto* atlas=atlasTexture->getImage();
    require(atlas->data(128,128)[3]==a->getStateSet()->getTextureAttribute(0,osg::StateAttribute::TEXTURE)->asTexture()->getImage(0)->data(0,0)[3],"diffuse alpha data preserved");
    require(atlas->data(127,128)[0]==atlas->data(383,128)[0],"repeat wrap chart gutter");
    source->getOrCreateStateSet()->setMode(GL_BLEND,osg::StateAttribute::ON);
    require(!SceneUtil::buildSelectiveHlod(*source,*source,fixture),"inherited alpha original fallback");source->getStateSet()->removeMode(GL_BLEND);
    auto* uv=static_cast<osg::Vec2Array*>(a->getTexCoordArray(0));(*uv)[0].x()=-4.01f;
    require(!SceneUtil::buildSelectiveHlod(*source,*source,fixture),"unbounded repeating UV fallback");(*uv)[0].x()=0;
    auto* aTexture=static_cast<osg::Texture2D*>(a->getStateSet()->getTextureAttribute(0,osg::StateAttribute::TEXTURE));
    auto* bTexture=static_cast<osg::Texture2D*>(b->getStateSet()->getTextureAttribute(0,osg::StateAttribute::TEXTURE));
    aTexture->setWrap(osg::Texture::WRAP_S,osg::Texture::CLAMP_TO_EDGE);(*uv)[0].x()=-.01f;
    require(!SceneUtil::buildSelectiveHlod(*source,*source,fixture),"clamped UV beyond chart fallback");
    aTexture->setWrap(osg::Texture::WRAP_S,osg::Texture::REPEAT);(*uv)[0].x()=0;
    a->addUpdateCallback(new Callback);
    require(!SceneUtil::certifySelectiveHlodTemplate(*source),"original controller ancestry fallback");a->setUpdateCallback(nullptr);
    osg::ref_ptr<osg::MatrixTransform> mirrored=new osg::MatrixTransform(osg::Matrix::scale(-1,1,1));mirrored->addChild(source);
    require(!SceneUtil::certifySelectiveHlodTemplate(*mirrored),"original mirrored transform fallback");
    osg::ref_ptr<osg::MatrixTransform> marker=new osg::MatrixTransform;
    marker->setNodeMask(0);marker->setDataVariance(osg::Object::STATIC);marker->setUserValue("recordIndex",7u);source->addChild(marker);
    require(SceneUtil::certifySelectiveHlodTemplate(*source) && SceneUtil::qualifySelectiveHlodTemplate(*source,false),"loader-proven empty static hidden marker");
    marker->addChild(a);
    require(!SceneUtil::certifySelectiveHlodTemplate(*source),"zero-mask drawable is never discarded");marker->removeChild(a);
    marker->setUpdateCallback(new Callback);
    require(!SceneUtil::certifySelectiveHlodTemplate(*source),"zero-mask controller is never discarded");marker->setUpdateCallback(nullptr);
    osg::ref_ptr<CustomGroup> customMarker=new CustomGroup;customMarker->setDataVariance(osg::Object::STATIC);customMarker->setUserValue("recordIndex",8u);
    marker->addChild(customMarker);
    require(!SceneUtil::certifySelectiveHlodTemplate(*source),"zero-mask custom descendant is never discarded");marker->removeChild(customMarker);
    marker->setUserValue("recordIndex",~0u);
    require(!SceneUtil::certifySelectiveHlodTemplate(*source),"empty mask and name alone are not loader provenance");source->removeChild(marker);
    std::atomic_bool cancelled=true;bool cancellationObserved=false;
    {SceneUtil::PagingWorkScope scope(&cancelled);try{SceneUtil::buildSelectiveHlod(*source,*source,fixture);}catch(const SceneUtil::PagingWorkCancelled&){cancellationObserved=true;}}
    require(cancellationObserved,"obsolete worker preparation cancels without publication");
    source->getOrCreateStateSet()->addUniform(new osg::Uniform("texMat0",osg::Matrixf::scale(2,2,1)));
    require(!SceneUtil::buildSelectiveHlod(*source,*source,fixture),"unqualified source texture transform retains originals");source->getStateSet()->removeUniform("texMat0");
    SceneUtil::HlodBuildOptions denied=fixture;denied.maxHostBytes=1;
    require(!SceneUtil::buildSelectiveHlod(*source,*source,denied),"host admission denial preserves all originals");
    b->getStateSet()->addUniform(new osg::Uniform("materialScalar",.5f));
    require(!SceneUtil::buildSelectiveHlod(*source,*source,fixture),"different scalar material fallback");b->getStateSet()->removeUniform("materialScalar");
    const auto sourceBound=source->getBound();
    require(result.representation->getBound().radius()+1e-5>=sourceBound.radius(),"original conservative bound");
    // The negative admission checks above intentionally change owned graph
    // structure. Qualify a fresh immutable candidate for the GL positive lane;
    // never refresh the earlier candidate's material guard after mutation.
    source->getOrCreateStateSet()->setDataVariance(osg::Object::STATIC);
    osg::ref_ptr<osg::Node> privateControl=osg::clone(source.get(),osg::CopyOp::DEEP_COPY_NODES|osg::CopyOp::DEEP_COPY_DRAWABLES|osg::CopyOp::DEEP_COPY_STATESETS);
    result=SceneUtil::buildSelectiveHlod(*source,*privateControl,fixture);
    require(bool(result),"restored source must qualify a fresh GL candidate");
    proxy=dynamic_cast<osg::Geometry*>(result.representation->asGroup()->getChild(0));

    osg::ref_ptr<osg::GraphicsContext::Traits> traits=new osg::GraphicsContext::Traits;
    traits->readDISPLAY();traits->width=64;traits->height=64;traits->pbuffer=true;traits->doubleBuffer=false;traits->windowDecoration=false;
    osg::ref_ptr<osg::GraphicsContext> context=osg::GraphicsContext::createGraphicsContext(traits);
    require(context.valid(),"real GL context required for atlas consumption fixture");
    osgViewer::Viewer viewer;viewer.setThreadingModel(osgViewer::Viewer::SingleThreaded);
    auto* camera=viewer.getCamera();camera->setGraphicsContext(context);camera->setViewport(0,0,64,64);
    camera->setProjectionMatrixAsOrtho(-1,1,-1,1,-1,1);camera->setViewMatrix(osg::Matrix::identity());camera->setClearColor({0,0,0,0});
    camera->getOrCreateStateSet()->setMode(GL_BLEND,osg::StateAttribute::OFF);
    osg::ref_ptr<Pixels> pixels=new Pixels;camera->setFinalDrawCallback(pixels);
    const auto beforeControl=SceneUtil::GeneratedStaticLod::Counters::controlDraws.load();
    viewer.setSceneData(privateControl);viewer.realize();viewer.frame();viewer.frame();const auto controlPixels=pixels->data;
    unsigned controlRgb=0;
    for(unsigned i=0;i<controlPixels.size();++i) if(i%4!=3 && controlPixels[i]) ++controlRgb;
    std::cout<<"H1_HELPER_CONTROL_RGB nonempty="<<controlRgb<<'\n';
    require(controlRgb>128,"actual textured RGB pixels required");
    const auto originalPerFrame=(SceneUtil::GeneratedStaticLod::Counters::controlDraws-beforeControl)/2;
    const auto beforeProxy=SceneUtil::HlodCounters::consumedDraws.load();
    viewer.setSceneData(result.representation);viewer.frame();viewer.frame();
    const auto proxyPerFrame=(SceneUtil::HlodCounters::consumedDraws-beforeProxy)/2;
    std::cout<<"H1_HELPER_DRAW_DIAGNOSTIC original_draws="<<originalPerFrame<<" proxy_draws="<<proxyPerFrame
        <<" cull_fallbacks="<<SceneUtil::GeneratedStaticLod::Counters::materialCullFallbacks.load()
        <<" draw_fallbacks="<<SceneUtil::HlodCounters::materialDrawFallbacks.load()<<'\n';
    unsigned differences=0,maxDifference=0;
    for(unsigned i=0;i<controlPixels.size();++i)
    {const unsigned delta=std::abs(int(controlPixels[i])-int(pixels->data[i]));maxDifference=std::max(maxDifference,delta);if(delta>8) ++differences;}
    require(originalPerFrame==2 && proxyPerFrame==1,"actual consumed GL draw reduction");
    require(SceneUtil::HlodCounters::consumedDraws>=2 && SceneUtil::HlodCounters::consumedIndices>=24,"production H1 actual draw receipt");
    require(differences<64,"atlas image/material/UV quality bound");
    if(pixels->error!=GL_NO_ERROR) std::cerr<<"GL error "<<pixels->error<<'\n';
    require(pixels->error==GL_NO_ERROR,"GL atlas realization and consumption");
    std::cout<<"H1_PRODUCTION_HELPER draws="<<originalPerFrame<<"->"<<proxyPerFrame
        <<" atlas_bytes="<<result.atlasBytes<<" max_pixel_delta="<<maxDifference<<" outside_tolerance="<<differences<<'\n';
    // A triangle crossing repeated source periods must retain affine UVs.
    // Rewriting each UV with fract() would visibly destroy this fixture.
    a->setDrawCallback(nullptr);b->setDrawCallback(nullptr);
    for(auto* geometry : {a.get(),b.get()})
    {
        auto* values=static_cast<osg::Vec2Array*>(geometry->getTexCoordArray(0));
        for(auto& value:*values) value= value*2.f-osg::Vec2(.25f,.25f);
        values->dirty();
    }
    aTexture->setMaxAnisotropy(16.f);bTexture->setMaxAnisotropy(16.f);
    auto repeated=SceneUtil::buildSelectiveHlod(*source,*source,fixture);
    require(bool(repeated),"bounded repeated UV production bake");
    auto* repeatProxy=dynamic_cast<osg::Geometry*>(repeated.representation->asGroup()->getChild(0));
    const auto* repeatTexture=static_cast<const osg::Texture2D*>(repeatProxy->getStateSet()->getTextureAttribute(0,osg::StateAttribute::TEXTURE));
    require(repeatTexture->getMaxAnisotropy()==16.f && repeatTexture->getMaxLOD()==3.f,"16x anisotropy and safe full-mip chart cap");
    require(SceneUtil::qualifySelectiveHlodTemplate(*source,false),"cheap qualifier admits the bounded periodic bake");
    viewer.setSceneData(source);viewer.frame();viewer.frame();const auto repeatControl=pixels->data;
    viewer.setSceneData(repeated.representation);viewer.frame();viewer.frame();
    std::uint64_t repeatError=0;unsigned repeatOutside=0;
    for(unsigned i=0;i<repeatControl.size();++i)
    {const unsigned delta=std::abs(int(repeatControl[i])-int(pixels->data[i]));repeatError+=delta;if(delta>24)++repeatOutside;}
    std::cout<<"H1_REPEAT_CONSUMPTION periods=4 anisotropy=16 absolute_channel_error="<<repeatError<<" outside_24="<<repeatOutside<<'\n';
    require(repeatOutside<repeatControl.size()/20 && pixels->error==GL_NO_ERROR,"actual repeated UV and anisotropic atlas quality");
    const auto materialProgram=roleProgram();roles(*a,*materialProgram,64);roles(*b,*materialProgram,96);
    auto material=SceneUtil::buildSelectiveHlod(*source,*source,fixture);
    if(!material) std::cerr<<material.rejection<<'\n';
    require(bool(material) && material.atlasBytes==3*result.atlasBytes,"all diffuse/normal/specular atlas debts included");
    auto* materialProxy=dynamic_cast<osg::Geometry*>(material.representation->asGroup()->getChild(0));
    require(materialProxy->getTexCoordArray(7)!=a->getTexCoordArray(7) && materialProxy->getTexCoordArray(1)==materialProxy->getTexCoordArray(0),"private tangents and co-registered role UVs");
    viewer.setSceneData(source);viewer.frame();viewer.frame();const auto materialControl=pixels->data;
    viewer.setSceneData(material.representation);viewer.frame();viewer.frame();
    unsigned materialOutside=0;std::uint64_t materialError=0;
    for(unsigned i=0;i<materialControl.size();++i)
    {const unsigned delta=std::abs(int(materialControl[i])-int(pixels->data[i]));materialError+=delta;if(delta>24)++materialOutside;}
    require(materialOutside<materialControl.size()/20 && pixels->error==GL_NO_ERROR,"actual diffuse/normal/specular atlas GL quality");
    std::cout<<"H1_MATERIAL_CONSUMPTION atlases=3 atlas_bytes="<<material.atlasBytes<<" absolute_channel_error="<<materialError<<" outside_24="<<materialOutside<<'\n';
    a->getStateSet()->setTextureAttribute(1,new SceneUtil::TextureType("normalHeightMap"));
    require(!SceneUtil::buildSelectiveHlod(*source,*source,fixture),"parallax role preserves originals");a->getStateSet()->setTextureAttribute(1,new SceneUtil::TextureType("normalMap"));
    osg::ref_ptr<osg::Array> originalTangents=a->getTexCoordArray(7);a->setTexCoordArray(7,nullptr);
    require(!SceneUtil::buildSelectiveHlod(*source,*source,fixture),"missing normal-map tangents preserves originals");a->setTexCoordArray(7,originalTangents,osg::Array::BIND_PER_VERTEX);
    a->getStateSet()->setTextureAttribute(1,new SceneUtil::TextureType("specularMap"));
    require(!SceneUtil::buildSelectiveHlod(*source,*source,fixture),"role/unit mismatch preserves originals");a->getStateSet()->setTextureAttribute(1,new SceneUtil::TextureType("normalMap"));
    osg::ref_ptr<osg::Vec2Array> differentUv=new osg::Vec2Array(*static_cast<osg::Vec2Array*>(a->getTexCoordArray(0)));(*differentUv)[0].x()+=.125f;
    a->setTexCoordArray(1,differentUv);
    require(!SceneUtil::buildSelectiveHlod(*source,*source,fixture),"non co-registered role UV retains originals");a->setTexCoordArray(1,a->getTexCoordArray(0),osg::Array::BIND_PER_VERTEX);
    a->getStateSet()->setTextureAttribute(3,aTexture);a->getStateSet()->setTextureAttribute(3,new SceneUtil::TextureType("emissiveMap"));
    require(!SceneUtil::buildSelectiveHlod(*source,*source,fixture),"extra maps retain originals");
    viewer.setSceneData(nullptr);a=nullptr;b=nullptr;source=nullptr;mirrored=nullptr;
    require(proxy->getVertexArray()->getNumElements()==8,"derived geometry survives original release");
    return 0;
}
catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
