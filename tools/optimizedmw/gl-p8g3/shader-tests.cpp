#include <components/shader/shadermanager.hpp>
#include <osg/GraphicsContext>
#include <osg/State>
#include <osgViewer/Viewer>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <stdexcept>

int main(int argc, char** argv)
try
{
    if(argc != 3) throw std::runtime_error("expected shader root and output directory");
    // Force the osgViewer window-system registration library to remain linked
    // under ELF --as-needed; GraphicsContext alone lives in libosg.
    osgViewer::Viewer windowSystemAnchor;
    osg::ref_ptr<osg::GraphicsContext::Traits> traits = new osg::GraphicsContext::Traits;
    traits->readDISPLAY();
    traits->setUndefinedScreenDetailsToDefaultScreen();
    traits->width=64; traits->height=64; traits->windowDecoration=false; traits->doubleBuffer=false;
    osg::ref_ptr<osg::GraphicsContext> context = osg::GraphicsContext::createGraphicsContext(traits);
    if(!context || !context->realize() || !context->makeCurrent()) throw std::runtime_error("no real OpenGL context");
    std::cout << "OpenGL renderer: " << glGetString(GL_RENDERER) << " version: " << glGetString(GL_VERSION) << '\n';
    std::filesystem::create_directories(argv[2]);
    int linked=0;
    for(int lod : {0,1}) for(int wind : {0,1}) for(int math : {0,1})
    for(int normals : {0,1}) for(int shadows : {0,1}) for(int clustered : {0,1})
    {
        Shader::ShaderManager manager;
        manager.setShaderPath(argv[1]);
        auto d=Shader::getDefaultDefines();
        const Shader::ShaderManager::DefineMap extra={
            {"optimizedmwGroundcoverLod",std::to_string(lod)},
            {"optimizedmwGroundcoverFastWind",std::to_string(wind)},
            {"optimizedmwGroundcoverGpuPath",std::to_string(math)},
            {"optimizedmwGroundcoverShadowReceive","1"},
            {"diffuseMap","1"},{"diffuseMapUV","0"},{"normalMap",std::to_string(normals)},
            {"normalMapUV","1"},{"reconstructNormalZ","0"},{"shadows_enabled",std::to_string(shadows)},
            {"shadow_texture_unit_list","12,13,14"},{"perspectiveShadowMaps","0"},
            {"disableNormalOffsetShadows","0"},{"shadowNormalOffset","1.0"},
            {"limitShadowMapDistance","1"},{"useShadowDebugOverlay","0"},
            {"groundcoverFadeStart","13500.0"},{"groundcoverFadeEnd","15000.0"},
            {"groundcoverStompMode","2"},{"groundcoverStompIntensity","2"},
            {"lightingMethodClustered",std::to_string(clustered)},{"maxLights","8"},
            {"simpleLighting","0"},{"alphaFunc","518"},{"adjustCoverage","0"},
            {"alphaToCoverage","0"},{"useGPUShader4","1"}};
        for(const auto& [k,v]:extra) d[k]=v;
        manager.setGlobalDefines(d);
        osg::ref_ptr<osg::Program> bindings=new osg::Program;
        bindings->addBindAttribLocation("aOffset",6);
        bindings->addBindAttribLocation("aRotation",7);
        auto program=manager.getProgram("groundcover",{},bindings);
        if (!program) throw std::runtime_error("ShaderManager returned no production program");
        const auto dir=std::filesystem::path(argv[2])/std::to_string(linked);
        std::filesystem::create_directories(dir);
        for(unsigned int i=0;i<program->getNumShaders();++i)
        {
            const auto* shader=program->getShader(i);
            std::ofstream out(dir/(std::to_string(i)+(shader->getType()==osg::Shader::VERTEX?".vert":".frag")));
            out<<shader->getShaderSource();
        }
        program->compileGLObjects(*context->getState());
        auto* pcp=program->getPCP(*context->getState());
        if(!pcp->isLinked())
        {
            std::string log; pcp->getInfoLog(log);
            throw std::runtime_error("production groundcover program failed variant "+std::to_string(linked)+" "+log);
        }
        ++linked;
        program->releaseGLObjects(context->getState());
    }
    context->releaseContext();
    context->close(true);
    std::cout<<linked<<" actual ShaderManager/OpenGL groundcover program variants linked\n";
}
catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
