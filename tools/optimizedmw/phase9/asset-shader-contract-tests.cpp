#include <components/shader/assetgeometrycontract.hpp>
#include <components/shader/indirectprojectioncontract.hpp>
#include <components/shader/shadermanager.hpp>
#include <components/settings/settings.hpp>
#include <components/settings/parser.hpp>
#include <components/settings/values.hpp>
#include <osgViewer/Viewer>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <stdexcept>

namespace A = Shader::AssetGeometry;
namespace
{
    void require(bool value, const char* message) {if (!value) throw std::runtime_error(message);}
    std::string read(const std::filesystem::path& path)
    {
        std::ifstream input(path); require(bool(input),"mandatory shader input unavailable");
        return {std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
    }
    Shader::ShaderManager::DefineMap defines(const std::filesystem::path& root)
    {
        auto result = Shader::getDefaultDefines();
        // This focused CPU provenance fixture supplies all template parameters
        // explicitly. The separate real paging fixture owns production GL and
        // captured light/shadow settings; no GL success is inferred here.
        const std::regex token(R"(@([A-Za-z_][A-Za-z_0-9]*))");
        std::size_t files = 0;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(root))
        {
            if (!entry.is_regular_file()) continue;
            require(++files <= 256,"bounded shader input population");
            const auto source = read(entry.path());
            for (auto it = std::sregex_iterator(source.begin(),source.end(),token); it != std::sregex_iterator(); ++it)
                result.emplace((*it)[1].str(),"0");
        }
        result["diffuseMap"] = result["normalMap"] = result["specularMap"] = "1";
        result["forcePPL"] = "1"; result["diffuseMapUV"] = "0";
        result["normalMapUV"] = "1"; result["specularMapUV"] = "2";
        result["lightingMethodClustered"] = "1";
        result["shadows_enabled"] = "1"; result["shadowMaps"] = "0,1,2";
        return result;
    }
    const A::Samplers samplers{{"diffuseMap",0},{"normalMap",1},{"specularMap",2}};
    osg::ref_ptr<osg::Program> program(Shader::ShaderManager& manager, const std::filesystem::path& root)
    {
        manager.setShaderPath(root);
        auto globals = defines(root); manager.setGlobalDefines(globals);
        auto value = manager.getProgram("compatibility/objects",{},nullptr,samplers);
        require(value.valid(),"normal program construction must remain valid"); return value;
    }
    struct Folder
    {
        std::filesystem::path path;
        explicit Folder(const std::filesystem::path& source)
        {
            path = std::filesystem::temp_directory_path() / ("optimizedmw-asset-contract-"
                + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
            require(!std::filesystem::exists(path),"isolated fixture root must be fresh");
            std::filesystem::copy(source,path,std::filesystem::copy_options::recursive);
        }
        ~Folder()
        {
            // Only this independently created, fixed-prefix direct temp child.
            if (path.parent_path() == std::filesystem::temp_directory_path()
                && path.filename().string().starts_with("optimizedmw-asset-contract-"))
            {std::error_code error; std::filesystem::remove_all(path,error);}
        }
    };
    void append(const std::filesystem::path& path)
    {std::ofstream file(path,std::ios::app);require(bool(file),"owned fixture dependency mutation");file << "\n// asset provenance mutation\n";}
}
int main(int argc,char** argv) try
{
    std::cout << std::unitbuf;
    require(argc == 4,"usage: p9-asset-shader-contract stockShaderRoot stagedPbrShaderRoot defaultsCfg");
#ifdef _WIN32
    _putenv_s("OPENMW_P9_ASSET_MASK","1");
#else
    setenv("OPENMW_P9_ASSET_MASK","1",1);
#endif
    Settings::SettingsFileParser settings;
    settings.loadSettingsFile(std::filesystem::u8path(argv[3]),Settings::Manager::mDefaultSettings);
    Settings::StaticValues::initDefaults(); Settings::Manager::mUserSettings = Settings::Manager::mDefaultSettings; Settings::StaticValues::init();
    const auto stock = std::filesystem::u8path(argv[1]), pbr = std::filesystem::u8path(argv[2]);
    require(std::filesystem::is_directory(stock) && std::filesystem::is_directory(pbr),"both reviewed shader families are mandatory");
    const auto before = A::liveMetadataBytes();
    for (const auto& [root,family] : {std::pair{stock,A::Family::Stock},std::pair{pbr,A::Family::Pbr}})
    {
        Shader::ShaderManager manager; auto value = program(manager,root);
        auto snapshot = A::capture(*value);
        if (!snapshot)
        {
            std::cout << "ASSET_CONTRACT_DIAGNOSTIC family=" << A::familyName(family)
                << " validate=" << A::refusalName(A::validate(*value))
                << " certify=" << A::refusalName(A::certify(*value,samplers))
                << " shaders=" << value->getNumShaders() << " metadata=" << A::liveMetadataBytes() << '\n';
            for (unsigned i = 0; i < value->getNumShaders(); ++i)
            {
                const auto* shader = value->getShader(i); const auto source = A::sourceStatus(*shader);
                std::cout << "ASSET_CONTRACT_PRODUCER index=" << i << " name=" << shader->getName()
                    << " proof_name=" << source.name << " refusal=" << A::refusalName(source.refusal)
                    << " families=" << source.families << " bytes=" << shader->getShaderSource().size() << '\n';
            }
        }
        require(snapshot.has_value(),"reviewed stock/PBR program must acquire an asset proof");
        require(snapshot->family() == family && snapshot->samplers() == samplers && !snapshot->recipe().empty(),"exact family/sampler/recipe identity");
        require(snapshot->define("normalMap") == "1" && snapshot->define("specularMap") == "1"
            && snapshot->define("lightingMethodClustered") == "1" && snapshot->define("shadows_enabled") == "1","normal/specular/shadow/light effective substitutions captured");
        require(A::current(*snapshot,*value) == A::Refusal::None,"immutable prepared source must validate");
        require(Shader::IndirectProjection::matches(*value) == (family == A::Family::Stock),"indirect authorization must stay stock-only");
        std::cout << "ASSET_CONTRACT_FAMILY family=" << A::familyName(family) << " producers=" << value->getNumShaders()
            << " metadata=" << A::liveMetadataBytes() << '\n';
        auto clone = Shader::ShaderManager::cloneProgram(value); require(A::current(*snapshot,*clone) == A::Refusal::None,"exact sampler-preserving clone remains usable");
        osg::ref_ptr<osg::Program> generic = new osg::Program(*value,osg::CopyOp::SHALLOW_COPY);
        require(A::current(*snapshot,*generic) == A::Refusal::Bindings,"generic clone cannot borrow private sampler apply behavior");
        auto* shader = value->getShader(0); const auto original = shader->getShaderSource(); auto changed = original;
        changed[0] = changed[0] == '#' ? ' ' : '#'; shader->setShaderSource(changed);
        require(changed.size() == original.size() && A::current(*snapshot,*value) == A::Refusal::Source,"same-length source mutation must invalidate");
        shader->setShaderSource(original); require(A::current(*snapshot,*value) == A::Refusal::None,"exact source restoration is distinguishable");
        shader->addCodeInjection(-1,"unreviewed"); require(A::current(*snapshot,*value) == A::Refusal::Source,"code injection invalidates"); shader->getCodeInjectionMap().clear();
        shader->setShaderRequirements({"unreviewed"}); require(A::current(*snapshot,*value) == A::Refusal::Defines,"shader requirements mutation invalidates"); shader->setShaderRequirements({});
        auto imported = value->getShaderDefines(); value->setShaderDefines({"unreviewed"}); require(A::current(*snapshot,*value) == A::Refusal::Defines,"program imported defines mutation invalidates"); value->setShaderDefines(imported);
        value->addShader(new osg::Shader(osg::Shader::GEOMETRY,"void main(){}")); require(A::current(*snapshot,*value) == A::Refusal::Stage,"extra shader stage invalidates"); value->removeShader(value->getShader(value->getNumShaders()-1));
        const auto vertices = value->getParameter(GL_GEOMETRY_VERTICES_OUT_EXT);
        value->setParameter(GL_GEOMETRY_VERTICES_OUT_EXT,vertices+1);
        require(A::current(*snapshot,*value) == A::Refusal::Stage,"geometry linking parameter mutation invalidates");
        value->setParameter(GL_GEOMETRY_VERTICES_OUT_EXT,vertices);
        const auto feedback = value->getTransformFeedBackMode();
        value->setTransformFeedBackMode(feedback == GL_SEPARATE_ATTRIBS ? GL_INTERLEAVED_ATTRIBS : GL_SEPARATE_ATTRIBS);
        require(A::current(*snapshot,*value) == A::Refusal::Stage,"feedback mode mutation invalidates");
        value->setTransformFeedBackMode(feedback);
        value->addBindAttribLocation("unreviewed",13); require(A::current(*snapshot,*value) == A::Refusal::Bindings,"attribute binding mutation invalidates");
        std::cout << "ASSET_CONTRACT_FAMILY family=" << A::familyName(family) << " positive=1 source=1 injection=1 stage=1 imports=1 clone=1 indirect_unchanged=1\n";
    }
    require(A::liveMetadataBytes() == before,"source/program/snapshot ownership must release bounded metadata");
    for (const auto* mutation : {"compatibility/objects.frag","lib/core/fragment.h.glsl","lib/core/lighting_fragment.glsl"})
    {
        Folder folder(pbr); append(folder.path/mutation);
        Shader::ShaderManager manager; auto value = program(manager,folder.path);
        require(!A::matches(*value),"root/include/linked producer source mutation cannot acquire proof");
        std::cout << "ASSET_CONTRACT_RAW path=" << mutation << " rejected=1\n";
    }
    for (unsigned binding = 0; binding < 2; ++binding)
    {
        Shader::ShaderManager manager; auto value = program(manager,pbr); auto snapshot = A::capture(*value);require(bool(snapshot),"binding setup");
        if (!binding) value->addBindFragDataLocation("unreviewed",1); else value->addBindUniformBlock("unreviewed",7);
        require(A::current(*snapshot,*value) == A::Refusal::Bindings,"fragment/block binding mutation invalidates");
    }
    {
        Shader::ShaderManager manager; auto value = program(manager,pbr);
        osg::ref_ptr<osg::StateSet> parent = new osg::StateSet, child = new osg::StateSet;
        require(value->getShaderDefines().contains("FORCE_OPAQUE"),"actual program must declare imported define");
        parent->setDefine("FORCE_OPAQUE","1",osg::StateAttribute::ON|osg::StateAttribute::OVERRIDE);
        child->setDefine("FORCE_OPAQUE","0");
        auto snapshot = A::capture(*value,{parent.get(),child.get()}); require(bool(snapshot),"effective import setup");
        child->setDefine("FORCE_OPAQUE","2"); require(A::current(*snapshot,*value,std::vector<const osg::StateSet*>{parent.get(),child.get()}) == A::Refusal::None,"overridden child is semantically unchanged");
        child->setDefine("FORCE_OPAQUE","2",osg::StateAttribute::ON|osg::StateAttribute::PROTECTED);
        require(A::current(*snapshot,*value,std::vector<const osg::StateSet*>{parent.get(),child.get()}) == A::Refusal::Defines,"effective protected define mutation invalidates");
        child->setDefine("DISTORTION","1"); require(!A::capture(*value,{child.get()}),"distortion feature cannot acquire an opaque asset snapshot");
        child->removeDefine("DISTORTION");
        child->setDefine("FORCE_OPAQUE","1\n#define unreviewed 1");
        require(!A::capture(*value,{child.get()}),"imported define cannot inject a shader directive");
        manager.setShaderPath(pbr); require(A::current(*snapshot,*value) == A::Refusal::Epoch,"path update invalidates exact owner cohort");
    }
    {
        Shader::ShaderManager manager; auto value = program(manager,pbr); auto snapshot = A::capture(*value);require(bool(snapshot),"global setup");
        auto globals = manager.getGlobalDefines(); globals["forcePPL"] = "0"; manager.setGlobalDefines(globals);
        require(A::current(*snapshot,*value) == A::Refusal::Epoch,"actual global regeneration invalidates prior proof");
    }
    {
        Folder folder(pbr); Shader::ShaderManager manager; auto value = program(manager,folder.path);
        auto snapshot = A::capture(*value); require(bool(snapshot),"reload setup"); append(folder.path/"compatibility/objects.frag");
        std::filesystem::last_write_time(folder.path/"compatibility/objects.frag",std::filesystem::file_time_type::clock::now()+std::chrono::seconds(1));
        manager.triggerShaderReload(); require(A::current(*snapshot,*value) == A::Refusal::Epoch,"reload request invalidates before publication");
        osgViewer::Viewer viewer; manager.update(viewer); require(!A::matches(*value),"actual touched-shader update remains fail closed");
    }
    {
        Shader::ShaderManager manager; manager.setShaderPath(pbr); auto globals = defines(pbr);
        globals["forcePPL"] = "1\n#define unreviewed 1"; manager.setGlobalDefines(globals);
        auto value = manager.getProgram("compatibility/objects",{},nullptr,samplers);
        require(value.valid() && !A::matches(*value),"global substitution injection preserves normal program but refuses optional proof");
    }
    {
        Shader::ShaderManager manager; manager.setShaderPath(pbr); auto globals = defines(pbr); manager.setGlobalDefines(globals);
        auto value = manager.getProgram("compatibility/objects",{{"diffuseMapUV","0; unreviewed"}},nullptr,samplers);
        require(value.valid() && !A::matches(*value),"local substitution injection refuses optional proof");
    }
    {
        Shader::ShaderManager manager; manager.setShaderPath(pbr); auto globals = defines(pbr);
        globals["unused_oversize_contract_input"] = std::string(4097,'x'); manager.setGlobalDefines(globals);
        auto value = manager.getProgram("compatibility/objects",{},nullptr,samplers);
        require(value.valid() && !A::matches(*value),"over-limit optional metadata must preserve usable normal program");
    }
    require(A::liveMetadataBytes() == before && before <= A::MaximumMetadataBytes,"all optional metadata must roll back without debt");
    std::cout << "ASSET_CONTRACT_PASS stock=1 pbr=1 source_negative=1 link_negative=1 bindings_negative=1 defines_negative=1 epoch_negative=1 path_negative=1 indirect_unchanged=1 metadata_bounded=1\n";
    return 0;
}
catch (const std::exception& error) {std::cerr << "ASSET_CONTRACT_FAILURE " << error.what() << '\n'; return 1;}
