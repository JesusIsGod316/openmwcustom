#include <components/sceneutil/drawphasetrace.hpp>
#include <osg/Texture2D>
#include <osg/Uniform>
#include <iostream>
#include <stdexcept>
#include <map>
#include <tuple>
#include <thread>

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


// Frozen pre-scratch algorithm: independent map/name/ref_ptr resolution serves
// as an equivalence oracle for mutable, inherited and bounded metadata.
struct ReferenceCatalog
{
    std::vector<Capture::ResourceRow> rows;
    std::uint64_t dropped = 0;
    void noteResourceUncovered() { ++dropped; }
    void catalogResource(const Capture::ResourceRow& row) { rows.push_back(row); }
};
    ReferenceCatalog referenceCatalog(const std::vector<const osg::StateSet*>& stateSets,
        unsigned frame,unsigned context,std::uint64_t camera,unsigned scope,Capture::Row* descriptor=nullptr)
    {
        ReferenceCatalog capture;
        struct Binding { const osg::Texture* texture=nullptr;const osg::StateSet* stateSet=nullptr;unsigned flags=0; };
        std::map<unsigned,Binding> textures;
        std::map<std::string,osg::StateSet::RefUniformPair> uniforms;
        constexpr std::size_t MaxBindings=512;
        for(const auto* stateSet:stateSets)
        {
            if(!stateSet) continue;
            const auto& attributes=stateSet->getTextureAttributeList();
            for(unsigned unit=0;unit<attributes.size();++unit)
            {
                const auto* pair=stateSet->getTextureAttributePair(unit,osg::StateAttribute::TEXTURE);
                const auto* texture=pair && pair->first ? pair->first->asTexture() : nullptr;
                if(!texture) continue;
                auto existing=textures.find(unit);
                if(existing!=textures.end() && (existing->second.flags & osg::StateAttribute::OVERRIDE)
                    && !(pair->second & osg::StateAttribute::PROTECTED)) continue;
                if(existing==textures.end() && textures.size()>=MaxBindings) { capture.noteResourceUncovered();continue; }
                textures[unit]={texture,stateSet,pair->second};
            }
            for(const auto& [name,pair]:stateSet->getUniformList())
            {
                auto existing=uniforms.find(name);
                if(existing!=uniforms.end() && (existing->second.second & osg::StateAttribute::OVERRIDE)
                    && !(pair.second & osg::StateAttribute::PROTECTED)) continue;
                if(existing==uniforms.end() && uniforms.size()>=MaxBindings) { capture.noteResourceUncovered();continue; }
                uniforms[name]=pair;
            }
        }
        for(const auto& [unit,binding]:textures)
        {
            const auto* image=binding.texture->getNumImages()>0 ? binding.texture->getImage(0) : nullptr;
            Capture::ResourceRow row;
            row.firstFrame=row.lastFrame=frame;row.context=context;row.unit=unit;row.scope=scope;
            row.texture=reinterpret_cast<std::uintptr_t>(binding.texture);
            row.image=reinterpret_cast<std::uintptr_t>(image);
            row.imageRevision=image ? image->getModifiedCount() : 0;
            row.bytes=image ? image->getTotalSizeInBytesIncludingMipmaps() : 0;
            row.stateSet=reinterpret_cast<std::uintptr_t>(binding.stateSet);row.submitCamera=camera;
            const std::string& filename=image && !image->getFileName().empty() ? image->getFileName() : binding.texture->getName();
            std::string role;
            for(const auto& [name,pair]:uniforms)
            {
                if(!pair.first || pair.first->getType()!=osg::Uniform::SAMPLER_2D) continue;
                if(pair.first->getNumElements()>512) capture.noteResourceUncovered();
                for(unsigned element=0;element<std::min(512u,pair.first->getNumElements());++element)
                {
                    int boundUnit=-1;
                    if(pair.first->getElement(element,boundUnit) && boundUnit>=0 && static_cast<unsigned>(boundUnit)==unit)
                    {
                        std::string bindingName=name;
                        if(pair.first->getNumElements()>1) bindingName+='['+std::to_string(element)+']';
                        if(role.size()+bindingName.size()+1>=row.semanticRole.size())
                        { capture.noteResourceUncovered();continue; }
                        if(!role.empty()) role+='|';
                        role+=bindingName;
                    }
                }
            }
            if(role.empty() && scope==2 && unit<=1)
                role=unit==0 ? "terrain_composite_diffuse" : "terrain_composite_blendmap";
            if(role.empty()) role="unknown";
            auto copyText=[](auto& output,const char* value) {
                std::memcpy(output.data(),value,std::min(std::strlen(value),output.size()-1));
            };
            copyText(row.textureClass,binding.texture->className());
            copyText(row.filename,filename.c_str());copyText(row.semanticRole,role.c_str());
            if(filename.size()>=row.filename.size()) capture.noteResourceUncovered();
            if(std::strlen(binding.texture->className())>=row.textureClass.size()) capture.noteResourceUncovered();
            if(binding.texture->getNumImages()>1) capture.noteResourceUncovered();
            row.labelHash=1469598103934665603ull;
            for(const auto* value:std::array<const std::string*,2>{&filename,&role})
            {
                row.labelHash^=value->size();row.labelHash*=1099511628211ull;
                for(unsigned char character:*value) { row.labelHash^=character;row.labelHash*=1099511628211ull; }
            }
            for(const char* character=binding.texture->className();*character;++character)
            { row.labelHash^=static_cast<unsigned char>(*character);row.labelHash*=1099511628211ull; }
            capture.catalogResource(row);
            if(descriptor)
            {
                if(row.bytes>=descriptor->texture0Bytes)
                {
                    descriptor->texture1Bytes=descriptor->texture0Bytes;descriptor->texture1=descriptor->texture0;
                    descriptor->texture0Bytes=row.bytes;descriptor->texture0={};
                    copyText(descriptor->texture0,filename.c_str());
                }
                else if(row.bytes>=descriptor->texture1Bytes)
                {
                    descriptor->texture1Bytes=row.bytes;descriptor->texture1={};
                    copyText(descriptor->texture1,filename.c_str());
                }
            }
        }
        return capture;
    }

void verifyEquivalent(const std::vector<const osg::StateSet*>& path, unsigned frame,
    unsigned context, std::uint64_t camera, unsigned scope, CatalogScratch* scratch = nullptr)
{
    auto& capture = Capture::instance();
    Capture::Row expectedDescriptor, actualDescriptor;
    const auto expected = referenceCatalog(path, frame, context, camera, scope, &expectedDescriptor);
    const auto first = capture.resourceCount(), dropped = capture.resourceDropped();
    if (scratch)
    {
        CatalogDiagnostic diagnostic(*scratch);
        catalogStateSetsImpl(path, frame, context, camera, scope, &actualDescriptor, *scratch);
    }
    else catalogStateSets(path, frame, context, camera, scope, &actualDescriptor);
    require(capture.resourceCount() == first + expected.rows.size(), "flat catalog differs from map descriptor count");
    require(capture.resourceDropped() == dropped + expected.dropped, "flat catalog changed explicit metadata loss");
    for (std::size_t i = 0; i < expected.rows.size(); ++i)
    {
        const auto actual = capture.resource(first + i);
        const auto& row = expected.rows[i];
        require(std::tie(actual.firstFrame, actual.lastFrame, actual.context, actual.unit, actual.imageRevision,
            actual.scope, actual.texture, actual.image, actual.stateSet, actual.submitCamera, actual.bytes,
            actual.labelHash, actual.textureClass, actual.filename, actual.semanticRole)
            == std::tie(row.firstFrame, row.lastFrame, row.context, row.unit, row.imageRevision,
            row.scope, row.texture, row.image, row.stateSet, row.submitCamera, row.bytes,
            row.labelHash, row.textureClass, row.filename, row.semanticRole),
            "flat catalog changed inherited metadata, ordering, label hash or tuple identity");
    }
    require(std::tie(actualDescriptor.texture0Bytes, actualDescriptor.texture1Bytes,
        actualDescriptor.texture0, actualDescriptor.texture1) == std::tie(expectedDescriptor.texture0Bytes,
        expectedDescriptor.texture1Bytes, expectedDescriptor.texture0, expectedDescriptor.texture1),
        "flat catalog changed the largest texture metadata");
    if (scratch) require(!scratch->textureCount && !scratch->uniformCount,
        "reused catalog scratch retained an active reference after return");
}

void mutableAndInheritedEquivalence()
{
    osg::ref_ptr<osg::StateSet> root = new osg::StateSet, middle = new osg::StateSet, leaf = new osg::StateSet;
    root->setDataVariance(osg::Object::STATIC);
    middle->setDataVariance(osg::Object::STATIC);
    leaf->setDataVariance(osg::Object::STATIC);
    auto a = texture("mutable/root.dds"), b = texture("mutable/middle.dds"), c = texture("mutable/leaf.dds");
    root->setTextureAttribute(0, a, osg::StateAttribute::OVERRIDE);
    middle->setTextureAttribute(0, b, osg::StateAttribute::PROTECTED);
    leaf->setTextureAttribute(0, c);
    leaf->setTextureAttribute(4, a);
    leaf->setTextureAttribute(600, b); // count bound, not a fabricated unit-number bound
    auto rootSampler = sampler("diffuseMap", 0), middleSampler = sampler("diffuseMap", 4);
    root->addUniform(rootSampler, osg::StateAttribute::OVERRIDE);
    middle->addUniform(middleSampler, osg::StateAttribute::PROTECTED);
    osg::ref_ptr<osg::Uniform> array = new osg::Uniform(osg::Uniform::SAMPLER_2D, "arraySampler", 3);
    array->setElement(0, 0); array->setElement(1, 4); array->setElement(2, 600);
    leaf->addUniform(array);
    leaf->addUniform(new osg::Uniform("ordinaryInteger", 0));
    osg::ref_ptr<osg::Uniform> cube = new osg::Uniform(osg::Uniform::SAMPLER_CUBE, "unknownSampler");
    cube->set(0); leaf->addUniform(cube);
    CatalogScratch scratch;
    std::uint64_t camera = 10000;
    auto check = [&] { verifyEquivalent({root, middle, leaf}, 30, 7, ++camera, 1, &scratch); };
    check();
    // Same objects and STATIC hints do not establish immutable values.
    middleSampler->set(0); array->setElement(1, 0); check();
    a->getImage()->setFileName("mutable/renamed.dds"); check();
    a->getImage()->dirty(); check();
    a->getImage()->allocateImage(4, 8, 1, GL_RGB, GL_UNSIGNED_BYTE); check();
    osg::ref_ptr<osg::Image> replacement = new osg::Image;
    replacement->allocateImage(3, 5, 1, GL_RGBA, GL_UNSIGNED_BYTE);
    replacement->setFileName("mutable/replaced.dds"); a->setImage(replacement); check();
    middle->setTextureAttribute(0, b, osg::StateAttribute::PROTECTED | osg::StateAttribute::OVERRIDE); check();
    leaf->setTextureAttribute(0, c, osg::StateAttribute::PROTECTED); check();
    leaf->addUniform(sampler("diffuseMap", 600), osg::StateAttribute::PROTECTED); check();
    // Effective non-sampler replacement must remove a former sampler label.
    leaf->addUniform(new osg::Uniform("diffuseMap", 0), osg::StateAttribute::PROTECTED); check();
    verifyEquivalent({root, middle, leaf}, 31, 8, ++camera, 1, &scratch);
    verifyEquivalent({root, middle, leaf}, 32, ~0u, 0, 2, &scratch);
    verifyEquivalent({leaf, root, middle}, 33, 8, ++camera, 1, &scratch);
    // Scratch reuse after a smaller/empty path must not expose old bindings.
    verifyEquivalent({}, 34, 8, ++camera, 1, &scratch);
    verifyEquivalent({root}, 35, 8, ++camera, 1, &scratch);
    leaf->addUniform(sampler(std::string(127, 'x').c_str(), 0));
    check();
    osg::ref_ptr<osg::Uniform> oversized = new osg::Uniform(osg::Uniform::SAMPLER_2D, "oversized", 513);
    for (unsigned i = 0; i < 513; ++i) oversized->setElement(i, -1);
    leaf->addUniform(oversized); check();
    // Both flat limits preserve existing bindings when a deeper layer updates
    // them, while the same excess distinct names/units remain explicit loss.
    osg::ref_ptr<osg::StateSet> wide = new osg::StateSet, wideChild = new osg::StateSet;
    for (unsigned i = 0; i < 513; ++i)
    {
        wide->setTextureAttribute(i, a);
        const std::string name = "integer" + std::to_string(i);
        wide->addUniform(new osg::Uniform(name.c_str(), 0));
    }
    wideChild->setTextureAttribute(0, b, osg::StateAttribute::PROTECTED);
    wideChild->setTextureAttribute(600, b);
    wideChild->addUniform(sampler("integer0", 0));
    wideChild->addUniform(sampler("newOverLimit", 0));
    verifyEquivalent({wide, wideChild}, 38, 8, ++camera, 1, &scratch);
    // Cull owners use independent scratch, with stable source objects while
    // parallel callers deduplicate against the shared, mutex-protected catalog.
    constexpr unsigned workers = 8, observations = 20;
    const auto beforeParallel = Capture::instance().resourceCount();
    std::vector<std::thread> threads;
    for (unsigned worker = 0; worker < workers; ++worker)
        threads.emplace_back([&, worker] {
            CatalogScratch ownScratch;
            for (unsigned observation = 0; observation < observations; ++observation)
            {
                CatalogDiagnostic diagnostic(ownScratch);
                catalogStateSetsImpl(std::array<const osg::StateSet*, 1>{root}, 40 + observation,
                    100 + worker, 20000 + worker, 1, nullptr, ownScratch);
            }
        });
    for (auto& thread : threads) thread.join();
    require(Capture::instance().resourceCount() == beforeParallel + workers,
        "parallel independent scratch corrupted shared catalog tuple deduplication");
    for (std::size_t i = beforeParallel; i < Capture::instance().resourceCount(); ++i)
    {
        const auto row = Capture::instance().resource(i);
        require(row.context >= 100 && row.context < 100 + workers
            && row.submitCamera == 20000 + row.context - 100 && row.firstFrame == 40
            && row.lastFrame == 40 + observations - 1,
            "parallel cull identities or complete observation extents were lost");
    }
    // Nested callers own distinct storage; no shared TLS can corrupt outer data.
    CatalogScratch outer, inner;
    {
        CatalogDiagnostic outerDiagnostic(outer);
        catalogStateSetsImpl(std::array<const osg::StateSet*, 1>{root}, 36, 8, ++camera, 1, nullptr, outer);
        const auto outerCount = outer.textureCount;
        verifyEquivalent({leaf}, 37, 8, ++camera, 1, &inner);
        require(outer.textureCount == outerCount && outer.textures[0].texture == a.get(),
            "nested catalog call corrupted another caller's scratch references");
    }
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
    // Size changes invalidate metadata even if a producer did not dirty its
    // image. Camera and effective StateSet identities must remain truthful
    // joins instead of being collapsed to avoid the old catalog bound.
    auto changed=capture.resource(5);
    changed.bytes+=16;changed.firstFrame=changed.lastFrame=20;
    capture.catalogResource(changed);
    require(capture.resourceCount()==8 && capture.resource(7).bytes==changed.bytes,
        "resource size change retained stale descriptor metadata");
    changed.submitCamera=777;capture.catalogResource(changed);
    changed.stateSet=888;capture.catalogResource(changed);
    require(capture.resourceCount()==10,"camera or effective material identity was erased by deduplication");
    const auto callsBefore = capture.catalogCalls();
    mutableAndInheritedEquivalence();
    require(capture.catalogCalls() > callsBefore, "short catalog calls disappeared from aggregate self-cost");
    const auto initial=capture.resourceCount();
    const auto initialDrops=capture.resourceDropped();
    require(capture.resourceCapacity()>=Capture::ResourceMinimumCapacity
        && capture.resourceCapacity()<=Capture::ResourceMaximumCapacity,"resource override escaped the hard memory bound");
    for(std::size_t i=0;i<capture.resourceCapacity();++i)
    {
        Capture::ResourceRow row;
        row.texture=100000+i;row.image=200000+i;row.stateSet=300000+i;
        row.context=7;row.unit=2;row.scope=1;
        capture.catalogResource(row);
    }
    require(capture.resourceCount()==capture.resourceCapacity() && capture.resourceDropped()==initialDrops+initial,
        "resource catalogue capacity or explicit drop accounting changed");
    auto repeated=capture.resource(0);repeated.lastFrame=100;
    capture.catalogResource(repeated);
    require(capture.resourceDropped()==initialDrops+initial && capture.resource(0).lastFrame==100,
        "a full catalog dropped repeat observations of a known descriptor");
    std::cout<<"PASS: frozen-map equivalence through live STATIC uniform/texture/image mutations, nested scratch isolation, effective inherited texture/sampler OVERRIDE and PROTECTED, truthful pointer/context/unit/revision/byte joins, static dedup, producer role/unknown context, bounded capacity="
        <<capture.resourceCapacity()<<", descriptors_bytes="<<capture.resourceCapacity()*sizeof(Capture::ResourceRow)
        <<", explicit overflow and dedup after full\n";
}
catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
