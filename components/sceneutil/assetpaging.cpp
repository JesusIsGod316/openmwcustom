#include "assetpaging.hpp"
#include "riggeometry.hpp"
#include "morphgeometry.hpp"
#include <components/resource/assetgpubudget.hpp>
#include <components/resource/derivedassetcache.hpp>
#include <components/vfs/manager.hpp>
#include <components/debug/debuglog.hpp>
#include <osg/Geometry>
#include <osg/Program>
#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <sstream>

namespace SceneUtil::AssetPaging
{
    void retainPagingOwners(osg::Node& root,std::uint64_t controlBytes,std::uint64_t privateBytes,
        std::uint64_t derivedBytes,Resource::SpeculativeScope::Stage* privateAdmission)
    {
        const auto context=Resource::SpeculativeScope::capture();
        if(!context.budget || !context.budget->experimentEnabled())
        {retainHostOwner(root,controlBytes+privateBytes+derivedBytes);return;}
        // Derived owner receipts already live on their exact graphs. Private
        // copies are a distinct incremental part of this root, adopted from
        // their original private reservation. Never recharge borrowed arrays.
        retainHostOwner(root,controlBytes);
        if(privateBytes)
            retainExperimentalOwner(root,privateBytes,nullptr,privateAdmission,0x50395052u);
    }
    void noteOptionalTransient(unsigned mask) noexcept
    { OptionalUpgradeScope::noteTransient(mask); }
    void noteOptionalAttempt(unsigned mask) noexcept
    { OptionalUpgradeScope::noteAttempt(mask); }
    void noteOptionalAdmissionWait(unsigned mask) noexcept
    { OptionalUpgradeScope::noteAdmissionWait(mask); }
    bool admissionWaiting(const Resource::SpeculativeDeferred& error) noexcept
    {
        if (!error.reservation || !error.reservation->reasons) return false;
        const auto& receipt = *error.reservation;
        using Budget = Resource::SpeculativeBudget;
        constexpr auto permanent = Budget::AccountingSaturated | Budget::InvalidRequestedBytes
            | Budget::RegressedGrowthWatermark | Budget::ArithmeticOverflow | Budget::ExperimentReservationMismatch;
        if (receipt.reasons & permanent) return false;
        // A request that cannot fit by itself cannot be fixed by waiting for
        // another owner. Preserve bounded failure/exhaustion for that case.
        if ((receipt.reasons & Budget::AggregateTransientLimit) && receipt.requested > receipt.transientLimit)
            return false;
        if ((receipt.reasons & Budget::ExperimentTotalLimit) && receipt.requested > receipt.experimentLimit)
            return false;
        if ((receipt.reasons & Budget::ExperimentScratchLimit) && receipt.requested > receipt.experimentScratchLimit)
            return false;
        return true;
    }
    void cpuRefusal(unsigned mask,CpuStage stage,const Resource::SpeculativeDeferred& error)
    {
        const unsigned slot = static_cast<unsigned>(stage);
        if (slot >= static_cast<unsigned>(CpuStage::Count)) return;
        for (unsigned bit : {Lod,Hlod}) if (mask & bit)
        {
            auto& counters = lane(bit);
            if (!error.reservation)
            {
                if (counters.cpuUnclassified[slot].fetch_add(1) == 0)
                    Log(Debug::Info) << "OptimizedMW asset CPU first refusal: lane=" << (bit==Lod ? "L1" : "H1")
                        << " stage=" << slot << " reason=unclassified reservation_receipt=0";
                continue;
            }
            const auto& value = *error.reservation;
            bool first = false;
            for (unsigned i=0;i<Resource::SpeculativeBudget::ReservationRefusalCount;++i)
                if ((value.reasons & (1u<<i)) && counters.cpuReasons[slot][i].fetch_add(1)==0) first=true;
            if (!first) continue;
            const auto& sample = value.sample;
            std::ostringstream receipt;
            receipt << "OptimizedMW asset CPU first refusal: lane=" << (bit==Lod ? "L1" : "H1")
                << " stage=" << slot << " reason_bits=" << value.reasons << " pressure_reason_bits=" << value.pressureReasons
                << " requested=" << value.requested << " current_future=" << value.currentFuture
                << " proposed_future=" << value.proposedFuture << " transient_limit=" << value.transientLimit
                << " growth=" << value.growth << " unobserved_growth=" << value.unobservedGrowth << " extra=" << value.extra
                << " known_owners=" << value.knownOwners << " estimated_owners=" << value.estimatedOwners
                << " pending_owners=" << value.pendingOwners << " jobs=" << value.jobs
                << " job_permit=" << value.jobPermit << " experimental=" << value.experimental
                << " experiment_jobs=" << value.experimentJobs << " experiment_job_limit=" << value.experimentJobLimit
                << " experiment_future=" << value.experimentFuture << " experiment_retained=" << value.experimentRetained
                << " experiment_limit=" << value.experimentLimit << " experiment_scratch_limit=" << value.experimentScratchLimit
                << " priority=" << static_cast<unsigned>(value.priority) << " sample_generation=" << sample.generation
                << " sampled_at_ms=" << sample.sampledAtMs << " pressure=" << static_cast<unsigned>(sample.decision.state)
                << " admissions_per_sample=" << sample.decision.admissionsPerSample
                << " accounted_growth=" << sample.accountedGrowth << " growth_watermark_valid=" << sample.growthWatermarkValid
                << " physical_total=" << sample.memory.physicalTotal << " physical_available=" << sample.memory.physicalAvailable
                << " physical_valid=" << sample.memory.physicalValid << " physical_floor=" << value.physicalFloor
                << " commit_available=" << sample.memory.commitAvailable << " commit_valid=" << sample.memory.commitValid
                << " system_commit_available=" << sample.memory.systemCommitAvailable << " system_commit_valid=" << sample.memory.systemCommitValid
                << " commit_floor=" << value.commitFloor << " low_memory_valid=" << sample.memory.lowMemoryValid
                << " low_memory=" << sample.memory.lowMemory;
            Log(Debug::Info) << receipt.str();
        }
    }
    void OptionalUpgradeScope::reserveReference()
    {
        if (!incrementalControl()) return;
        try { sCurrent->mOverlap->reserveAdditional(512,128 * Resource::SpeculativeBudget::MiB); }
        catch (const Resource::SpeculativeDeferred& error)
        {
            cpuRefusal(Lod,CpuStage::Outer,error);
            ++Counters::lod.outerCpuDenied; refuse(Lod,Gate::Capacity);
            noteOptionalTransient(Lod); throw;
        }
    }
    ControlEstimate controlEstimate(const osg::Node& original)
    {
        struct Census
        {
            std::uint64_t graph = 0, streams = 0, indices = 0, triangleIndices = 0, vertices = 0;
            unsigned nodes = 0;
            bool valid = true;
            void add(std::uint64_t& target,std::uint64_t bytes)
            { if (bytes > UINT64_MAX-target) valid=false; else target+=bytes; }
            void visit(const osg::Node& node,unsigned depth)
            {
                if (!valid || depth>64 || ++nodes>16384 || node.getName().size()>4096
                    || node.getDescriptions().size()>1024)
                { valid=false; return; }
                add(graph,2048+node.getName().size()*2ull);
                for (const auto& description:node.getDescriptions())
                { if(description.size()>4096) {valid=false;return;} add(graph,64+description.size()*2ull); }
                // Paging CopyOp unwraps these ordinary (ineligible for L1)
                // drawables. Their source streams are not child nodes, so the
                // allocation census must follow the same hidden ownership.
                if(const auto* rig=dynamic_cast<const RigGeometry*>(&node))
                { if(const auto source=rig->getSourceGeometry()) visit(*source,depth+1); else valid=false; return; }
                if(const auto* morph=dynamic_cast<const MorphGeometry*>(&node))
                { if(const auto source=morph->getSourceGeometry()) visit(*source,depth+1); else valid=false; return; }
                if (const auto* geometry=dynamic_cast<const osg::Geometry*>(&node))
                {
                    if(geometry->getNumTexCoordArrays()>32 || geometry->getNumVertexAttribArrays()>32
                        || geometry->getNumPrimitiveSets()>1024) {valid=false;return;}
                    const auto array=[&](const osg::Array* value) {
                        if(value) {add(streams,value->getTotalDataSize());add(graph,256);}
                    };
                    array(geometry->getVertexArray()); array(geometry->getNormalArray()); array(geometry->getColorArray());
                    array(geometry->getSecondaryColorArray()); array(geometry->getFogCoordArray());
                    if(geometry->getVertexArray()) add(vertices,geometry->getVertexArray()->getNumElements());
                    for(unsigned i=0;i<geometry->getNumTexCoordArrays();++i) array(geometry->getTexCoordArray(i));
                    for(unsigned i=0;i<geometry->getNumVertexAttribArrays();++i) array(geometry->getVertexAttribArray(i));
                    for(unsigned i=0;i<geometry->getNumPrimitiveSets();++i)
                    {
                        const auto* primitive=geometry->getPrimitiveSet(i);
                        if(!primitive) { valid=false;return; }
                        const std::uint64_t count=primitive->getNumIndices();
                        add(indices,count); add(graph,256);
                        switch(primitive->getMode())
                        {
                            case GL_TRIANGLES: add(triangleIndices,count); break;
                            case GL_TRIANGLE_STRIP: case GL_TRIANGLE_FAN: case GL_POLYGON:
                            case GL_QUADS: case GL_QUAD_STRIP:
                                // TriangleIndexFunctor expands these modes;
                                // repeated/degenerate strips still allocate the
                                // expanded input and pinned adjacency scratch.
                                if(count>UINT64_MAX/3) valid=false;
                                else add(triangleIndices,count*3);
                                break;
                            case GL_POINTS: case GL_LINES: case GL_LINE_STRIP: case GL_LINE_LOOP: break;
                            default: valid=false; break;
                        }
                    }
                }
                if(const auto* children=node.asGroup())
                {
                    if(children->getNumChildren()>16384) {valid=false;return;}
                    add(graph,children->getNumChildren()*32ull);
                    for(unsigned i=0;i<children->getNumChildren() && valid;++i) visit(*children->getChild(i),depth+1);
                }
            }
        } census;
        census.visit(original,0);
        constexpr auto maximum=128 * Resource::SpeculativeBudget::MiB;
        // Existing source arrays remain owned. New flattened/merged output,
        // clone coexistence, index conversions, merge working vectors and the
        // pinned post-transform triangle/vertex adjacency all fit this bound.
        if(!census.valid || census.graph>maximum || census.streams>maximum/4
            || census.indices>maximum/8 || census.triangleIndices>maximum/80 || census.vertices>maximum/192) return {};
        const auto retained=census.graph+census.streams*2+census.indices*4+census.triangleIndices*4;
        const auto peak=census.graph*2+census.streams*4+census.indices*8+census.triangleIndices*80+census.vertices*192;
        if(peak>maximum || retained>peak) return {};
        return {peak,retained,true};
    }
    std::unique_ptr<Resource::SpeculativeScope::Stage> reserveControlSources(unsigned lanes,Outcome& outcome)
    {
        try { return std::make_unique<Resource::SpeculativeScope::Stage>(0,0); }
        catch (const Resource::SpeculativeDeferred& error)
        {
            cpuRefusal(lanes,CpuStage::Outer,error); outcome.defer(lanes,admissionWaiting(error));
            each(lanes,[](LaneCounters& value) {++value.outerCpuDenied;}); refuse(lanes,Gate::Capacity);
            return {};
        }
    }
    bool admitControlSource(const ControlEstimate& estimate,unsigned lanes,Outcome& outcome,
        Resource::SpeculativeScope::Stage& admission)
    {
        try
        {
            if(!estimate.valid || !estimate.peakBytes || estimate.retainedBytes>estimate.peakBytes)
            {
                each(lanes,[](LaneCounters& value) {++value.controlCensusRefused;});
                throw Resource::SpeculativeDeferred{};
            }
            admission.reserveAdditional(estimate.peakBytes,128 * Resource::SpeculativeBudget::MiB);
            each(lanes,[&](LaneCounters& value) {
                ++value.controlSourcesAdmitted; value.controlReservationBytes+=estimate.peakBytes;
                value.controlRetainedBytes+=estimate.retainedBytes;
            });
            outcome.constructionStarted |= lanes;
            outcome.admissionWait &= ~lanes;
            return true;
        }
        catch (const Resource::SpeculativeDeferred& error)
        {
            cpuRefusal(lanes,CpuStage::Outer,error); outcome.defer(lanes,admissionWaiting(error));
            each(lanes,[](LaneCounters& value) {++value.outerCpuDenied;}); refuse(lanes,Gate::Capacity);
            return false;
        }
    }
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
        result.admissionWait = (result.admissionWait & ~outcome.attempted) | outcome.admissionWait;
        if (outcome.completed) result.readiness = outcome.readiness;
        for (unsigned i = 0; i < 2; ++i)
        {
            const unsigned bit = 1u << i;
            if (!(outcome.attempted & bit)) continue;
            if (outcome.admissionWait & bit)
            {
                result.retryAfter[i] = now <= UINT64_MAX-RetryDelayMilliseconds ? now+RetryDelayMilliseconds : UINT64_MAX;
                continue;
            }
            if ((transient | completed) & bit) ++Counters::retryAttempts;
            result.attempts[i] = static_cast<unsigned char>(std::min<unsigned>(MaximumAttempts, result.attempts[i] + 1));
            if (!((outcome.transient | outcome.completed) & bit)) continue;
            if (result.attempts[i] >= MaximumAttempts)
            { result.exhausted |= bit; ++Counters::retryExhausted; }
            else result.retryAfter[i] = now <= UINT64_MAX-RetryDelayMilliseconds ? now+RetryDelayMilliseconds : UINT64_MAX;
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
    void refusalDetail(unsigned lanes,GeneratedStaticLod::Detail detail,std::string_view model,const GeneratedStaticLod::RefusalInfo* info)
    {
        if(detail==GeneratedStaticLod::Detail::None) detail=GeneratedStaticLod::Detail::Other;
        const auto clean=[](std::string_view value,std::size_t maximum) {
            std::string text(value.substr(0,maximum));for(char& c:text) if(static_cast<unsigned char>(c)<32 || c=='"') c='?';return text;
        };
        for(unsigned bit:{Lod,Hlod}) if(lanes&bit)
        {
            const auto previous=lane(bit).details[static_cast<unsigned>(detail)].fetch_add(1);
            if(previous) continue; // At most one small example per lane/subtype.
            Log(Debug::Info)<<"OptimizedMW asset first refusal: lane="<<(bit==Lod?"L1":"H1")
                <<" detail="<<GeneratedStaticLod::detailName(detail)<<" model=\""<<clean(model,192)<<'"'
                <<" node=\""<<clean(info && info->node?info->node->getName():std::string_view{},96)<<'"'
                <<" program=\""<<clean(info && info->program?info->program->getName():std::string_view{},96)<<'"';
        }
    }
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
            GeneratedStaticLod::RefusalInfo info;
            const auto certificate = GeneratedStaticLod::certify(original,&info);
            if (certificate == GeneratedStaticLod::Refusal::None) eligible |= Lod;
            else {refuse(Lod, lodGate(certificate));refusalDetail(Lod,info.detail,model.value(),&info);}
        }
        if (requested & Hlod)
        {
            GeneratedStaticLod::RefusalInfo info;
            if (!certifySelectiveHlodTemplate(original,&info)) {refuse(Hlod, Gate::Graph);refusalDetail(Hlod,info.detail,model.value(),&info);}
            else if (qualifySelectiveHlodTemplate(original,true,&info)) eligible |= Hlod;
            else {refuse(Hlod, Gate::Material);refusalDetail(Hlod,info.detail,model.value(),&info);}
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
        catch (const Resource::SpeculativeDeferred& error)
        { cpuRefusal(eligible,CpuStage::Snapshot,error); outcome.defer(eligible,admissionWaiting(error)); each(eligible, [](LaneCounters& value) { ++value.snapshotDenied; }); refuse(eligible, Gate::Capacity); }
        catch (...)
        { each(eligible, [](LaneCounters& value) { ++value.snapshotDenied; }); refuse(eligible, Gate::Source); }
        return {};
    }
    std::unique_ptr<Resource::SpeculativeScope::Stage> reservePrivateSources(unsigned eligible, Outcome& outcome)
    {
        // Reserve each bounded private clone before it is made. The source
        // estimate includes its retained arrays, primitive data and conservative
        // node overhead. Unused population capacity is not a simultaneous live
        // allocation. Every admitted copy stays reserved through the final root
        // charge, so original/control/private/cook overlap remains additive.
        try { return std::make_unique<Resource::SpeculativeScope::Stage>(0,0,true); }
        catch (const Resource::SpeculativeDeferred& error)
        { cpuRefusal(eligible,CpuStage::Private,error); outcome.defer(eligible,admissionWaiting(error)); each(eligible, [](LaneCounters& value) { ++value.privateCpuDenied; }); refuse(eligible, Gate::Capacity); }
        return {};
    }
    bool admitPrivateSource(const Source& source, Outcome& outcome,
        Resource::SpeculativeScope::Stage& hostAdmission,std::size_t copyBytes)
    {
        try { hostAdmission.reserveAdditional(copyBytes,32 * Resource::SpeculativeBudget::MiB); }
        catch (const Resource::SpeculativeDeferred& error)
        {
            cpuRefusal(source.eligible,CpuStage::Private,error);
            outcome.defer(source.eligible,admissionWaiting(error)); each(source.eligible,[](LaneCounters& value) { ++value.privateCpuDenied; });
            refuse(source.eligible,Gate::Capacity); return false;
        }
        auto reservation = Resource::AssetGpuBudget::reserve(source.bytes);
        if (!reservation || !Resource::AssetGpuBudget::retainUntilShutdown(reservation))
        {
            // No private clone was allocated, so this CPU future can be
            // released. The GPU accounting retains its existing policy.
            hostAdmission.releaseUnallocated(copyBytes);
            outcome.defer(source.eligible); each(source.eligible, [](LaneCounters& value) { ++value.privateGpuDenied; });
            refuse(source.eligible, Gate::Capacity); return false;
        }
        each(source.eligible, [](LaneCounters& value) { ++value.candidateAdmitted; });
        outcome.constructionStarted |= source.eligible;
        outcome.admissionWait &= ~source.eligible;
        return true;
    }
    Tiers buildTiers(osg::Node& source, osg::Group& control, std::string_view identity,
        unsigned eligible, osg::Camera* camera, Outcome& outcome, bool activeGridLod, bool cacheHitAdmission)
    {
        Tiers tiers;
        std::unique_ptr<Resource::ExperimentalSpeculativeScope> experimental;
        try {experimental=std::make_unique<Resource::ExperimentalSpeculativeScope>();}
        catch(const Resource::SpeculativeDeferred& error)
        {
            cpuRefusal(eligible,CpuStage::Private,error);outcome.defer(eligible,admissionWaiting(error));
            each(eligible,[](LaneCounters& value) {++value.privateCpuDenied;});refuse(eligible,Gate::Capacity);
            return tiers;
        }
        each(eligible, [](LaneCounters& value) { ++value.buildAttempts; });
        outcome.constructionStarted |= eligible;
        outcome.admissionWait &= ~eligible;
        if (eligible & Lod)
        {
            if (activeGridLod) ++GeneratedStaticLod::Counters::activeBuildAttempts;
            tiers.lod = GeneratedStaticLod::build(control, identity, camera,true,activeGridLod,cacheHitAdmission);
            if (tiers.lod.node && !Resource::AssetGpuBudget::retainUntilShutdown(tiers.lod.gpuReservation))
            {
                tiers.lod.node = nullptr;
                tiers.lod.retryable = true;
                tiers.lod.refusal = GeneratedStaticLod::Refusal::Capacity;
                tiers.lod.gpuRefusalReasons = Resource::AssetGpuBudget::RetainAllocationFailed;
                tiers.lod.allocationFailure = true;
                GeneratedStaticLod::recordBuildFailure(GeneratedStaticLod::BuildStep::GpuRetention, tiers.lod, identity);
            }
            if (tiers.lod.node)
            {
                outcome.completed |= Lod;
                if (activeGridLod) ++GeneratedStaticLod::Counters::activeBuilt;
            }
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
