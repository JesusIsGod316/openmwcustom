#include <components/files/collections.hpp>
#include <components/esm3/esmreader.hpp>
#include <components/esm3/loadstat.hpp>
#include <components/nif/niffile.hpp>
#include <components/nifosg/nifloader.hpp>
#include <components/resource/bgsmfilemanager.hpp>
#include <components/resource/derivedassetcache.hpp>
#include <components/resource/imagemanager.hpp>
#include <components/sceneutil/generatedstaticlod.hpp>
#include <components/sceneutil/optimizer.hpp>
#include <components/sceneutil/pagingwork.hpp>
#include <components/sceneutil/temporalvertexlineage.hpp>
#include <components/sceneutil/lightmanager.hpp>
#include <components/sceneutil/shadow.hpp>
#include <components/shader/indirectprojectioncontract.hpp>
#include <components/shader/shadermanager.hpp>
#include <components/shader/shadervisitor.hpp>
#include <components/vfs/manager.hpp>
#include <components/vfs/registerarchives.hpp>
#include <apps/openmw/mwrender/vismask.hpp>
#include <osg/LOD>
#include <osg/MatrixTransform>
#include <osg/NodeCallback>
#include <osg/TexMat>
#include <osg/Texture2D>
#include <osg/Uniform>
#include <osgViewer/Viewer>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <set>
#include <vector>

namespace L1 = SceneUtil::GeneratedStaticLod;
namespace
{
    void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
    struct Geometries final : osg::NodeVisitor
    {
        Geometries() : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN) {}
        std::vector<osg::Geometry*> values;
        void apply(osg::Geometry& value) override { values.push_back(&value); }
    };
    struct Pixels final : osg::Camera::DrawCallback
    {
        mutable std::array<unsigned char, 128 * 128 * 4> data{};
        mutable GLenum error = GL_NO_ERROR;
        void operator()(osg::RenderInfo&) const override
        { glReadPixels(0,0,128,128,GL_RGBA,GL_UNSIGNED_BYTE,data.data()); error = glGetError(); }
    };
    struct Programs final : osg::NodeVisitor
    {
        Programs() : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN) {}
        std::vector<osg::Program*> values;
        void apply(osg::Node& node) override
        {
            if (auto* state = node.getStateSet())
                if (auto* program = dynamic_cast<osg::Program*>(state->getAttribute(osg::StateAttribute::PROGRAM));
                    program && std::find(values.begin(),values.end(),program) == values.end()) values.push_back(program);
            traverse(node);
        }
    };
    struct EffectiveMaterial final : osg::NodeVisitor
    {
        explicit EffectiveMaterial(const osg::Geometry* value) : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN), target(value) {}
        const osg::Geometry* target;
        std::vector<const osg::StateSet*> ancestry;
        osg::ref_ptr<osg::StateSet> result;
        unsigned paths = 0;
        void apply(osg::Node& node) override
        {
            if (node.getStateSet()) ancestry.push_back(node.getStateSet());
            if (&node == target)
            {
                ++paths; result = new osg::StateSet;
                for (const auto* state : ancestry) result->merge(*state);
            }
            traverse(node);
            if (node.getStateSet()) ancestry.pop_back();
        }
    };
    struct Callback final : osg::NodeCallback {};
    struct PagingOptimize final : SceneUtil::Optimizer::IsOperationPermissibleForObjectCallback
    {
        bool isOperationPermissibleForObjectImplementation(const SceneUtil::Optimizer*,const osg::Drawable*,unsigned) const override { return true; }
        bool isOperationPermissibleForObjectImplementation(const SceneUtil::Optimizer*,const osg::Node* node,unsigned) const override
        { return node->getDataVariance() != osg::Object::DYNAMIC; }
    };
    struct Topology final : osg::NodeVisitor
    {
        Topology() : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN) {}
        void apply(osg::Node& node) override
        {
            std::cout << "L1_CONTROL_NODE class=" << node.libraryName() << "::" << node.className()
                << " mask=" << node.getNodeMask() << " culling=" << node.isCullingActive()
                << " transform=" << bool(node.asTransform()) << '\n';
            traverse(node);
        }
    };
    osg::ref_ptr<osg::Node> privateClone(const osg::Node& node)
    {
        return osg::clone(&node, osg::CopyOp::DEEP_COPY_NODES | osg::CopyOp::DEEP_COPY_DRAWABLES
            | osg::CopyOp::DEEP_COPY_ARRAYS | osg::CopyOp::DEEP_COPY_PRIMITIVES);
    }
    // Only used AFTER the original ancestry certificate. Reproduce the static
    // production paging CopyOp's mask/name/metadata handling for a fair strong
    // optimizer control; do not qualify controller-stripped arbitrary graphs.
    struct PagingCopy final : osg::CopyOp
    {
        PagingCopy() : osg::CopyOp(DEEP_COPY_NODES | DEEP_COPY_DRAWABLES | DEEP_COPY_ARRAYS | DEEP_COPY_PRIMITIVES) {}
        osg::Node* operator()(const osg::Node* node) const override
        {
            if (!(node->getNodeMask() & ~MWRender::Mask_UpdateVisitor)) return nullptr;
            if (const auto* drawable = node->asDrawable()) return operator()(drawable);
            auto* cloned = static_cast<osg::Node*>(node->clone(*this));
            cloned->setName(""); cloned->setUserDataContainer(nullptr); cloned->setDataVariance(osg::Object::STATIC);
            return cloned;
        }
        osg::Drawable* operator()(const osg::Drawable* drawable) const override
        {
            if (!(drawable->getNodeMask() & ~MWRender::Mask_UpdateVisitor)) return nullptr;
            auto* cloned = static_cast<osg::Drawable*>(drawable->clone(*this));
            cloned->setName(""); cloned->setUserDataContainer(nullptr); cloned->setDataVariance(osg::Object::STATIC);
            return cloned;
        }
    };
}

int main(int argc, char** argv) try
{
#ifdef _WIN32
    _putenv_s("OPENMW_P9_ASSET_MASK", "1");
#else
    setenv("OPENMW_P9_ASSET_MASK", "1", 1);
#endif
    require(argc >= 5, "usage: p9-generated-static-lod --probe|--draw|--static-draw openmw.cfg sourceRoot [activePlugin] mesh1 [mesh2 ...]");
    const bool staticRecords = std::string(argv[1]) == "--static-draw";
    const bool draw = std::string(argv[1]) == "--draw" || staticRecords;
    const int firstMesh = staticRecords ? 5 : 4;
    require(argc > firstMesh, "bounded corpus model required");
    Files::PathContainer directories; std::vector<std::string> archives, content;
    std::ifstream config(std::filesystem::u8path(argv[2])); require(bool(config), "read-only loadout config unavailable");
    for (std::string line; std::getline(config, line);)
    {
        if (line.starts_with("data="))
        {
            std::string path = line.substr(5);
            if (path.size() > 1 && path.front() == '"' && path.back() == '"') path = path.substr(1,path.size()-2);
            const auto directory = std::filesystem::u8path(path);
            if (std::filesystem::is_directory(directory)) directories.push_back(directory);
            else std::cout << "L1_CORPUS_MISSING_DIRECTORY " << path << '\n';
        }
        if (line.starts_with("fallback-archive=")) archives.push_back(line.substr(17));
        if (line.starts_with("content=")) content.push_back(line.substr(8));
    }
    require(!directories.empty(), "loadout has no source directories");
    VFS::Manager vfs; VFS::registerArchives(&vfs, Files::Collections(directories), archives, true, nullptr); vfs.buildIndex();
    std::set<std::string> staticModels;
    if (staticRecords)
    {
        const std::string plugin = argv[4];
        require(std::find(content.begin(),content.end(),plugin) != content.end(), "fixture static plugin must be active in the loadout");
        const VFS::Path::Normalized pluginPath(plugin);
        ESM::ESMReader records; records.open(vfs.get(pluginPath),std::filesystem::u8path(plugin));
        unsigned scanned = 0, statics = 0;
        while (records.hasMoreRecs())
        {
            require(++scanned <= 250000, "bounded real static record scan");
            const auto name = records.getRecName(); records.getRecHeader();
            if (name.toInt() != ESM::REC_STAT) { records.skipRecord(); continue; }
            ESM::Static value; bool deleted = false; value.load(records,deleted); ++statics;
            if (deleted) continue;
            const VFS::Path::Normalized model("meshes/" + value.mModel.getNormalized().value()); staticModels.insert(model.value());
            for (int i = firstMesh; i < argc; ++i)
                if (model == VFS::Path::Normalized(argv[i])) std::cout << "L1_CORPUS_STATIC id=" << value.mId.toString()
                    << " model=" << model.value() << " active_plugin=" << plugin << '\n';
        }
        std::cout << "L1_CORPUS_STATIC_SCAN records=" << scanned << " static_records=" << statics << '\n';
    }
    Resource::ImageManager images(&vfs, 0); Resource::BgsmFileManager materials(&vfs, 0);
    osg::ref_ptr<SceneUtil::LightManager> lighting = new SceneUtil::LightManager;
    Shader::ShaderManager shaders; shaders.setShaderPath(std::filesystem::u8path(argv[3]) / "files/shaders");
    auto defines = Shader::getDefaultDefines();
    for (const auto& [name,value] : SceneUtil::ShadowManager::getShadowsDisabledDefines()) defines[name] = value;
    for (const auto& [name,value] : lighting->getLightDefines()) defines[name] = value;
    shaders.setGlobalDefines(defines);
    Shader::ShaderVisitor shaderVisitor(shaders, images, "objects");
    shaderVisitor.setAutoUseNormalMaps(true); shaderVisitor.setNormalMapPattern("_n"); shaderVisitor.setNormalHeightMapPattern("_nh");
    shaderVisitor.setAutoUseSpecularMaps(true); shaderVisitor.setSpecularMapPattern("_s");
    osg::ref_ptr<osg::Camera> mainCamera = new osg::Camera;
    L1::Result selected; std::string selectedName;
    osg::ref_ptr<osg::Node> selectedSource;
    for (int argument = firstMesh; argument < argc && argument < firstMesh + 64; ++argument)
    {
        const VFS::Path::Normalized path = VFS::Path::toNormalized(std::string_view(argv[argument]));
        try
        {
            if (staticRecords) require(staticModels.contains(path.value()), "model has no live STATIC record in the active plugin");
            const auto estimate = vfs.getOpenEstimate(path);
            require(estimate && estimate->sourceBytes && estimate->sourceBytes <= 8 * 1024 * 1024
                && estimate->openPeakBytes <= 8 * 1024 * 1024, "real corpus provider must qualify before source open");
            auto stream = vfs.get(path);
            Resource::DerivedAssets::Bytes bytes(static_cast<std::size_t>(estimate->sourceBytes));
            stream->read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
            require(stream->gcount() == static_cast<std::streamsize>(bytes.size()) && !stream->bad()
                && stream->peek() == std::char_traits<char>::eof(), "corpus metadata must match winning source stream length");
            Nif::NIFFile file(path); Nif::Reader reader(file,nullptr); reader.parse(vfs.get(path));
            auto loaded = NifOsg::Loader::load(file,&images,&materials);
            const auto sourceCertificate = L1::certify(*loaded);
            std::cout << "L1_CORPUS_SOURCE " << path.value() << " winning=" << vfs.getArchive(path)
                << " original_certificate=" << static_cast<unsigned>(sourceCertificate) << '\n';
            if (sourceCertificate != L1::Refusal::None) continue;
            loaded->accept(shaderVisitor);
            require(L1::certify(*loaded) == L1::Refusal::None, "shipped shader certificate must survive source preparation");
            // Production optimizes a partition Group containing instance roots,
            // never the loaded NIF transform as the optimizer's unremovable root.
            osg::ref_ptr<osg::Group> control = new osg::Group;
            control->addChild(PagingCopy{}(loaded.get()));
            SceneUtil::Optimizer optimizer;
            optimizer.setIsOperationPermissibleForObjectCallback(new PagingOptimize);
            optimizer.optimize(control, SceneUtil::Optimizer::FLATTEN_STATIC_TRANSFORMS | SceneUtil::Optimizer::REMOVE_REDUNDANT_NODES
                | SceneUtil::Optimizer::MERGE_GEOMETRY | SceneUtil::Optimizer::MERGE_GEODES | SceneUtil::Optimizer::SHARE_DUPLICATE_STATE);
            // The builder decorates its private control with actual draw
            // receipts. Source-certificate negatives must start from the
            // certified pre-build graph, never erase those later callbacks.
            osg::ref_ptr<osg::Node> certificateSource;
            if(!selected.node) certificateSource=privateClone(*control);
            auto result = L1::build(*control, path.value() + "|" + Resource::DerivedAssets::digest(bytes),mainCamera,false);
            const auto* cacheDirectory=std::getenv("OPENMW_P9_DERIVED_CACHE_DIR");
            if (result.node && certificateSource && cacheDirectory && *cacheDirectory)
            {
                auto cachedControl = privateClone(*certificateSource);
                // certificateSource only exists for the first positive source.
                if (cachedControl)
                {
                    const auto hits = L1::Counters::cacheFastHits.load();
                    const auto cached = L1::build(*cachedControl,path.value()+"|"+Resource::DerivedAssets::digest(bytes),mainCamera,false,false,true);
                    require(cached.node && cached.derivedIndices==result.derivedIndices
                        && L1::Counters::cacheFastHits>hits, "verified hit must reuse identical reduced indices through fast admission");
                    Geometries expected,actual; result.node->derived()->accept(expected);cached.node->derived()->accept(actual);
                    require(expected.values.size()==actual.values.size(), "cache hit must preserve derived leaf layout");
                    for(std::size_t leaf=0;leaf<expected.values.size();++leaf)
                    {
                        const auto* a=expected.values[leaf];const auto* b=actual.values[leaf];
                        require(a->getNumPrimitiveSets()==b->getNumPrimitiveSets(), "cache hit changed primitive layout");
                        for(unsigned p=0;p<a->getNumPrimitiveSets();++p)
                        {
                            const auto* x=a->getPrimitiveSet(p);const auto* y=b->getPrimitiveSet(p);
                            require(x->getMode()==y->getMode() && x->getNumIndices()==y->getNumIndices(), "cache hit changed primitive recipe");
                            for(unsigned i=0;i<x->getNumIndices();++i) require(x->index(i)==y->index(i), "cache hit changed exact index payload");
                        }
                    }
                    require(L1::cookPeakBytes(*certificateSource,true)<L1::cookPeakBytes(*certificateSource),
                        "cache-only admission must exclude simplifier scratch");
                    std::cout << "L1_CACHE_FAST_PROOF exact_indices=1 reduced_leaf_layout=1 scratch_excluded=1 cache_fast_hits="
                        << L1::Counters::cacheFastHits.load() << '\n';
                }
            }
            std::cout << "L1_CORPUS_RESULT " << path.value() << " original_indices=" << result.originalIndices
                << " tier_indices=" << result.derivedIndices << " refusal=" << static_cast<unsigned>(result.refusal)
                << " error=" << (result.node ? result.node->error() : 0.f) << '\n';
            if (result.node && !selected.node) { selected = result; selectedName = path.value(); selectedSource=certificateSource; }
        }
        catch (const std::exception& error) { std::cout << "L1_CORPUS_REFUSED " << path.value() << " reason=" << error.what() << '\n'; }
    }
    require(selected.node.valid(), "no positive real static NIF LOD reduction; lane remains unproved");
    require(selectedSource.valid() && L1::certify(*selectedSource)==L1::Refusal::None,
        "source-certificate fixtures require the exact immutable pre-build control without derived receipts");
    require(selected.derivedIndices < selected.originalIndices && !selected.readiness->ready, "reduced immutable tier starts unprepared");
    {
        // These recipes passed the historical opaque/shader certificate, then
        // failed only after simplification. The exact shared preflight must
        // now reject before touching disk products or reduced index resources.
        for(unsigned recipe=0;recipe<3;++recipe)
        {
            osg::ref_ptr<osg::Node> control=privateClone(*selectedSource);
            Geometries leaves;control->accept(leaves);require(!leaves.values.empty(),"guard refusal fixture needs real optimized geometry");
            auto* leaf=leaves.values.front();
            osg::ref_ptr<osg::StateSet> state=leaf->getStateSet()
                ? new osg::StateSet(*leaf->getStateSet(),osg::CopyOp::SHALLOW_COPY) : new osg::StateSet;
            leaf->setStateSet(state);
            const auto expected=recipe==0 ? L1::GuardRefusal::AttributeRecipe
                : recipe==1 ? L1::GuardRefusal::TextureImage : L1::GuardRefusal::UniformRecipe;
            if(recipe==0) state->setTextureAttribute(9,new osg::TexMat);
            else if(recipe==1) state->setTextureAttribute(9,new osg::Texture2D);
            else state->addUniform(new osg::Uniform(osg::Uniform::FLOAT,"unused_guard_fixture",4097));
            require(L1::certify(*control)==L1::Refusal::None,"negative guard recipe must reach the historically weaker original certificate");
            const auto beforeDisk=L1::Counters::diskHits.load(),beforeIndices=L1::Counters::newIndexGpuBytes.load();
            const auto result=L1::build(*control,selectedName+"|guard-preflight-"+std::to_string(recipe),mainCamera,false);
            require(!result.node && result.failureStep==L1::BuildStep::Guard && result.guardPhase==L1::GuardPhase::Preflight
                && result.guardRefusal.reason==expected && result.newIndexBytes==0
                && L1::Counters::diskHits==beforeDisk && L1::Counters::newIndexGpuBytes==beforeIndices,
                "exact material recipe must refuse before simplification/cache work with bounded inner reason");
            require(recipe==2 || result.guardRefusal.textureUnit==9,"texture rejection must name exact unsupported unit");
            require(leaf->getDrawCallback()==nullptr,"refused preflight must preserve ordinary callback ownership");
            std::cout<<"L1_GUARD_PREFLIGHT reason="<<L1::guardRefusalName(expected)
                <<" phase=preflight before_simplification=1 ordinary_untouched=1 texture_unit="<<result.guardRefusal.textureUnit<<'\n';
        }
    }
    {
        // One real reducible optimized leaf plus an irreducible triangle must
        // share every ordinary stream and retain the unchanged leaf's exact
        // PrimitiveSet/EBO, while charging only the reduced leaf's new indices.
        Geometries sourceLeaves; selectedSource->accept(sourceLeaves);
        require(!sourceLeaves.values.empty(),"real immutable post-optimizer source leaf required");
        osg::ref_ptr<osg::Node> reducible=privateClone(*selectedSource);
        osg::ref_ptr<osg::Geometry> triangle=new osg::Geometry(*sourceLeaves.values.front(),osg::CopyOp::SHALLOW_COPY);
        triangle->setDrawCallback(nullptr);triangle->setUserDataContainer(nullptr);
        auto originalPrimitive=triangle->getPrimitiveSet(0);
        require(originalPrimitive->getNumIndices()>=3,"real source triangle required");
        osg::ref_ptr<osg::DrawElementsUInt> only=new osg::DrawElementsUInt(GL_TRIANGLES);
        for (unsigned i=0;i<3;++i) only->push_back(originalPrimitive->index(i));
        only->setElementBufferObject(new osg::ElementBufferObject);triangle->setPrimitiveSet(0,only);
        // Preserve the actual effective ancestry of this copied leaf.
        EffectiveMaterial effective(sourceLeaves.values.front());selectedSource->accept(effective);
        require(effective.paths==1 && effective.result,"one exact source material ancestry required");
        triangle->setStateSet(effective.result);
        osg::ref_ptr<osg::Group> cohort=new osg::Group;cohort->setDataVariance(osg::Object::STATIC);
        cohort->addChild(reducible);cohort->addChild(triangle);
        const auto result=L1::build(*cohort,selectedName+"|mixed-reduced-irreducible",mainCamera,false);
        require(result.node && result.newIndexBytes>0,"mixed actual geometry cohort must reduce at least one leaf");
        Geometries control,tier;result.node->original()->accept(control);result.node->derived()->accept(tier);
        require(control.values.size()==tier.values.size(),"mixed leaf population must remain exact");
        std::uint64_t actualNewIndices=0;unsigned unchanged=0;
        for (unsigned i=0;i<control.values.size();++i)
        {
            const auto* original=control.values[i];const auto* derived=tier.values[i];
            require(original->getVertexArray()==derived->getVertexArray()
                && original->getVertexArray()->getVertexBufferObject()==derived->getVertexArray()->getVertexBufferObject(),
                "mixed leaves must share ordinary vertex array and VBO identity");
            if (original->getPrimitiveSet(0)==derived->getPrimitiveSet(0)) ++unchanged;
            else actualNewIndices+=derived->getPrimitiveSet(0)->getNumIndices()*sizeof(unsigned);
        }
        require(unchanged>0 && result.newIndexBytes==actualNewIndices,
            "unchanged primitive must retain its exact EBO and incur no new index debt");
        std::cout<<"L1_MIXED_INDEX_RESOURCES unchanged_primitive_ebo_identity=1 shared_vertices_vbos=1 unchanged_leaves="<<unchanged
            <<" new_index_bytes="<<result.newIndexBytes<<" exact_new_index_debt=1\n";
    }
    const auto builtBeforeDemand = L1::Counters::built.load(), diskBeforeDemand = L1::Counters::diskHits.load();
    require(!L1::build(*selected.node->original(),selectedName,mainCamera).node,
        "visible demand without optional worker must not cook a production tier");
    std::atomic_bool optionalCancel{false};
    {
        SceneUtil::PagingWorkScope optional(&optionalCancel,SceneUtil::PagingWorkScope::Phase::OptionalOptimization);
        require(!L1::build(*selected.node->original(),selectedName,mainCamera).node,
            "optional phase without speculative host scope must not cook a production tier");
    }
    require(L1::Counters::built == builtBeforeDemand && L1::Counters::diskHits == diskBeforeDemand,
        "unadmitted demand cannot reach derived cache or tier publication");
    osg::ref_ptr<osg::Group> markerRoot = new osg::Group;
    markerRoot->addChild(privateClone(*selectedSource));
    osg::ref_ptr<osg::Group> emptyMarker = new osg::Group;
    emptyMarker->setNodeMask(0); emptyMarker->setDataVariance(osg::Object::STATIC);
    emptyMarker->setUserValue("recordIndex",1u); markerRoot->addChild(emptyMarker);
    require(L1::certify(*markerRoot) == L1::Refusal::None,
        "static loader empty hidden marker has no replaced rendering population");
    emptyMarker->addChild(privateClone(*selectedSource));
    require(L1::certify(*markerRoot) == L1::Refusal::Graph,
        "hidden drawable population cannot use empty-marker qualification");
    emptyMarker->removeChildren(0,emptyMarker->getNumChildren());
    emptyMarker->setCullCallback(new Callback);
    require(L1::certify(*markerRoot) == L1::Refusal::Graph,
        "hidden loader marker callback cannot be erased to qualify a tier");
    Geometries originals, tiers; selected.node->original()->accept(originals); selected.node->derived()->accept(tiers);
    require(originals.values.size() == tiers.values.size() && !originals.values.empty(), "same material/drawable population");
    for (unsigned i = 0; i < originals.values.size(); ++i)
    {
        auto* original = originals.values[i]; auto* tier = tiers.values[i];
        require(original != tier && original->getStateSet() == tier->getStateSet(), "shared immutable material state");
        require(original->getVertexArray() == tier->getVertexArray() && original->getNormalArray() == tier->getNormalArray()
            && original->getColorArray() == tier->getColorArray(), "original vertex/normal/color streams preserved");
        require(original->getNumTexCoordArrays() == tier->getNumTexCoordArrays(), "same UV/tangent layout");
        require(SceneUtil::temporalVertexLineage(*original)
            && SceneUtil::temporalVertexLineage(*original) == SceneUtil::temporalVertexLineage(*tier), "same immutable temporal vertex lineage");
        require(L1::isDelegatingDrawCallback(tier->getDrawCallback()), "only exact production delegating receipt accepted");
        require(L1::temporalDrawRecipeCurrent(*tier), "current exact LOD receipt qualifies its temporal draw recipe");
        for (unsigned unit = 0; unit < original->getNumTexCoordArrays(); ++unit)
            require(original->getTexCoordArray(unit) == tier->getTexCoordArray(unit), "unchanged UV/tangent streams");
    }
    {
        auto* original=originals.values.front(); auto* tier=tiers.values.front();
        osg::ref_ptr<osg::Geometry> borrowed=new osg::Geometry(*tier,osg::CopyOp::SHALLOW_COPY);
        require(!L1::temporalDrawRecipeCurrent(*borrowed), "borrowed derived receipt cannot qualify another geometry owner");
        unsigned unit=0;
        while(unit<original->getNumTexCoordArrays() && !original->getTexCoordArray(unit)) ++unit;
        require(unit<original->getNumTexCoordArrays(), "actual source UV stream required for temporal recipe mutation regression");
        osg::ref_ptr<osg::Array> uv=original->getTexCoordArray(unit);
        original->setTexCoordArray(unit,osg::clone(uv.get(),osg::CopyOp::DEEP_COPY_ALL));
        require(!L1::temporalDrawRecipeCurrent(*tier), "changed original UV identity invalidates captured derived temporal recipe");
        original->setTexCoordArray(unit,uv);
        require(L1::temporalDrawRecipeCurrent(*tier), "restoring exact source UV recovers the frozen recipe without recertification");
        osg::ref_ptr<osg::StateSet> state=original->getStateSet();
        original->setStateSet(state ? new osg::StateSet(*state,osg::CopyOp::SHALLOW_COPY) : new osg::StateSet);
        require(!L1::temporalDrawRecipeCurrent(*tier), "changed original material identity invalidates captured derived temporal recipe");
        original->setStateSet(state);
        osg::ref_ptr<osg::PrimitiveSet> indices=tier->getPrimitiveSet(0);
        tier->setPrimitiveSet(0,osg::clone(indices.get(),osg::CopyOp::DEEP_COPY_ALL));
        require(!L1::temporalDrawRecipeCurrent(*tier), "changed derived indices invalidate captured temporal recipe");
        tier->setPrimitiveSet(0,indices);
        require(L1::temporalDrawRecipeCurrent(*tier), "exact restored material/index recipe remains current");
        std::cout<<"L1_TEMPORAL_DRAW_RECIPE exact_owner=1 borrowed_owner_refused=1 source_uv_refused=1 source_material_refused=1 derived_indices_refused=1 restored_exact=1\n";
    }
    // Real geometry regression: the same material in two spatial buckets and
    // ordinary population must return to the whole-chunk strong optimizer.
    EffectiveMaterial materialPath(originals.values.front()); selected.node->original()->accept(materialPath);
    osg::ref_ptr<osg::StateSet> mergeMaterial = materialPath.result;
    require(mergeMaterial.valid() && materialPath.paths == 1, "real fixture requires one exact stock material ancestry");
    const auto* materialProgram = dynamic_cast<const osg::Program*>(mergeMaterial->getAttribute(osg::StateAttribute::PROGRAM));
    require(materialProgram && Shader::IndirectProjection::matches(*materialProgram), "effective real stock program material required");
    osg::ref_ptr<osg::StateSet> equivalentMaterial = new osg::StateSet(*mergeMaterial,osg::CopyOp::SHALLOW_COPY);
    const std::array<L1::MaterialOwner,3> sharedOwners{{{mergeMaterial,2u},{equivalentMaterial,4u},{mergeMaterial,1u}}};
    const auto conflicts = L1::conflictingMaterialOwners(sharedOwners);
    require((conflicts & 6u) == 6u, "equivalent material across buckets/ordinary population returns both candidates");
    require(L1::conflictingMaterialOwners(sharedOwners,false) == ~0u, "unknown material ownership keeps full control topology");
    const std::array<L1::MaterialOwner,1> privateOwner{{{mergeMaterial,2u}}};
    require(!L1::conflictingMaterialOwners(privateOwner), "one privately owned material remains eligible");
    // The asset proof must read Geometry's own material, restore sibling
    // ancestry and apply actual State::push* override/protected precedence.
    osg::ref_ptr<osg::Geometry> leafOnly = new osg::Geometry;
    leafOnly->setStateSet(mergeMaterial);
    L1::EffectiveMaterialOwners leafOwners;
    leafOwners.observe(*leafOnly,2u);
    require(leafOwners.materials().size()==1 && !leafOwners.report().conflicting,
        "leaf-only material must not become null ownership");
    osg::ref_ptr<osg::Group> ancestryRoot = new osg::Group;
    ancestryRoot->getOrCreateStateSet()->addUniform(new osg::Uniform("ownership_test",1.f));
    osg::ref_ptr<osg::Group> firstBranch = new osg::Group, secondBranch = new osg::Group;
    firstBranch->getOrCreateStateSet()->addUniform(new osg::Uniform("ownership_test",2.f));
    firstBranch->addChild(leafOnly);
    osg::ref_ptr<osg::Geometry> secondLeaf = new osg::Geometry;
    secondLeaf->setStateSet(mergeMaterial); secondBranch->addChild(secondLeaf);
    ancestryRoot->addChild(firstBranch); ancestryRoot->addChild(secondBranch);
    L1::EffectiveMaterialOwners branchOwners; branchOwners.observe(*ancestryRoot,2u);
    std::set<float> branchValues;
    for (const auto& value:branchOwners.materials())
    { float uniform=0; require(value.state->getUniform("ownership_test") && value.state->getUniform("ownership_test")->get(uniform),"inherited uniform included"); branchValues.insert(uniform); }
    require(branchOwners.materials().size()==2 && branchValues==std::set<float>{1.f,2.f}
        && !branchOwners.report().conflicting,"sibling must recover parent state instead of previous branch");
    ancestryRoot->getOrCreateStateSet()->addUniform(new osg::Uniform("ownership_test",1.f),osg::StateAttribute::ON|osg::StateAttribute::OVERRIDE);
    L1::EffectiveMaterialOwners overrideOwners; overrideOwners.observe(*ancestryRoot,2u);
    require(overrideOwners.materials().size()==1,"unprotected child Uniform must obey ancestor OVERRIDE");
    firstBranch->getOrCreateStateSet()->addUniform(new osg::Uniform("ownership_test",2.f),osg::StateAttribute::ON|osg::StateAttribute::PROTECTED);
    L1::EffectiveMaterialOwners protectedOwners; protectedOwners.observe(*ancestryRoot,2u);
    require(protectedOwners.materials().size()==2,"protected child Uniform must replace ancestor OVERRIDE");
    L1::EffectiveMaterialOwners sharedEffective;
    sharedEffective.observe(*leafOnly,2u); sharedEffective.observe(*secondLeaf,4u); sharedEffective.observe(*secondLeaf,1u);
    require(sharedEffective.report().shared && (sharedEffective.report().conflicting&6u)==6u,
        "actual equal leaf materials across ordinary and spatial owners must refuse both candidates");
    L1::EffectiveMaterialOwners cohesiveEffective;
    cohesiveEffective.observe(*leafOnly,2u); cohesiveEffective.observe(*secondLeaf,4u);
    cohesiveEffective.replaceOwner(4u,2u);
    require(!cohesiveEffective.report().conflicting && cohesiveEffective.materials().front().owners==2u,
        "reuniting a fully private material family removes only the partition ownership conflict");
    osg::ref_ptr<osg::Geometry> nullLeaf = new osg::Geometry;
    L1::EffectiveMaterialOwners nullOwners; nullOwners.observe(*nullLeaf,2u);
    require(nullOwners.report().nullMaterial && nullOwners.report().conflicting==~0u,"material-less leaf keeps null refusal subtype");
    osg::ref_ptr<osg::Node> unknownLeaf = new osg::Node;
    L1::EffectiveMaterialOwners unknownOwners; unknownOwners.observe(*unknownLeaf,2u);
    require(unknownOwners.report().unknown && unknownOwners.report().conflicting==~0u,"unknown draw-bearing leaf retains conservative refusal");
    osg::ref_ptr<osg::Group> capacityRoot = new osg::Group;
    for (unsigned i=0;i<257;++i)
    {
        osg::ref_ptr<osg::Geometry> leaf = new osg::Geometry;
        leaf->getOrCreateStateSet()->addUniform(new osg::Uniform("ownership_capacity",float(i)));
        capacityRoot->addChild(leaf);
    }
    L1::EffectiveMaterialOwners capacityOwners; capacityOwners.observe(*capacityRoot,2u);
    require(capacityOwners.report().capacity && capacityOwners.report().conflicting==~0u
        && capacityOwners.materials().size()==256,"257th distinct material must refuse without unbounded ownership metadata");
    std::cout<<"L1_EFFECTIVE_MATERIAL_OWNERSHIP leaf_own=1 inherited=1 sibling_restore=1 override_protected=1 shared_ordinary=1 family_reunion=1 null_refused=1 unknown_refused=1 capacity_refused=1\n";
    osg::ref_ptr<osg::Group> wholeControl = new osg::Group;
    osg::ref_ptr<osg::Group> firstBucket = new osg::Group, secondBucket = new osg::Group;
    std::array<osg::ref_ptr<osg::Node>,3> instances;
    for (unsigned i = 0; i < instances.size(); ++i)
    {
        Geometries sourceGeometries; selectedSource->accept(sourceGeometries);
        require(!sourceGeometries.values.empty(), "actual certified pre-build geometry required for control merge fixture");
        auto cloned = privateClone(*sourceGeometries.values.front());
        cloned->setName(""); cloned->setUserDataContainer(nullptr); cloned->setDataVariance(osg::Object::STATIC);
        cloned->setStateSet(i == 1 ? equivalentMaterial : mergeMaterial);
        osg::ref_ptr<osg::MatrixTransform> instance = new osg::MatrixTransform(osg::Matrix::translate(i * 10.,0.,0.));
        instance->setDataVariance(osg::Object::STATIC); instance->addChild(cloned); instances[i] = instance;
    }
    firstBucket->addChild(instances[0]); secondBucket->addChild(instances[1]); wholeControl->addChild(instances[2]);
    if (conflicts & 2u) { firstBucket->removeChild(instances[0]); wholeControl->addChild(instances[0]); }
    if (conflicts & 4u) { secondBucket->removeChild(instances[1]); wholeControl->addChild(instances[1]); }
    require(wholeControl->containsNode(instances[0]) && wholeControl->containsNode(instances[1])
        && !firstBucket->getNumChildren() && !secondBucket->getNumChildren(), "exact original attachment before strong merge");
    Geometries beforeMerge; wholeControl->accept(beforeMerge); require(beforeMerge.values.size() == 3, "three actual compatible control instances");
    // Canonicalization mirrors SceneManager::shareState before compatible merge.
    for (auto* geometry : beforeMerge.values) geometry->setStateSet(mergeMaterial);
    SceneUtil::Optimizer wholeOptimizer; wholeOptimizer.setIsOperationPermissibleForObjectCallback(new PagingOptimize);
    wholeOptimizer.optimize(wholeControl,SceneUtil::Optimizer::FLATTEN_STATIC_TRANSFORMS
        | SceneUtil::Optimizer::REMOVE_REDUNDANT_NODES | SceneUtil::Optimizer::MERGE_GEOMETRY);
    Geometries afterMerge; wholeControl->accept(afterMerge);
    require(afterMerge.values.size() == 1 && afterMerge.values.front()->getNumPrimitiveSets() == 1,
        "rejected buckets preserve strong merge into one real indexed submission");
    std::cout << "L1_MATERIAL_OWNERSHIP_RETURN before=3 after=1 exact_attachments=1\n";
    const auto originalBound = selected.node->original()->getBound();
    require(selected.node->getBound() == originalBound, "conservative original union bound");
    osg::ref_ptr<osg::LOD> authored = new osg::LOD; authored->addChild(selectedSource);
    require(L1::certify(*authored) == L1::Refusal::Graph, "authored LOD preferred");
    osg::ref_ptr<osg::MatrixTransform> mirrored = new osg::MatrixTransform(osg::Matrix::scale(-1,1,1)); mirrored->addChild(selectedSource);
    require(L1::certify(*mirrored) == L1::Refusal::Transform, "negative transform rejected");
    auto callbackSource = privateClone(*selectedSource); callbackSource->setCullCallback(new Callback);
    require(L1::certify(*callbackSource) == L1::Refusal::Graph, "original callback provenance rejected");
    auto alphaSource = privateClone(*selectedSource);
    osg::ref_ptr<osg::StateSet> alphaState = alphaSource->getStateSet()
        ? new osg::StateSet(*alphaSource->getStateSet(),osg::CopyOp::SHALLOW_COPY) : new osg::StateSet;
    alphaState->setMode(GL_BLEND,osg::StateAttribute::ON); alphaSource->setStateSet(alphaState);
    require(L1::certify(*alphaSource) == L1::Refusal::Material, "transparent source rejected");
    std::atomic_bool cancel{true};
    bool cancelled = false;
    try { SceneUtil::PagingWorkScope scope(&cancel,SceneUtil::PagingWorkScope::Phase::OptionalOptimization);
        L1::build(*selectedSource,selectedName,mainCamera,false); }
    catch (const SceneUtil::PagingWorkCancelled&) { cancelled = true; }
    require(cancelled, "obsolete preparation exits at work checkpoint");
    auto stale = privateClone(*selectedSource);
    Geometries staleGeometries; stale->accept(staleGeometries);
    require(!SceneUtil::temporalVertexLineage(*staleGeometries.values.front()), "deep-copied arrays cannot inherit lineage");
    // Modified generation invalidates both representations of a private pair.
    auto* staleSource = staleGeometries.values.front();
    osg::ref_ptr<osg::Geometry> staleTier = new osg::Geometry(*staleSource,osg::CopyOp::SHALLOW_COPY);
    osg::ref_ptr<SceneUtil::TemporalVertexLineage> staleLineage = new SceneUtil::TemporalVertexLineage(staleSource->getVertexArray());
    SceneUtil::installTemporalVertexLineage(*staleSource,staleLineage);
    SceneUtil::installTemporalVertexLineage(*staleTier,staleLineage);
    staleSource->getVertexArray()->dirty();
    require(!SceneUtil::temporalVertexLineage(*staleSource) && !SceneUtil::temporalVertexLineage(*staleTier), "stale immutable stream generation rejected");
    require(SceneUtil::temporalVertexLineage(*originals.values.front()) && SceneUtil::temporalVertexLineage(*tiers.values.front()),
        "the independently mutated lineage pair must preserve the selected immutable candidate");
    if (!draw) return 0;

    osg::ref_ptr<osg::GraphicsContext::Traits> traits = new osg::GraphicsContext::Traits;
    traits->readDISPLAY(); traits->width=128; traits->height=128; traits->pbuffer=true; traits->doubleBuffer=false; traits->windowDecoration=false;
    osg::ref_ptr<osg::GraphicsContext> context = osg::GraphicsContext::createGraphicsContext(traits);
    require(context.valid(), "real GL context required");
    osgViewer::Viewer viewer; viewer.setThreadingModel(osgViewer::Viewer::SingleThreaded); viewer.setCamera(mainCamera);
    mainCamera->setGraphicsContext(context); mainCamera->setViewport(0,0,128,128); mainCamera->setComputeNearFarMode(osg::CullSettings::DO_NOT_COMPUTE_NEAR_FAR);
    mainCamera->setProjectionMatrixAsPerspective(60.,1.,1.,1000000.);
    const osg::Vec3d center(originalBound.center()); const float radius = originalBound.radius();
    const double farDistance = std::max(double(radius)*3.,double(radius)+selected.node->error()*std::sqrt(3.)*64./.6);
    auto setDistance = [&](double distance) { mainCamera->setViewMatrixAsLookAt(center+osg::Vec3d(0,-distance,0),center,osg::Vec3d(0,0,1)); };
    setDistance(farDistance); mainCamera->setClearColor({0,0,0,0});
    auto* state = mainCamera->getOrCreateStateSet();
    state->addUniform(new osg::Uniform("projectionMatrix",osg::Matrixf(mainCamera->getProjectionMatrix())));
    state->addUniform(new osg::Uniform("screenRes",osg::Vec2(128,128)));
    state->addUniform(new osg::Uniform("near",1.f)); state->addUniform(new osg::Uniform("far",1000000.f));
    state->addUniform(new osg::Uniform("fogColor",osg::Vec4(0,0,0,1))); state->addUniform(new osg::Uniform("fogEnd",1000000.f)); state->addUniform(new osg::Uniform("fogStart",999999.f));
    osg::ref_ptr<Pixels> pixels = new Pixels; mainCamera->setFinalDrawCallback(pixels);
    osg::ref_ptr<osgUtil::IncrementalCompileOperation> ico = new osgUtil::IncrementalCompileOperation;
    osg::ref_ptr<SceneUtil::Light> sun = new SceneUtil::Light;
    sun->setPosition({-.3f,-.7f,.5f,0}); sun->setDiffuse({.7f,.7f,.7f,1});
    sun->setAmbient({.5f,.5f,.5f,1}); sun->setSpecular({.15f,.15f,.15f,1}); lighting->setSunlight(sun);
    lighting->addChild(selected.node);
    viewer.setIncrementalCompileOperation(ico); viewer.setSceneData(lighting); viewer.realize();
    const auto beforeDraw = L1::Counters::derivedDraws.load(), beforeControl = L1::Counters::controlDraws.load(); viewer.frame();
    require(L1::Counters::derivedDraws == beforeDraw && L1::Counters::controlDraws > beforeControl, "pending compile renders original");
    const auto controlPixels = pixels->data;
    Programs programs; selected.node->original()->accept(programs);
    require(!programs.values.empty(), "actual shipped object program required");
    for (auto* program : programs.values)
    {
        require(Shader::IndirectProjection::matches(*program), "exact stock shader producer required");
        auto* contextProgram = program->getPCP(*context->getState());
        require(contextProgram && contextProgram->isLinked(), "actual stock shader must link in fixture context");
    }
    osgUtil::StateToCompile stateToCompile(osgUtil::GLObjectsVisitor::COMPILE_STATE_ATTRIBUTES | osgUtil::GLObjectsVisitor::COMPILE_DISPLAY_LISTS,nullptr);
    selected.node->accept(stateToCompile);
    osg::ref_ptr<osgUtil::IncrementalCompileOperation::CompileSet> compile = new osgUtil::IncrementalCompileOperation::CompileSet(selected.node);
    osgUtil::IncrementalCompileOperation::ContextSet contexts{context.get()};
    compile->buildCompileMap(contexts,stateToCompile);
    compile->_compileCompletedCallback = new L1::CompileCompleted(selected.readiness); ico->add(compile,false);
    for (unsigned step=0; step<60 && !selected.readiness->ready; ++step) viewer.frame();
    require(selected.readiness->ready, "actual ICO preparation must complete");
    const auto derivedDraws = L1::Counters::derivedDraws.load(), derivedIndices = L1::Counters::submittedIndices.load();
    viewer.frame(); viewer.frame();
    std::cout << "L1_DRAW_SELECTION_PROOF compilation_ready=" << selected.node->compilationReady()
        << " local_material_guard=" << selected.node->materialGuardCurrent()
        << " coarse_views=" << L1::Counters::coarseViews.load()
        << " original_views=" << L1::Counters::originalViews.load()
        << " material_cull_fallbacks=" << L1::Counters::materialCullFallbacks.load()
        << " material_draw_fallbacks=" << L1::Counters::materialDrawFallbacks.load()
        << " derived_draws=" << L1::Counters::derivedDraws.load() << '\n';
    require(L1::Counters::derivedDraws > derivedDraws && L1::Counters::submittedIndices > derivedIndices, "main camera actually consumes reduced index tier");
    const auto farPixels = pixels->data;
    const auto occupied = [](const auto& image, int x, int y) {
        if (x < 0 || y < 0 || x >= 128 || y >= 128) return false;
        const auto offset = (y*128+x)*4;
        return image[offset] > 4 || image[offset+1] > 4 || image[offset+2] > 4;
    };
    unsigned controlRgb = 0, tierRgb = 0, silhouetteOutside = 0, interior = 0;
    std::uint64_t rgbError = 0;
    for (int y=0; y<128; ++y) for (int x=0; x<128; ++x)
    {
        const bool a = occupied(controlPixels,x,y), b = occupied(farPixels,x,y);
        controlRgb += a; tierRgb += b;
        bool controlNeighbour = false, tierNeighbour = false, solid = a && b;
        for (int dy=-1; dy<=1; ++dy) for (int dx=-1; dx<=1; ++dx)
        {
            controlNeighbour |= occupied(controlPixels,x+dx,y+dy); tierNeighbour |= occupied(farPixels,x+dx,y+dy);
            solid &= occupied(controlPixels,x+dx,y+dy) && occupied(farPixels,x+dx,y+dy);
        }
        silhouetteOutside += (a && !tierNeighbour) || (b && !controlNeighbour);
        if (solid)
        {
            ++interior;
            for (unsigned channel=0; channel<3; ++channel)
                rgbError += std::abs(int(controlPixels[(y*128+x)*4+channel])-int(farPixels[(y*128+x)*4+channel]));
        }
    }
    require(controlRgb > 16 && tierRgb > 16 && interior > 8, "RGB-visible stock shading required; alpha-only output fails");
    require(silhouetteOutside <= std::max(1u,(controlRgb+tierRgb)/20), "far silhouette must remain within one pixel with bounded outliers");
    require(rgbError <= std::uint64_t(interior)*3*20, "bounded far-view interior RGB error");
    osg::ref_ptr<L1::Group> populationSelector = new L1::Group(*selected.node->original(),*selected.node->derived(),
        selected.node->error(),selected.readiness,mainCamera,true);
    lighting->removeChild(selected.node); lighting->addChild(populationSelector);
    const auto visiblePopulationDraws = L1::Counters::derivedDraws.load(); viewer.frame();
    if (L1::Counters::derivedDraws == visiblePopulationDraws)
    { Topology topology; selected.node->original()->accept(topology); }
    require(L1::Counters::derivedDraws > visiblePopulationDraws,
        "HLOD population qualification admits the current complete visible control");
    const auto savedMask = originals.values.front()->getNodeMask(); originals.values.front()->setNodeMask(0);
    const auto hiddenPopulationDraws = L1::Counters::derivedDraws.load(); viewer.frame();
    require(L1::Counters::derivedDraws == hiddenPopulationDraws,
        "HLOD population qualification preserves current control member mask rejection");
    originals.values.front()->setNodeMask(savedMask);
    lighting->removeChild(populationSelector); lighting->addChild(selected.node);
    const auto secondaryStart = L1::Counters::derivedDraws.load();
    // A different camera identity is always original, including reflections.
    osg::ref_ptr<osg::Camera> secondary = new osg::Camera(*mainCamera,osg::CopyOp::SHALLOW_COPY);
    viewer.setCamera(secondary); viewer.frame();
    require(L1::Counters::derivedDraws == secondaryStart, "secondary camera preserves original");
    viewer.setCamera(mainCamera);
    if (selected.node->error() > 0)
    {
        setDistance(radius + std::max(.01,double(selected.node->error())*std::sqrt(3.)*64./2.));
        const auto closeStart = L1::Counters::derivedDraws.load(); viewer.frame();
        require(L1::Counters::derivedDraws == closeStart, "current close view immediately refines");
    }
    selected.node->releaseGLObjects(context->getState());
    require(!selected.readiness->ready, "context release invalidates prepared tier");
    require(pixels->error == GL_NO_ERROR, "real LOD GL validation");
    std::cout << "L1_REAL_CONSUMPTION asset=" << selectedName << " original_indices=" << selected.originalIndices
        << " tier_indices=" << selected.derivedIndices << " consumed_draws=" << L1::Counters::derivedDraws-derivedDraws
        << " consumed_indices=" << L1::Counters::submittedIndices-derivedIndices << " error=" << selected.node->error()
        << " control_rgb_pixels=" << controlRgb << " tier_rgb_pixels=" << tierRgb << " interior_samples=" << interior
        << " silhouette_outside_1px=" << silhouetteOutside << " mean_rgb_error=" << double(rgbError)/(interior*3) << '\n';
    return 0;
}
catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
