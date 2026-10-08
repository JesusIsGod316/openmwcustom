#include <components/shader/assetgeometrycontract.hpp>
#include <components/shader/indirectprojectioncontract.hpp>
#include <components/shader/shadermanager.hpp>
#include <components/sceneutil/generatedstaticlod.hpp>
#include <components/settings/settings.hpp>
#include <components/settings/parser.hpp>
#include <components/settings/values.hpp>
#include <osgViewer/Viewer>
#include <osg/Geometry>
#include <osg/UserDataContainer>
#include <chrono>
#include <barrier>
#include <thread>
#include <array>
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
    osg::ref_ptr<osg::Geometry> source(osg::Program& program)
    {
        osg::ref_ptr<osg::Geometry> geometry = new osg::Geometry;
        geometry->setDataVariance(osg::Object::STATIC);
        osg::ref_ptr<osg::Vec3Array> vertices = new osg::Vec3Array;
        vertices->push_back({0,0,0}); vertices->push_back({1,0,0}); vertices->push_back({0,1,0});
        geometry->setVertexArray(vertices);
        osg::ref_ptr<osg::DrawElementsUShort> indices = new osg::DrawElementsUShort(GL_TRIANGLES);
        indices->push_back(0); indices->push_back(1); indices->push_back(2); geometry->addPrimitiveSet(indices);
        geometry->getOrCreateStateSet()->setAttribute(&program);
        return geometry;
    }
    unsigned userObjects(const osg::Object& value)
    { return value.getUserDataContainer() ? value.getUserDataContainer()->getNumUserObjects() : 0; }
}
int main(int argc,char** argv) try
{
    std::cout << std::unitbuf;
    require(argc == 4 || (argc == 5 && std::string_view(argv[4]) == "--temporal-only"),
        "usage: p9-asset-shader-contract stockShaderRoot stagedPbrShaderRoot defaultsCfg [--temporal-only]");
    const bool temporalOnly = argc == 5;
#ifdef _WIN32
    _putenv_s("OPENMW_P9_ASSET_MASK",temporalOnly ? "0" : "1");
    if (temporalOnly) _putenv_s("OPENMW_P9_DYNAMIC_MOTION","1");
#else
    setenv("OPENMW_P9_ASSET_MASK",temporalOnly ? "0" : "1",1);
    if (temporalOnly) setenv("OPENMW_P9_DYNAMIC_MOTION","1",1);
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
        require(Shader::IndirectProjection::matches(*value) == (!temporalOnly && family == A::Family::Stock),
            "indirect authorization must stay stock-only and cannot follow the temporal-only source flag");
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
    for (const auto& [root,family] : {std::pair{stock,A::Family::Stock},std::pair{pbr,A::Family::Pbr}})
    {
        Shader::ShaderManager manager; auto value = program(manager,root); auto snapshot = A::capture(*value);
        require(bool(snapshot),"cached regeneration setup"); auto graph = source(*value);
        require(SceneUtil::GeneratedStaticLod::certify(*graph) == SceneUtil::GeneratedStaticLod::Refusal::None,
            "actual source L1 qualification before regeneration");
        const auto originalRecipe = std::string(snapshot->recipe());
        const auto generation = manager.assetGeneration(), bytes = A::liveMetadataBytes();
        const auto programObjects = userObjects(*value); const auto sourceObjects = userObjects(*value->getShader(0));
        auto globals = manager.getGlobalDefines();
        manager.setGlobalDefines(globals); manager.setGlobalDefines(globals);
        require(manager.assetGeneration() == generation && A::current(*snapshot,*value) == A::Refusal::None
            && A::liveMetadataBytes() == bytes,"identical global recipes do not invalidate or allocate");
        globals["forcePPL"] = "0"; manager.setGlobalDefines(globals);
        require(manager.assetGeneration() == generation + 1 && A::current(*snapshot,*value) == A::Refusal::Epoch,
            "actual global regeneration invalidates prior immutable proof");
        auto cached = manager.getProgram("compatibility/objects",{},nullptr,samplers);
        require(cached == value,"approved regeneration retains the actual cached producer");
        auto refreshed = A::capture(*cached);
        require(bool(refreshed) && refreshed->family() == family && refreshed->define("forcePPL") == "0"
            && refreshed->recipe() != originalRecipe,"cached program reacquires exact regenerated recipe");
        require(A::current(*refreshed,*cached) == A::Refusal::None
            && SceneUtil::GeneratedStaticLod::certify(*graph) == SceneUtil::GeneratedStaticLod::Refusal::None,
            "fresh cached proof restores actual L1 source qualification");
        require(A::current(*snapshot,*cached) == A::Refusal::Epoch,"new publication never refreshes an old snapshot");
        require(userObjects(*cached) == programObjects && userObjects(*cached->getShader(0)) == sourceObjects,
            "regeneration replaces immutable proof slots without duplicate objects");
        globals["forcePPL"] = "1"; manager.setGlobalDefines(globals);
        auto restored = A::capture(*cached);
        require(bool(restored) && restored->recipe() == originalRecipe
            && A::current(*snapshot,*cached) == A::Refusal::Epoch
            && A::current(*refreshed,*cached) == A::Refusal::Epoch,
            "equivalent restored code gets a fresh epoch and does not revive old snapshots");
        for (unsigned repetition = 0; repetition < 12; ++repetition)
        {
            globals["forcePPL"] = repetition % 2 ? "1" : "0"; manager.setGlobalDefines(globals);
            require(A::matches(*cached) && userObjects(*cached) == programObjects
                && userObjects(*cached->getShader(0)) == sourceObjects
                && A::liveMetadataBytes() <= A::MaximumMetadataBytes,
                "repeated fresh publication stays bounded with old snapshots retained");
        }
        std::cout << "ASSET_CONTRACT_REFRESH family=" << A::familyName(family)
            << " cached=1 old_snapshot_rejected=1 current_recipe=1 l1_source=1 no_op=1 slot_replaced=1 bounded=1\n";
    }
    {
        Shader::ShaderManager manager; auto value = program(manager,pbr);
        auto globals = manager.getGlobalDefines();
        constexpr unsigned rounds = 16;
        std::barrier acquired(3), regenerated(3);
        std::atomic_bool coherent{true}, stale{true};
        std::array<std::thread,2> readers;
        for (auto& reader : readers) reader = std::thread([&] {
            for (unsigned round = 0; round < rounds; ++round)
            {
                std::optional<A::Snapshot> snapshot;
                {
                    A::ProducerReadScope producer;
                    snapshot = A::capture(*value);
                    const auto generation = manager.assetGeneration(); // atomic identity, never manager mutex
                    const auto original = value->getShader(0)->getShaderSource();
                    acquired.arrive_and_wait(); // actual writer now contends with both retained readers
                    for (unsigned check = 0; check < 32; ++check)
                        if (!snapshot || A::current(*snapshot,*value) != A::Refusal::None
                            || value->getShader(0)->getShaderSource() != original
                            || manager.assetGeneration() != generation) coherent = false;
                }
                regenerated.arrive_and_wait();
                if (!snapshot || A::current(*snapshot,*value) == A::Refusal::None) stale = false;
            }
        });
        for (unsigned round = 0; round < rounds; ++round)
        {
            acquired.arrive_and_wait();
            globals["clamp"] = globals["clamp"] == "1" ? "0" : "1";
            manager.setGlobalDefines(globals);
            regenerated.arrive_and_wait();
        }
        for (auto& reader : readers) reader.join();
        require(coherent && stale && A::matches(*value),
            "two producer readers observed a partial global regeneration or stale snapshot revival");
        std::cout << "ASSET_CONTRACT_CONCURRENCY readers=2 writes=" << rounds
            << " coherent_snapshots=1 no_deadlock=1 stale_refused=1\n";
    }
    for (unsigned mutation = 0; mutation < 8; ++mutation)
    {
        Shader::ShaderManager manager; auto value = program(manager,pbr); auto snapshot = A::capture(*value);
        require(bool(snapshot),"cached mutation setup"); auto* shader = value->getShader(0);
        switch (mutation)
        {
            case 0: shader->setShaderSource(shader->getShaderSource()+"\n// unreviewed producer"); break;
            case 1: shader->addCodeInjection(-1,"unreviewed"); break;
            case 2: shader->setShaderRequirements({"unreviewed"}); break;
            case 3: value->setShaderDefines({"unreviewed"}); break;
            case 4: value->addBindAttribLocation("unreviewed",13); break;
            case 5: value->addBindUniformBlock("unreviewed",7); break;
            case 6: value->addShader(new osg::Shader(osg::Shader::GEOMETRY,"void main(){}")); break;
            case 7: value->setParameter(GL_GEOMETRY_VERTICES_OUT_EXT,value->getParameter(GL_GEOMETRY_VERTICES_OUT_EXT)+1); break;
        }
        require(!A::matches(*value),"unapproved source or linking mutation rejects before regeneration");
        auto globals = manager.getGlobalDefines(); globals["forcePPL"] = "0"; manager.setGlobalDefines(globals);
        auto cached = manager.getProgram("compatibility/objects",{},nullptr,samplers);
        require(cached.valid() && cached == value && !A::matches(*cached) && !A::capture(*cached),
            "approved global update cannot bless prior unapproved source or linking mutation");
        require(A::current(*snapshot,*cached) != A::Refusal::None,"old mutated snapshot remains invalid");
        std::cout << "ASSET_CONTRACT_REFRESH_NEGATIVE mutation=" << mutation << " rejected=1\n";
    }
    {
        Folder folder(pbr); Shader::ShaderManager manager; auto value = program(manager,folder.path);
        auto snapshot = A::capture(*value); require(bool(snapshot),"reload setup"); append(folder.path/"compatibility/objects.frag");
        std::filesystem::last_write_time(folder.path/"compatibility/objects.frag",std::filesystem::file_time_type::clock::now()+std::chrono::seconds(1));
        manager.triggerShaderReload(); require(A::current(*snapshot,*value) == A::Refusal::Epoch,"reload request invalidates before publication");
        osgViewer::Viewer viewer; manager.update(viewer); require(!A::matches(*value),"actual touched-shader update remains fail closed");
        auto globals = manager.getGlobalDefines(); globals["forcePPL"] = "0"; manager.setGlobalDefines(globals);
        require(manager.getProgram("compatibility/objects",{},nullptr,samplers) == value && !A::capture(*value),
            "global regeneration cannot reuse a cleared exact raw/include proof after reload");
    }
    {
        Shader::ShaderManager manager; auto value = program(manager,pbr); auto old = A::capture(*value);
        require(bool(old),"cached invalid substitution setup");
        auto globals = manager.getGlobalDefines(); globals["forcePPL"] = "1\n#define unreviewed 1";
        manager.setGlobalDefines(globals);
        require(value.valid() && !A::matches(*value) && !A::capture(*value)
            && A::current(*old,*value) != A::Refusal::None,
            "cached global substitution injection preserves original shader objects but refuses fresh proof");
    }
    {
        Shader::ShaderManager manager; auto value = program(manager,pbr);
        std::vector<A::Snapshot> retained;
        auto globals = manager.getGlobalDefines(); bool exhausted = false;
        for (unsigned generation = 0; generation < 64; ++generation)
        {
            auto snapshot = A::capture(*value);
            if (!snapshot) {exhausted = true; break;}
            retained.push_back(*snapshot);
            globals["forcePPL"] = generation % 2 ? "1" : "0"; manager.setGlobalDefines(globals);
            require(value.valid() && A::liveMetadataBytes() <= A::MaximumMetadataBytes
                && A::current(retained.front(),*value) != A::Refusal::None,
                "held old generations stay invalid and charged under the unchanged global cap");
        }
        require(exhausted && !retained.empty() && !A::capture(*value),
            "bounded metadata pressure refuses fresh optional proof without replacing normal program");
        std::cout << "ASSET_CONTRACT_REFRESH_CAPACITY retained=" << retained.size()
            << " metadata=" << A::liveMetadataBytes() << " cap=" << A::MaximumMetadataBytes << " fallback=1\n";
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
    std::cout << "ASSET_CONTRACT_PASS stock=1 pbr=1 source_negative=1 link_negative=1 bindings_negative=1 defines_negative=1 epoch_negative=1 path_negative=1 indirect_unchanged=1 metadata_bounded=1 cached_refresh=1 l1_source=1 old_snapshot_rejected=1 no_op=1 mutation_refresh_rejected=1 refresh_capacity=1 temporal_only=" << temporalOnly << '\n';
    return 0;
}
catch (const std::exception& error) {std::cerr << "ASSET_CONTRACT_FAILURE " << error.what() << '\n'; return 1;}
