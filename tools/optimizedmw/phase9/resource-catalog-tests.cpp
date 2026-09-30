#include <components/sceneutil/drawphasetrace.hpp>
#include <osg/Texture2D>
#include <osg/Uniform>
#include <iostream>
#include <stdexcept>

using namespace SceneUtil::DrawPhaseTrace;
void require(bool value,const char* message) { if(!value)throw std::runtime_error(message); }

osg::ref_ptr<osg::Texture2D> texture(const char* filename)
{
    osg::ref_ptr<osg::Image> image=new osg::Image;
    image->allocateImage(2,2,1,GL_RGBA,GL_UNSIGNED_BYTE);
    image->setFileName(filename);
    return new osg::Texture2D(image);
}
osg::ref_ptr<osg::Uniform> sampler(const char* name,int unit)
{
    osg::ref_ptr<osg::Uniform> result=new osg::Uniform(osg::Uniform::SAMPLER_2D,name);
    result->set(unit);
    return result;
}

int main() try
{
    auto& capture=Capture::instance();
    require(capture.enabled(),"catalog fixture must enable capture before construction");
    osg::ref_ptr<osg::StateSet> parent=new osg::StateSet;
    osg::ref_ptr<osg::StateSet> child=new osg::StateSet;
    auto inherited=texture("terrain/inherited.dds");
    auto overridden=texture("terrain/rejected_child.dds");
    auto independent=texture("terrain/detail.dds");
    parent->setTextureAttribute(0,inherited,osg::StateAttribute::OVERRIDE);
    parent->addUniform(sampler("diffuseMap",0),osg::StateAttribute::OVERRIDE);
    child->setTextureAttribute(0,overridden);
    child->setTextureAttribute(1,independent);
    child->addUniform(sampler("diffuseMap",1));
    child->addUniform(sampler("detailMap",1));
    catalogStateSets({parent,child},3,7,100,1);
    require(capture.resourceCount()==2,"not all effective texture units catalogued");
    const auto unit0=capture.resource(0),unit1=capture.resource(1);
    require(unit0.texture==reinterpret_cast<std::uintptr_t>(inherited.get())
        && unit0.stateSet==reinterpret_cast<std::uintptr_t>(parent.get())
        && unit0.image==reinterpret_cast<std::uintptr_t>(inherited->getImage())
        && unit0.context==7 && unit0.submitCamera==100 && unit0.unit==0,
        "parent OVERRIDE or actual pointer/unit/image/state/camera metadata lost");
    require(std::string(unit0.semanticRole.data())=="diffuseMap"
        && std::string(unit1.semanticRole.data())=="detailMap",
        "uniform OVERRIDE produced a false sampler semantic role");
    catalogStateSets({parent,child},9,7,100,1);
    require(capture.resourceCount()==2 && capture.resource(0).firstFrame==3 && capture.resource(0).lastFrame==9,
        "static descriptors allocated repeatedly per frame or lost observation extent");
    child->setTextureAttribute(0,overridden,osg::StateAttribute::PROTECTED);
    child->addUniform(sampler("diffuseMap",1),osg::StateAttribute::PROTECTED);
    catalogStateSets({parent,child},10,7,100,1);
    require(capture.resourceCount()==4 && capture.resource(2).texture==reinterpret_cast<std::uintptr_t>(overridden.get()),
        "PROTECTED child did not override parent's texture proof");
    require(std::string(capture.resource(2).semanticRole.data())=="unknown"
        && std::string(capture.resource(3).semanticRole.data())=="detailMap|diffuseMap",
        "PROTECTED sampler unit replacement retained an old semantic label");
    independent->getImage()->dirty();
    catalogStateSets({parent,child},11,7,100,1);
    require(capture.resourceCount()==5 && capture.resource(4).imageRevision==independent->getImage()->getModifiedCount(),
        "image revision changed but descriptor identity did not");
    osg::ref_ptr<osg::StateSet> composite=new osg::StateSet;
    composite->setTextureAttribute(0,inherited);
    composite->setTextureAttribute(1,independent);
    // Terrain::createPasses uses integer Uniforms for its shader sampler names.
    // The producer's verified unit convention labels these, without inferring
    // that arbitrary integer uniforms in other materials are GLSL samplers.
    composite->addUniform(new osg::Uniform("diffuseMap",0));
    composite->addUniform(new osg::Uniform("blendMap",1));
    catalogStateSets({composite},0,~0u,0,2);
    require(capture.resourceCount()==7 && capture.resource(5).context==~0u && capture.resource(5).submitCamera==0
        && std::string(capture.resource(5).semanticRole.data())=="terrain_composite_diffuse"
        && std::string(capture.resource(6).semanticRole.data())=="terrain_composite_blendmap",
        "composite producer context or sampler roles were invented/lost");
    for(std::size_t i=0;i<32768;++i)
    {
        Capture::ResourceRow row;
        row.texture=100000+i;row.image=200000+i;row.stateSet=300000+i;
        row.context=7;row.unit=2;row.scope=1;
        capture.catalogResource(row);
    }
    require(capture.resourceCount()==32768 && capture.resourceDropped()==7,
        "resource catalogue capacity or explicit drop accounting changed");
    std::cout<<"PASS: effective inherited texture/sampler OVERRIDE and PROTECTED, actual pointer/context/unit/revision joins, static dedup, producer role/unknown context, hard catalog bound and explicit losses\n";
}
catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
