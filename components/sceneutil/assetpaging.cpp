#include "assetpaging.hpp"
#include <components/resource/assetgpubudget.hpp>
#include <components/resource/derivedassetcache.hpp>
#include <components/vfs/manager.hpp>
#include <osg/Geometry>
#include <algorithm>
#include <chrono>
#include <stdexcept>

namespace SceneUtil::AssetPaging
{
    std::uint64_t milliseconds()
    { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
    bool Quality::satisfies(unsigned requested, std::uint64_t now) const
    {
        for (unsigned i = 0; i < 2; ++i)
        {
            const unsigned bit = 1u << i;
            if (!(requested & bit) || (settled() & bit)) continue;
            if (!((transient | completed) & bit) || now >= retryAfter[i]) return false;
        }
        return true;
    }
    Quality Quality::after(const Outcome& outcome, std::uint64_t now) const
    {
        Quality result = *this;
        result.attempted |= outcome.attempted;
        result.completed = (result.completed & ~outcome.attempted) | outcome.completed;
        result.terminal = (result.terminal & ~outcome.attempted) | outcome.terminal;
        result.transient = (result.transient & ~outcome.attempted) | outcome.transient;
        if (outcome.completed) result.readiness = outcome.readiness;
        for (unsigned i = 0; i < 2; ++i)
        {
            const unsigned bit = 1u << i;
            if (!(outcome.attempted & bit)) continue;
            if ((transient | completed) & bit) ++Counters::retryAttempts;
            result.attempts[i] = static_cast<unsigned char>(std::min<unsigned>(MaximumAttempts, result.attempts[i] + 1));
            if (!((outcome.transient | outcome.completed) & bit)) continue;
            if (result.attempts[i] >= MaximumAttempts)
            { result.exhausted |= bit; ++Counters::retryExhausted; }
            else result.retryAfter[i] = now + RetryDelayMilliseconds;
        }
        return result;
    }
    Quality Quality::withoutOwner() const
    {
        Quality result = *this;
        const unsigned successful = readiness && readiness->everReady.load(std::memory_order_acquire)
            ? completed & ~transient : 0;
        // Failed/expired preparation keeps its finite generation retry ledger.
        // A completed GL owner may be cooked afresh after ordinary cache eviction.
        result.transient |= completed & ~successful;
        result.completed = 0;
        result.readiness.reset();
        result.exhausted &= ~successful;
        for (unsigned i = 0; i < 2; ++i)
            if (successful & (1u << i)) { result.attempts[i] = 0; result.retryAfter[i] = 0; }
        return result;
    }
    void refuse(unsigned lanes, Gate gate)
    { each(lanes, [&](LaneCounters& value) { ++value.refusals[static_cast<unsigned>(gate)]; }); }
    namespace
    {
        std::pair<std::size_t,std::size_t> sourceBytes(const osg::Node& root)
        {
            std::size_t nodes = 0, geometries = 0, bytes = 0, graphBytes = 0;
            constexpr std::size_t limit = 32 * Resource::SpeculativeBudget::MiB;
            const auto count = [&](const auto& self, const osg::Node& node, unsigned depth) -> bool {
                if (++nodes > 1024 || depth > 32) return false;
                // Deep paging clones retain descriptions and object metadata until
                // CopyOp clears them. Account that overlap and conservative OSG
                // node/geometry/buffer allocations separately from GL streams.
                graphBytes += 4096 + node.getName().size() * 2;
                for (const auto& description : node.getDescriptions()) graphBytes += 32 + description.size() * 2;
                if (graphBytes > limit) return false;
                const auto add = [&](std::size_t value) { if (value > limit - bytes) return false; bytes += value; return true; };
                if (const auto* geometry = dynamic_cast<const osg::Geometry*>(&node))
                {
                    if (++geometries > 64 || !geometry->getVertexArray() || !add(geometry->getVertexArray()->getTotalDataSize())) return false;
                    for (const auto* array : {geometry->getNormalArray(), geometry->getColorArray(), geometry->getSecondaryColorArray(), geometry->getFogCoordArray()})
                        if (array && !add(array->getTotalDataSize())) return false;
                    for (unsigned i = 0; i < geometry->getNumTexCoordArrays(); ++i)
                        if (const auto* array = geometry->getTexCoordArray(i); array && !add(array->getTotalDataSize())) return false;
                    for (unsigned i = 0; i < geometry->getNumVertexAttribArrays(); ++i)
                        if (const auto* array = geometry->getVertexAttribArray(i); array && !add(array->getTotalDataSize())) return false;
                    for (unsigned i = 0; i < geometry->getNumPrimitiveSets(); ++i)
                        if (!add(geometry->getPrimitiveSet(i)->getNumIndices() * sizeof(unsigned))) return false;
                }
                if (const auto* group = node.asGroup())
                    for (unsigned i = 0; i < group->getNumChildren(); ++i) if (!self(self, *group->getChild(i), depth + 1)) return false;
                return true;
            };
            return count(count, root, 0) && geometries && bytes <= limit - graphBytes
                ? std::pair{bytes,bytes + graphBytes} : std::pair<std::size_t,std::size_t>{};
        }
        Gate lodGate(GeneratedStaticLod::Refusal value)
        {
            switch (value)
            {
                case GeneratedStaticLod::Refusal::Graph: return Gate::Graph;
                case GeneratedStaticLod::Refusal::Material: return Gate::Material;
                case GeneratedStaticLod::Refusal::Transform: return Gate::Transform;
                case GeneratedStaticLod::Refusal::Layout: return Gate::Layout;
                case GeneratedStaticLod::Refusal::NoReduction: return Gate::NoReduction;
                default: return Gate::Capacity;
            }
        }
        Gate hlodGate(const HlodBuildResult& value)
        {
            if (value.retryable) return Gate::Capacity;
            const std::string_view reason = value.rejection;
            if (reason == "no residual differing-texture submissions beyond optimized control"
                || reason == "no actual submission reduction") return Gate::NoReduction;
            if (reason == "source ancestry certificate no longer valid") return Gate::Graph;
            if (reason == "cluster is not spatially bounded" || reason == "nonuniform, mirrored, singular or custom transform") return Gate::Transform;
            if (reason == "source geometry admission limit" || reason == "vertex admission limit"
                || reason == "host source/scratch/output admission limit") return Gate::Capacity;
            if (reason == "unsupported position/normal/UV/index semantics" || reason == "dynamic source streams"
                || reason == "custom vertex effects" || reason == "nonfinite vertex data" || reason == "out of range source index") return Gate::Layout;
            if (reason.find("snapshot admission") != std::string_view::npos || reason.find("decoder") != std::string_view::npos
                || reason.find("compressed") != std::string_view::npos || reason.find("diffuse blocks") != std::string_view::npos)
                return Gate::Source;
            return Gate::Material;
        }
    }
    std::optional<Source> prepareSource(const osg::Node& original, VFS::Path::NormalizedView model,
        const VFS::Manager& vfs, unsigned requested, Outcome& outcome)
    {
        requested &= Mask;
        each(requested, [](LaneCounters& value) { ++value.sourceRequested; });
        unsigned eligible = 0;
        if (requested & Lod)
        {
            const auto certificate = GeneratedStaticLod::certify(original);
            if (certificate == GeneratedStaticLod::Refusal::None) eligible |= Lod;
            else refuse(Lod, lodGate(certificate));
        }
        if (requested & Hlod)
        {
            if (!certifySelectiveHlodTemplate(original)) refuse(Hlod, Gate::Graph);
            else if (qualifySelectiveHlodTemplate(original)) eligible |= Hlod;
            else refuse(Hlod, Gate::Material);
        }
        each(eligible, [](LaneCounters& value) { ++value.sourceCertified; });
        each(requested & ~eligible, [](LaneCounters& value) { ++value.sourceRejected; });
        if (!eligible) return {};
        try
        {
            if (!PagingWorkScope::optionalOptimization() || !Resource::SpeculativeScope::active())
                throw Resource::SpeculativeDeferred{};
            const auto estimate = vfs.getOpenEstimate(model);
            constexpr auto limit = 8 * Resource::SpeculativeBudget::MiB;
            if (model.value().size() > 4096 || !estimate || !estimate->sourceBytes || estimate->sourceBytes > limit || estimate->openPeakBytes > limit)
                throw std::runtime_error("asset source provider lacks a bounded snapshot contract");
            Resource::SpeculativeScope::Stage snapshot(estimate->sourceBytes + estimate->openPeakBytes, 0);
            auto stream = vfs.get(model);
            Resource::DerivedAssets::Bytes bytes(static_cast<std::size_t>(estimate->sourceBytes));
            stream->read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            PagingWorkScope::checkpoint();
            if (stream->gcount() != static_cast<std::streamsize>(bytes.size()) || stream->bad() || stream->peek() != std::char_traits<char>::eof())
                throw std::runtime_error("asset source snapshot differs from provider metadata");
            const auto [streams,hostBytes] = sourceBytes(original);
            if (!streams) throw std::runtime_error("asset source streams exceed the bounded private population");
            return Source{std::string(model.value()) + "|" + Resource::DerivedAssets::digest(bytes), streams, hostBytes, eligible};
        }
        catch (const PagingWorkCancelled&) { throw; }
        catch (const Resource::SpeculativeDeferred&)
        { outcome.defer(eligible); each(eligible, [](LaneCounters& value) { ++value.snapshotDenied; }); refuse(eligible, Gate::Capacity); }
        catch (...)
        { each(eligible, [](LaneCounters& value) { ++value.snapshotDenied; }); refuse(eligible, Gate::Source); }
        return {};
    }
    std::unique_ptr<Resource::SpeculativeScope::Stage> reservePrivateSources(unsigned eligible, Outcome& outcome)
    {
        try { return std::make_unique<Resource::SpeculativeScope::Stage>(32 * Resource::SpeculativeBudget::MiB, 32 * Resource::SpeculativeBudget::MiB); }
        catch (const Resource::SpeculativeDeferred&)
        { outcome.defer(eligible); each(eligible, [](LaneCounters& value) { ++value.privateCpuDenied; }); refuse(eligible, Gate::Capacity); }
        return {};
    }
    bool admitPrivateSource(const Source& source, Outcome& outcome)
    {
        auto reservation = Resource::AssetGpuBudget::reserve(source.bytes);
        if (!reservation || !Resource::AssetGpuBudget::retainUntilShutdown(reservation))
        {
            outcome.defer(source.eligible); each(source.eligible, [](LaneCounters& value) { ++value.privateGpuDenied; });
            refuse(source.eligible, Gate::Capacity); return false;
        }
        each(source.eligible, [](LaneCounters& value) { ++value.candidateAdmitted; });
        return true;
    }
    Tiers buildTiers(osg::Node& source, osg::Group& control, std::string_view identity,
        unsigned eligible, osg::Camera* camera, Outcome& outcome)
    {
        Tiers tiers;
        each(eligible, [](LaneCounters& value) { ++value.buildAttempts; });
        if (eligible & Lod)
        {
            tiers.lod = GeneratedStaticLod::build(control, identity, camera);
            if (tiers.lod.node && !Resource::AssetGpuBudget::retainUntilShutdown(tiers.lod.gpuReservation))
            { tiers.lod.node = nullptr; tiers.lod.retryable = true; }
            if (tiers.lod.node) outcome.completed |= Lod;
            else { ++Counters::lod.buildRefused; refuse(Lod, lodGate(tiers.lod.refusal)); if (tiers.lod.retryable) outcome.defer(Lod); }
        }
        if (eligible & Hlod)
        {
            tiers.hlod = buildSelectiveHlod(source, control);
            if (tiers.hlod && !Resource::AssetGpuBudget::retainUntilShutdown(tiers.hlod.gpuReservation))
            { tiers.hlod.representation = nullptr; tiers.hlod.retryable = true; }
            if (tiers.hlod) outcome.completed |= Hlod;
            else { ++Counters::hlod.buildRefused; refuse(Hlod, hlodGate(tiers.hlod)); if (tiers.hlod.retryable) outcome.defer(Hlod); }
        }
        return tiers;
    }
}
