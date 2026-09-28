#include <components/sceneutil/shadowsettingsupdate.hpp>
#include <array>
#include <vector>
#include <iostream>
#include <stdexcept>
void require(bool b,const char* m){if(!b)throw std::runtime_error(m);}
int main()
try
{
    const auto normal=SceneUtil::shadowRuntimeParameters(4096.f,.9f,2048);
    require(normal && normal->distance==4096.f && normal->resolution==2048,"normal quality changed");
    require(std::abs(normal->fadeStart-3686.4f)<.01f,"fade distance wrong");
    const auto unlimited=SceneUtil::shadowRuntimeParameters(0.f,.9f,2048);
    require(unlimited && std::isfinite(unlimited->distance-unlimited->fadeStart)
        && unlimited->distance>unlimited->fadeStart,"unlimited fade denominator invalid");
    require(!SceneUtil::shadowRuntimeParameters(4096.f,.9f,65536),"short extent overflow accepted");
    require(!SceneUtil::shadowRuntimeParameters(NAN,.9f,2048),"NaN accepted");
    require(SceneUtil::shadowRuntimeParameters(4096.f,1.f,2048)->fadeStart<4096.f,"zero fade denominator");
    require(SceneUtil::shadowTextureExtent({2048,2048},false,0,3,2)==osg::Vec2s(2048,2048),"near cascade changed");
    require(SceneUtil::shadowTextureExtent({2048,2048},false,2,3,2)==osg::Vec2s(1024,1024),"existing far divisor lost");
    require(SceneUtil::shadowTextureExtent({2048,2048},true,2,3,2)==osg::Vec2s(512,512),"debug extent changed");
    osg::ref_ptr<osg::Uniform> old=new osg::Uniform("maximumShadowMapDistance",4096.f);
    std::array<std::vector<osg::ref_ptr<osg::Uniform>>,2> lists{{{old},{old}}};
    SceneUtil::replaceShadowUniform(lists,"maximumShadowMapDistance",8192.f);
    float retained=0,updated=0;old->get(retained);lists[0][0]->get(updated);
    require(retained==4096.f && updated==8192.f && lists[0][0]==lists[1][0],"captured uniform mutated or frame slots disagree");
    std::cout<<"PASS: shadow extent/far/debug preservation, finite distance/fade validation, copy-on-replace frame uniforms\n";
}
catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
