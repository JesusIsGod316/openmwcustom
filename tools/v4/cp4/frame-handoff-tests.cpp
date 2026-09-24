#include <components/rendercore/frameproducer.hpp>
#include <components/rendercore/renderer.hpp>
#include <iostream>
#include <limits>
#include <chrono>
#include <stdexcept>
#include <type_traits>
#include <atomic>
#include <array>
#include <barrier>

using namespace RenderCore;
static_assert(!std::is_copy_assignable_v<OwnedImmediateEffects>);
static_assert(!std::is_move_assignable_v<OwnedImmediateEffects>);
void require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
ImmediateEffectDraw fixture()
{
    ImmediateEffectDraw d; d.identity = "owned";
    d.mesh.positions = {{0,0,0},{1,0,0},{0,1,0}};
    d.mesh.indices = {0,1,2}; d.mesh.surfaces = {{PrimitiveTopology::Triangles,0,3,0}};
    d.bounds.minimum = {0,0,0}; d.bounds.maximum = {1,1,0};
    return d;
}
int main()
{
    try
    {
        std::vector<ImmediateEffectDraw> source{fixture()};
        {
            auto original = fixture();
            original.meshSnapshot = std::make_shared<const FrozenEffectMesh>(original.mesh);
            original.mesh = {};
            const OwnedImmediateEffects first(std::span(&original, 1));
            original.meshSnapshot.reset();
            require(first.valid() && first.draws()[0].meshData().positions.size() == 3,
                "frozen mesh owner did not survive producer release");
            auto replacement = fixture();
            replacement.meshSnapshot = first.draws()[0].meshSnapshot;
            const OwnedImmediateEffects second(std::span(&replacement, 1));
            require(first.draws()[0].meshSnapshot == second.draws()[0].meshSnapshot,
                "immutable geometry copied at publication");
        }
        auto pixels = std::make_shared<TexturePixels>(); pixels->rgba8 = {1,2,3,4};
        EffectTextureSnapshot texture; texture.texture.sourceIdentity = "pixels";
        texture.texture.contentIdentity = "fixture"; texture.texture.width = texture.texture.height = 1;
        texture.texture.pixels = pixels; source[0].textures = {texture};
        auto owned = std::make_shared<const OwnedImmediateEffects>(source);
        require(owned->valid(), "valid ownership fixture rejected");
        source[0].mesh.positions[0].x = std::numeric_limits<float>::quiet_NaN();
        source[0].identity = "mutated"; pixels->rgba8[0] = 255;
        require(owned->draws()[0].identity == "owned" && owned->draws()[0].mesh.positions[0].x == 0
            && owned->draws()[0].textures[0].texture.pixels->rgba8[0] == 1 && owned->valid(),
            "mutable producer alias escaped publication");
        require(!OwnedImmediateEffects(source).valid(), "nonfinite mesh accepted");
        source = {fixture(),fixture()};
        require(!OwnedImmediateEffects(source).valid(), "duplicate identities accepted");
        source[1].identity = "second";
        require(OwnedImmediateEffects(source).valid(), "distinct identities rejected");
        BoundedParallelFor workers(3);
        require(workers.workersFor(127)==0 && workers.workersFor(128)==3, "bounded worker threshold");
        {
            BoundedParallelFor serial(0);
            std::size_t visits=0;
            serial.forEach(256,[&](std::size_t) { ++visits; });
            require(visits==256, "zero-worker inline fallback");
        }
        // Force all four participants (three workers and caller) to meet.
        std::barrier meeting(4);
        std::atomic_uint arrivals{0};
        workers.forEach(128, [&](std::size_t) {
            if (arrivals.fetch_add(1) < 4) meeting.arrive_and_wait();
        });
        require(arrivals == 128, "parallel work lost or duplicated indices");
        for (unsigned repeat=0;repeat<40;++repeat)
        {
            std::array<std::atomic_uint,257> visits{};
            workers.forEach(visits.size(), [&](std::size_t i) { ++visits[i]; });
            for (const auto& visitsForIndex : visits)
                require(visitsForIndex == 1, "repeated submission lost or duplicated work");
        }
        bool threw = false;
        try { workers.forEach(256, [](std::size_t i) { if (i == 3) throw std::runtime_error("fixture"); }); }
        catch (const std::runtime_error&) { threw = true; }
        require(threw, "worker/caller exception was swallowed");
        workers.forEach(256, [](std::size_t) {}); // worker pool survives rejected publication
        source.assign(2000,fixture());
        for (std::size_t i=0;i<source.size();++i)
        {
            source[i].identity=std::to_string(i);
            source[i].mesh.positions.resize(256, glm::vec3(0.5f));
            source[i].mesh.normals.assign(256, glm::vec3(0,0,1));
            source[i].mesh.colors.assign(256, glm::vec4(1));
            source[i].mesh.texCoordSets={std::vector<glm::vec2>(256,glm::vec2(0))};
        }
        for (auto* pool : {static_cast<BoundedParallelFor*>(nullptr), &workers})
        {
            const auto begin=std::chrono::steady_clock::now();
            const OwnedImmediateEffects snapshot(source,pool);
            require(snapshot.valid() && snapshot.draws().size()==source.size(), "parallel publication rejected");
            for (std::size_t i=0;i<source.size();++i)
                require(snapshot.draws()[i].identity==source[i].identity
                    && snapshot.draws()[i].mesh.positions==source[i].mesh.positions, "parallel draw order/content changed");
            std::cout<<"PUBLICATION parallel="<<(pool!=nullptr)<<" bytes="<<snapshot.payloadBytes()<<" ms="
                <<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count()<<'\n';
        }
        source.back().mesh.positions[0].x=std::numeric_limits<float>::quiet_NaN();
        require(!OwnedImmediateEffects(source,&workers).valid(), "parallel malformed payload accepted");
        source.back().mesh.positions[0].x=0;
        source.back().identity=source.front().identity;
        require(!OwnedImmediateEffects(source,&workers).valid(), "parallel duplicate identity accepted");
        source.back().identity="last";
        source.front().textures={texture};
        const OwnedImmediateEffects parallelAlias(source,&workers);
        pixels->rgba8[0]=17;
        require(parallelAlias.draws().front().textures[0].texture.pixels->rgba8[0]!=17,
            "parallel publication retained mutable texture alias");
        // Scaling observations exercise the production constructors/validators;
        // no machine-dependent timing threshold or gameplay FPS claim.
        for (std::size_t count : {256u,1000u,2000u,4000u})
        {
            source.assign(count,fixture());
            for (std::size_t i=0;i<count;++i) source[i].identity=std::to_string(i);
            const auto begin=std::chrono::steady_clock::now();
            auto snapshot=std::make_shared<const OwnedImmediateEffects>(source);
            const auto published=std::chrono::steady_clock::now();
            RenderWorld sampleWorld; SingleViewFrameProducer sampleProducer; SingleViewFrameInput sample;
            sample.renderExtent=sample.outputExtent={1920,1080}; sample.ownedImmediateEffects=snapshot;
            for (int i=0;i<100;++i)
            {
                auto prepared=sampleProducer.prepare(sampleWorld,sample);
                require(prepared && prepared->valid() && frameCompatibleWithWorld(sampleWorld,*prepared)
                    && sampleProducer.commitPresented(*prepared), "scaling production path failed");
            }
            const auto end=std::chrono::steady_clock::now();
            std::cout << "SCALING draws=" << count << " publish_ms="
                << std::chrono::duration<double,std::milli>(published-begin).count() << " handoff100_ms="
                << std::chrono::duration<double,std::milli>(end-published).count() << '\n';
        }
        // Representative exterior size; no all-pairs check or geometry recopy
        // between input routing, frame preparation and history validation.
        source.assign(2005,fixture());
        for (std::size_t i=0;i<source.size();++i) source[i].identity=std::to_string(i);
        owned=std::make_shared<const OwnedImmediateEffects>(source);
        RenderWorld world; SingleViewFrameProducer producer; SingleViewFrameInput input;
        input.renderExtent=input.outputExtent={1920,1080}; input.ownedImmediateEffects=owned;
        auto routed=input; auto frame=producer.prepare(world,routed);
        require(frame && frame->valid() && &frame->immediateEffectDraws()==&owned->draws(),
            "prepare copied or invalidated owned geometry");
        require(frameCompatibleWithWorld(world,*frame) && producer.commitPresented(*frame), "coherence/history rejected");
        require(!producer.commitPresented(*frame), "duplicate presentation advanced history");
        auto skipped=producer.prepare(world,input);
        require(skipped && skipped->views().front().historyValid,"presented history missing");
        auto retry=producer.prepare(world,input);
        require(retry && retry->frameId()==skipped->frameId() && retry->historyEpoch()==skipped->historyEpoch(),
            "unpresented prepare consumed frame/history");
        input.invalidateHistory=true;
        auto discontinuous=producer.prepare(world,input);
        require(discontinuous && !discontinuous->views().front().historyValid
            && discontinuous->historyEpoch()!=retry->historyEpoch(),"history invalidation ignored");
        input.invalidateHistory=false;
        const auto* retained=&owned->draws(); owned.reset(); source.clear();
        require(frame->valid() && &frame->immediateEffectDraws()==retained,"shared snapshot lifetime ended early");
        input.immediateEffectDraws={fixture()};
        require(!producer.prepare(world,input), "ambiguous owned and legacy input accepted");
        input.immediateEffectDraws.clear();
        const auto handle=world.reserveMaterial();
        require(handle && world.commit(*handle,MaterialRecord{}), "world fixture update failed");
        require(!frameCompatibleWithWorld(world,*frame), "cached validation bypassed live world revision");
        std::cout << "PASS owned frame alias safety, malformed/duplicate payloads, scaling, shared lifetime, skipped/retried/history/coherence\n";
        return 0;
    }
    catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
