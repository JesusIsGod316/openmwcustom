#include <components/render/backend/vsg/immediateeffectrealizer.hpp>
#include <components/render/backend/vsg/immediateeffectcontract.hpp>
#include <array>
#include <chrono>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace RenderCore;
using Streams = std::vector<RenderVsg::StaticRealizationResult::MutableDrawStreams>;
void require(bool value, const char* why) { if (!value) throw std::runtime_error(why); }
ImmediateEffectDraw drawFixture(std::size_t vertices = 3)
{
    ImmediateEffectDraw draw;
    draw.identity = "fixture";
    draw.mesh.positions.resize(vertices);
    draw.mesh.normals.assign(vertices, {0,0,1});
    draw.mesh.colors.assign(vertices, glm::vec4(1));
    draw.mesh.texCoordSets = {std::vector<glm::vec2>(vertices), {}};
    for (std::size_t i=0; i<vertices; ++i)
        draw.mesh.positions[i] = {float(i%17), float(i%31), float(i%7)};
    draw.mesh.indices = {0,1,2};
    draw.mesh.surfaces = {{PrimitiveTopology::Triangles,0,3,0}};
    draw.bounds.maximum = {17,31,7};
    return draw;
}
Streams streamsFor(const ImmediateEffectDraw& draw)
{
    Streams result(draw.mesh.surfaces.size());
    for (auto& stream : result)
    {
        stream.positions = vsg::vec3Array::create(draw.mesh.positions.size());
        stream.normals = vsg::vec3Array::create(draw.mesh.positions.size());
        stream.colors = vsg::vec4Array::create(draw.mesh.positions.size());
        for (const auto& uv : draw.mesh.texCoordSets)
            stream.texCoords.push_back(vsg::vec2Array::create(uv.size()));
        stream.sorted = vsg::DepthSorted::create();
    }
    return result;
}
void equal(const Streams& left, const Streams& right)
{
    require(left.size() == right.size(), "surface count");
    for (std::size_t s=0;s<left.size();++s)
    {
        const auto& a=left[s]; const auto& b=right[s];
        require(bool(a.sorted) == bool(b.sorted), "sort owner differs");
        if (a.sorted)
            require(a.sorted->bound.center == b.sorted->bound.center
                && a.sorted->bound.radius == b.sorted->bound.radius, "sort bounds differ");
        for (std::size_t v=0;v<a.positions->size();++v)
            require((*a.positions)[v]==(*b.positions)[v] && (*a.normals)[v]==(*b.normals)[v]
                && (*a.colors)[v]==(*b.colors)[v], "vertex stream differs");
        for (std::size_t uv=0;uv<a.texCoords.size();++uv)
            for (std::size_t v=0;v<a.texCoords[uv]->size();++v)
                require((*a.texCoords[uv])[v]==(*b.texCoords[uv])[v], "UV stream differs");
    }
}
auto versions(const Streams& streams)
{
    std::array<vsg::ModifiedCount,4> result;
    streams[0].positions->getModifiedCount(result[0]);
    streams[0].normals->getModifiedCount(result[1]);
    streams[0].colors->getModifiedCount(result[2]);
    streams[0].texCoords[0]->getModifiedCount(result[3]);
    return result;
}
int main()
{
    try
    {
        {
            auto resident=drawFixture();
            resident.meshSnapshot=std::make_shared<const FrozenEffectMesh>(resident.mesh);
            resident.mesh={};
            auto current=resident;
            require(RenderVsg::immediateEffectLayoutMatches(resident,current),"identical frozen owner mismatched");
            current.material.alpha=.5f;
            require(RenderVsg::immediateEffectLayoutMismatch(resident,current)==RenderVsg::ImmediateEffectMismatch::Material,
                "frozen mesh hid material mutation");
            current=resident;current.textures.push_back({});
            require(RenderVsg::immediateEffectLayoutMismatch(resident,current)==RenderVsg::ImmediateEffectMismatch::TextureCount,
                "frozen mesh hid texture mutation");
            current=resident;current.mesh=resident.meshData();current.meshSnapshot.reset();current.mesh.indices[0]=1;
            require(RenderVsg::immediateEffectLayoutMismatch(resident,current)==RenderVsg::ImmediateEffectMismatch::Indices,
                "different mesh owner skipped exact topology checks");
        }
        for (bool billboard : {false,true})
        for (bool validateOnce : {false,true})
        for (bool bulkEnabled : {false,true})
        for (bool sorted : {false,true})
        {
            auto draw=drawFixture();
            draw.mesh.surfaces.push_back(draw.mesh.surfaces[0]);
            if (billboard) draw.billboard=ModelBillboardMode::AlwaysFaceCamera;
            auto scalar=streamsFor(draw), bulk=streamsFor(draw);
            if (!sorted)
                for (auto* group : {&scalar, &bulk})
                    for (auto& stream : *group) stream.sorted={};
            for (unsigned frame=0; frame<120; ++frame)
            {
                draw.mesh.positions[0].x=float(frame)-60;
                draw.mesh.normals[1].y=float(frame)/120;
                draw.mesh.colors[2].w=float(frame)/120;
                draw.mesh.texCoordSets[0][0].x=float(frame);
                const OwnedImmediateEffects owner(std::span(&draw,1));
                require(RenderVsg::updateImmediateEffectRealization(draw,scalar), "scalar update");
                require(validateOnce ? RenderVsg::updateImmediateEffectRealization(owner,0,bulk,bulkEnabled)
                    : RenderVsg::updateImmediateEffectRealization(draw,bulk,bulkEnabled), "candidate update");
                equal(scalar,bulk);
                const auto before=versions(bulk);
                require(RenderVsg::updateImmediateEffectRealization(owner,0,bulk,true), "unchanged update");
                require(before==versions(bulk), "unchanged data marked dirty");
                require(!RenderVsg::updateImmediateEffectRealization(owner,1,bulk,true), "bad owner index accepted");
            }
            draw.mesh.normals.clear(); draw.mesh.colors.clear();
            require(RenderVsg::updateImmediateEffectRealization(draw,scalar), "scalar defaults");
            require(RenderVsg::updateImmediateEffectRealization(draw,bulk,true), "bulk defaults");
            equal(scalar,bulk);
            draw.mesh.texCoordSets[0][0].x=std::numeric_limits<float>::quiet_NaN();
            const OwnedImmediateEffects invalid(std::span(&draw,1));
            require(!RenderVsg::updateImmediateEffectRealization(invalid,0,bulk,true), "invalid owner accepted");
            require(!RenderVsg::updateImmediateEffectRealization(draw,bulk,true), "NaN accepted");
            draw.mesh.texCoordSets[0][0].x=0;
            const OwnedImmediateEffects valid(std::span(&draw,1));
            draw.mesh.positions[0].x=999; // must not change the immutable owner
            bulk[1].texCoords[0]={};
            const auto position=(*bulk[0].positions)[0];
            require(!RenderVsg::updateImmediateEffectRealization(valid,0,bulk,true), "bad later destination accepted");
            require((*bulk[0].positions)[0]==position, "partial destination mutation");
        }
        {
            // Aligned or padded source vectors must use component conversion.
            struct alignas(16) Padded { float x, y, z, padding; };
            std::vector<Padded> source{{1,2,3,999}, {4,5,6,999}};
            auto destination=vsg::vec3Array::create(2);
            RenderVsg::effect_update_detail::updatePackedStream(source,*destination,vsg::vec3(0,0,1));
            require((*destination)[0]==vsg::vec3(1,2,3) && (*destination)[1]==vsg::vec3(4,5,6),
                "padded source layout was copied as packed");
            vsg::ModifiedCount before, after;
            destination->getModifiedCount(before);
            source[0].padding=-999;
            RenderVsg::effect_update_detail::updatePackedStream(source,*destination,vsg::vec3(0,0,1));
            destination->getModifiedCount(after);
            require(before==after, "padding marked data dirty");
        }
        // Observational microbenchmark only: same workload, no FPS claim.
        auto draw=drawFixture(300000);
        const OwnedImmediateEffects owner(std::span(&draw,1));
        for (bool sorted : {false,true})
        for (bool bulk : {false,true})
        for (bool validated : {false,true})
        {
            auto streams=streamsFor(draw);
            if (!sorted) streams[0].sorted={};
            require(RenderVsg::updateImmediateEffectRealization(draw,streams), "benchmark seed");
            const auto begin=std::chrono::steady_clock::now();
            for (unsigned i=0;i<40;++i)
                require(validated ? RenderVsg::updateImmediateEffectRealization(owner,0,streams,bulk)
                    : RenderVsg::updateImmediateEffectRealization(draw,streams,bulk), "benchmark update");
            std::cout<<"UPDATE sorted="<<sorted<<" bulk="<<bulk<<" validated="<<validated<<" ms="
                <<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count()<<'\n';
        }
        std::cout<<"PASS scalar/bulk/owned parity, animation, billboards, defaults, invalid input, atomic rejection\n";
    }
    catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
