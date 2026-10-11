#ifndef OPENMW_SCENEUTIL_GPUINDIRECTPACKETS_H
#define OPENMW_SCENEUTIL_GPUINDIRECTPACKETS_H

#include "persistentopaquepackets.hpp"
#include <components/shader/indirectprojectioncontract.hpp>
#include <components/resource/speculativebudget.hpp>
#include <osg/ContextData>
#include <osg/PrimitiveSetIndirect>
#include <osg/Program>
#include <osg/Shader>
#include <osg/Uniform>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <mutex>
#include <vector>

// The pinned OSG Windows headers omit these OpenGL 4.2 enum aliases.
// Values are from Khronos OpenGL-Registry/api/GL/glcorearb.h.
#ifndef GL_TRANSFORM_FEEDBACK_ACTIVE
#define GL_TRANSFORM_FEEDBACK_ACTIVE 0x8E24
#endif
#ifndef GL_TRANSFORM_FEEDBACK_PAUSED
#define GL_TRANSFORM_FEEDBACK_PAUSED 0x8E23
#endif

namespace SceneUtil::GpuIndirectPackets
{
    inline bool switchEnabled(const char* name)
    {
        const char* value = std::getenv(name);
        return value && std::strcmp(value, "1") == 0;
    }
    inline bool indirectEnabled()
    {
        static const bool value = switchEnabled("OPENMW_P9_MULTI_DRAW_INDIRECT")
            || switchEnabled("OPENMW_P9_GPU_VISIBILITY");
        return value;
    }
    inline bool visibilityEnabled()
    {
        static const bool value = switchEnabled("OPENMW_P9_GPU_VISIBILITY");
        return value;
    }
    struct Counters
    {
        inline static std::atomic_uint64_t built{ 0 }, draws{ 0 }, commands{ 0 }, dispatches{ 0 };
        inline static std::atomic_uint64_t fallbacks{ 0 }, busySlots{ 0 }, unknownShaders{ 0 }, liveBytes{ 0 };
        inline static std::atomic_uint64_t liveOwners{ 0 }, admissionDeclines{ 0 };
        inline static std::atomic_uint64_t commandUploads{0}, commandUploadBytes{0}, commandUploadsAvoided{0};
        inline static std::atomic_uint64_t nestedGeometry{0}, nestedBuilt{0}, nestedBranchDeclines{0};
        inline static std::atomic_uint64_t installSourceDeclines{0}, installCommandShapeDeclines{0};
        inline static std::atomic_uint64_t capabilityDraws{0}, capabilitySnapshots{0}, gpuResolutionsAvoided{0};
        inline static std::atomic_uint64_t visibilityOriginalDraws{0}, visibilityDispatchesAvoided{0};
        inline static std::atomic_uint64_t projectionProofReuses{0}, earlyAdmissionFallbacks{0}, earlyBusyFallbacks{0};
#ifdef OPENMW_INDIRECT_PACKET_TEST_HOOKS
        inline static std::atomic_uint64_t actualVersionQueries{0}, actualFunctionResolutions{0};
        inline static std::atomic_uint64_t actualProducerProofChecks{0};
#endif
    };
    // Configure before paging workers start. Each produced primitive captures
    // its cohort, so no live draw changes policy underneath pending commands.
    inline std::atomic_bool reuseIndirectCommands{false};
    inline std::atomic_bool reuseContextCapabilities{false};
    inline std::atomic_bool filterVisibilityOpportunity{false};
    inline std::atomic_bool reuseProjectionProof{false};
    inline std::atomic_bool earlyAdmissionFallback{false};
    using Sample = Resource::OpenGlPressureSample (*)(void*);
    // Startup/shutdown only, beside the existing upload-pool sampler; no new
    // OS query is introduced on a draw or paging worker.
    inline Sample sample = nullptr;
    inline void* sampleOwner = nullptr;
    inline Resource::SpeculativeBudget* ledger = nullptr;
    constexpr std::uint64_t MaximumLiveBytes = 32ull * 1024 * 1024;
    constexpr std::uint64_t MaximumLiveOwners = 1024;
    struct Capacity
    {
        std::uint64_t bytes = 0;
        Resource::SpeculativeBudget::ChargePtr charge;
        std::array<std::atomic_bool, 4> pendingContexts{};
        ~Capacity()
        {
            if (bytes)
            {
                Counters::liveBytes.fetch_sub(bytes, std::memory_order_relaxed);
                Counters::liveOwners.fetch_sub(1, std::memory_order_relaxed);
            }
        }
    };

    inline const osg::Uniform* appliedProjection(const osg::State& state, const osg::StateSet* leaf)
    {
        const osg::Uniform* inherited = nullptr;
        osg::StateAttribute::OverrideValue inheritedMode = 0;
        const auto found = state.getUniformMap().find("projectionMatrix");
        if (found != state.getUniformMap().end() && !found->second.uniformVec.empty())
        {
            inherited = found->second.uniformVec.back().first;
            inheritedMode = found->second.uniformVec.back().second;
        }
        const auto* local = leaf ? leaf->getUniformPair("projectionMatrix") : nullptr;
        if (local && (!(inheritedMode & osg::StateAttribute::OVERRIDE)
            || (local->second & osg::StateAttribute::PROTECTED))) return local->first;
        return inherited;
    }

    // A successful full producer proof may be borrowed only inside this draw's
    // held reader, for its exact State/Program on this thread. No cross-frame
    // recipe, context or uniform-value cache is created. Nested scopes restore
    // their predecessor, including failed/disabled scopes.
    class ProjectionProofScope
    {
    public:
        ProjectionProofScope(const osg::State* state, const osg::Program* program, bool verified)
            : mPrevious(sCurrent), mState(state), mProgram(program)
            , mVerified(verified && state && program && Shader::IndirectProjection::producerReaders)
        { sCurrent = this; }
        ~ProjectionProofScope() { sCurrent = mPrevious; }
        ProjectionProofScope(const ProjectionProofScope&) = delete;
        ProjectionProofScope& operator=(const ProjectionProofScope&) = delete;
        static bool current(const osg::State* state, const osg::Program* program)
        {
            return sCurrent && sCurrent->mVerified && Shader::IndirectProjection::producerReaders
                && sCurrent->mState == state && sCurrent->mProgram == program;
        }
    private:
        inline static thread_local const ProjectionProofScope* sCurrent = nullptr;
        const ProjectionProofScope* mPrevious;
        const osg::State* mState;
        const osg::Program* mProgram;
        const bool mVerified;
    };

    inline bool linearObjectProjection(const osg::Program* program, const osg::State* state = nullptr)
    {
        if (!program) return true; // Exact legacy fixed-function matrix path.
        if (ProjectionProofScope::current(state, program))
        { ++Counters::projectionProofReuses; return true; }
#ifdef OPENMW_INDIRECT_PACKET_TEST_HOOKS
        ++Counters::actualProducerProofChecks;
#endif
        return Shader::IndirectProjection::matches(*program);
    }

    inline bool commandProgramSafe(const osg::Program* program, const osg::State& state)
    {
        if (!program) return true;
#ifdef OPENMW_INDIRECT_PACKET_TEST_HOOKS
        ++Counters::actualProducerProofChecks;
#endif
        // Custom shaders can observe changed draw boundaries or vertex-cache
        // invocation counts through side effects. Token resemblance is not a
        // purity certificate. Exact shipped object recipes (including their
        // read-only clustered-light SSBOs) and legacy fixed function are the
        // entire initial command-repacking scope.
        return Shader::IndirectProjection::matches(*program) && Shader::IndirectProjection::safeImports(*program, state);
    }

    // Prove that the exact compute transform cannot reject any contained command
    // against its four side planes. Use double arithmetic on the actual float
    // inputs and a conservative allowance for float products, summation and FMA.
    // Ambiguous/boundary/non-finite cases keep the existing visibility pipeline.
    inline bool wholeBoundInside(const osg::BoundingBoxf& bounds, const osg::Matrixf& mvp)
    {
        if (!bounds.valid()) return false;
        for (unsigned row = 0; row < 4; ++row)
            for (unsigned column = 0; column < 4; ++column)
                if (!std::isfinite(mvp(row, column))) return false;
        for (unsigned corner = 0; corner < 8; ++corner)
        {
            const auto position = bounds.corner(corner);
            const std::array<double, 4> p{position.x(), position.y(), position.z(), 1.0};
            std::array<double, 3> clip{}, magnitude{};
            constexpr std::array<unsigned, 3> columns{0, 1, 3};
            for (unsigned axis = 0; axis < columns.size(); ++axis)
                for (unsigned row = 0; row < 4; ++row)
                {
                    const double term = p[row] * static_cast<double>(mvp(row, columns[axis]));
                    if (!std::isfinite(term)) return false;
                    clip[axis] += term;
                    magnitude[axis] += std::abs(term);
                }
            const double scale = std::max({1.0, magnitude[0], magnitude[1], magnitude[2]});
            if (!std::isfinite(scale) || scale > std::numeric_limits<float>::max() / 64.0) return false;
            const double margin = (64.0 * std::numeric_limits<float>::epsilon() + 0.0002) * scale;
            if (!(clip[2] > margin && std::abs(clip[0]) + margin < clip[2]
                && std::abs(clip[1]) + margin < clip[2])) return false;
        }
        return true;
    }

    inline const char* computeSource()
    {
        return R"GLSL(#version 430
layout(local_size_x=32) in;
layout(std430,binding=0) readonly buffer Inputs { vec4 data[]; };
struct Command { uint count; uint instances; uint first; uint baseVertex; uint baseInstance; };
layout(std430,binding=1) writeonly buffer Output { Command commands[]; };
uniform mat4 mvp;
uniform uint rows;
void main() {
    uint i=gl_GlobalInvocationID.x; if(i>=rows) return;
    vec3 lo=data[i*3].xyz, hi=data[i*3+1].xyz;
    uvec4 info=floatBitsToUint(data[i*3+2]);
    bvec4 allOutside=bvec4(true);
    for(uint corner=0;corner<8;corner++) {
        vec3 p=vec3((corner&1u)!=0u?hi.x:lo.x,(corner&2u)!=0u?hi.y:lo.y,(corner&4u)!=0u?hi.z:lo.z);
        vec4 c=mvp*vec4(p,1.0);
        float e=0.0001*max(1.0,max(abs(c.w),max(abs(c.x),abs(c.y))));
        if(any(isnan(c)) || any(isinf(c))) { allOutside=bvec4(false); break; }
        allOutside=bvec4(allOutside.x && c.x < -c.w-e, allOutside.y && c.x > c.w+e,
                        allOutside.z && c.y < -c.w-e, allOutside.w && c.y > c.w+e);
    }
    commands[i]=Command(info.x,any(allOutside)?0u:1u,info.y,0u,0u);
})GLSL";
    }

#include "gpuindirectstorage.inl"

    class Draw final : public PersistentOpaquePackets::RetainedOpaqueDrawCallback
    {
    public:
        Draw(osg::Geometry* candidate, const osg::Geometry& source, std::shared_ptr<Capacity> capacity)
            : mCandidate(candidate), mCapacity(std::move(capacity)), mVertex(source.getVertexArray())
            , mVertexGeneration(mVertex->getModifiedCount()), mMaterial(source.getStateSet())
            , mReuseProjectionProof(reuseProjectionProof.load(std::memory_order_relaxed))
            , mEarlyAdmissionFallback(earlyAdmissionFallback.load(std::memory_order_relaxed))
        {
            for (const auto& primitive : source.getPrimitiveSetList())
            {
                mPrimitiveOwners.push_back(primitive);
                mProducer.emplace_back(primitive);
            }
        }
        void drawImplementation(osg::RenderInfo& info, const osg::Drawable* drawable) const override
        {
            const auto* geometry = drawable ? drawable->asGeometry() : nullptr;
            const auto* state = info.getState();
            if (mEarlyAdmissionFallback && state && mCandidate->getNumPrimitiveSets() == 1
                && static_cast<const Primitive*>(mCandidate->getPrimitiveSet(0))->earlyFallbackBeforeProof(*state))
            {
                // Always draw the live authoritative source. In particular,
                // no stale candidate arrays/indices or shader proof is used.
                ++Counters::fallbacks;
                if (drawable) drawable->drawImplementation(info);
                return;
            }
            Shader::IndirectProjection::ProducerReadScope producer;
            const auto* pcp = state ? state->getLastAppliedProgramObject() : nullptr;
            const bool safeProgram = producer && state && commandProgramSafe(pcp ? pcp->getProgram() : nullptr, *state);
            if (!safeProgram) ++Counters::unknownShaders;
            bool current = geometry && geometry->getVertexArray() == mVertex.get()
                && mVertex->getModifiedCount() == mVertexGeneration && geometry->getStateSet() == mMaterial.get()
                && geometry->getNumPrimitiveSets() == mProducer.size()
                && safeProgram && PersistentOpaquePackets::opaque(geometry->getStateSet())
                // RenderLeaf has already resolved inherited OVERRIDE/PROTECTED
                // state. Use OSG's const applied-mode cache; this introduces no
                // driver queries or allocations. A material can become sorted
                // in place after installation, or inherit one of these modes.
                && state && !state->getLastAppliedMode(GL_BLEND)
                && !state->getLastAppliedMode(GL_ALPHA_TEST)
                && !state->getLastAppliedMode(GL_STENCIL_TEST);
            if (current)
            {
                current = geometry->getNormalArray() == mCandidate->getNormalArray()
                    && geometry->getColorArray() == mCandidate->getColorArray()
                    && geometry->getSecondaryColorArray() == mCandidate->getSecondaryColorArray()
                    && geometry->getFogCoordArray() == mCandidate->getFogCoordArray()
                    && geometry->getTexCoordArrayList() == mCandidate->getTexCoordArrayList()
                    && geometry->getVertexAttribArrayList() == mCandidate->getVertexAttribArrayList()
                    && geometry->getDataVariance() != osg::Object::DYNAMIC;
            }
            if (current) for (unsigned i = 0; i < geometry->getNumPrimitiveSets(); ++i)
                current = current && mProducer[i].matches(geometry->getPrimitiveSet(i));
            if (current)
            {
                ProjectionProofScope proof(state, pcp ? pcp->getProgram() : nullptr,
                    mReuseProjectionProof && safeProgram && producer);
                mCandidate->drawImplementation(info);
            }
            else
            {
                Counters::fallbacks.fetch_add(1, std::memory_order_relaxed);
                if (drawable) drawable->drawImplementation(info);
            }
        }
#ifdef OPENMW_INDIRECT_PACKET_TEST_HOOKS
        const Primitive* testPrimitive() const { return static_cast<const Primitive*>(mCandidate->getPrimitiveSet(0)); }
#endif
    protected:
        ~Draw() override { if (mCapacity && mCapacity->charge) mCapacity->charge->pending(); }
    private:
        osg::ref_ptr<osg::Geometry> mCandidate;
        std::shared_ptr<Capacity> mCapacity;
        osg::ref_ptr<const osg::Array> mVertex;
        unsigned mVertexGeneration;
        osg::ref_ptr<const osg::StateSet> mMaterial;
        std::vector<PersistentOpaquePackets::PrimitiveRevision> mProducer;
        std::vector<osg::ref_ptr<const osg::PrimitiveSet>> mPrimitiveOwners;
        const bool mReuseProjectionProof = false, mEarlyAdmissionFallback = false;
    };

    inline bool install(osg::Geometry& original, bool requested = indirectEnabled(), bool gpu = visibilityEnabled())
    {
        if (!requested) return false;
        const auto sourceDecline = [] { ++Counters::installSourceDeclines; return false; };
        if (!PersistentOpaquePackets::eligible(original) || original.getDrawCallback()
            || !original.getUseVertexBufferObjects() || original.getUseDisplayList()) return sourceDecline();
        const auto* vertices = dynamic_cast<const osg::Vec3Array*>(original.getVertexArray());
        if (!vertices || vertices->empty()) return sourceDecline();
        // Per-primitive-set attributes depend on original draw boundaries.
        osg::Geometry::ArrayList arrays; original.getArrayList(arrays);
        for (const auto& array : arrays)
            if (array && array->getBinding() == osg::Array::BIND_PER_PRIMITIVE_SET) return sourceDecline();
        constexpr std::size_t MaximumIndices = 196608, ClusterIndices = 3072, MaximumCommands = 128;
        std::size_t totalIndices = 0, totalCommands = 0;
        for (const auto& primitive : original.getPrimitiveSetList())
        {
            if (!primitive || !primitive->getDrawElements() || primitive->getMode() != GL_TRIANGLES
                || primitive->getNumInstances() != 0 || primitive->getNumIndices() % 3
                || primitive->getNumIndices() > MaximumIndices - totalIndices) return sourceDecline();
            totalIndices += primitive->getNumIndices();
            totalCommands += (primitive->getNumIndices() + ClusterIndices - 1) / ClusterIndices;
        }
        if (totalCommands < 2 || totalCommands > MaximumCommands)
        { ++Counters::installCommandShapeDeclines; return false; }
        const std::uint64_t charge = totalIndices * sizeof(unsigned) * 5 + totalCommands * 3 * sizeof(osg::Vec4) * 5
            + totalCommands * sizeof(osg::DrawElementsIndirectCommand) * 9 + sizeof(Draw) + sizeof(Primitive)
            + original.getNumPrimitiveSets() * (sizeof(PersistentOpaquePackets::PrimitiveRevision) + sizeof(void*) * 2)
            + arrays.size() * sizeof(void*) * 2;
        if (!sample || !ledger) { ++Counters::admissionDeclines; return false; }
        std::optional<Resource::SpeculativeBudget::Stage> reservation;
        try { reservation.emplace(ledger->reserve(sample(sampleOwner), charge)); }
        catch (const Resource::SpeculativeDeferred&) { ++Counters::admissionDeclines; return false; }
        std::shared_ptr<Capacity> capacity;
        try { capacity = std::make_shared<Capacity>(); }
        catch (const std::bad_alloc&) { return false; }
        auto owners = Counters::liveOwners.load(std::memory_order_relaxed);
        do { if (owners >= MaximumLiveOwners) return false; }
        while (!Counters::liveOwners.compare_exchange_weak(owners, owners + 1, std::memory_order_relaxed));
        auto current = Counters::liveBytes.load(std::memory_order_relaxed);
        do
        {
            if (charge > MaximumLiveBytes || current > MaximumLiveBytes - charge)
            { Counters::liveOwners.fetch_sub(1, std::memory_order_relaxed); return false; }
        } while (!Counters::liveBytes.compare_exchange_weak(current, current + charge, std::memory_order_relaxed));
        capacity->bytes = charge;
        try
        {
            capacity->charge = ledger->track({ capacity.get(), 11u }, charge, false, &*reservation);
            std::vector<unsigned> indices;
            std::vector<osg::DrawElementsIndirectCommand> commands;
            osg::ref_ptr<osg::Vec4Array> bounds = new osg::Vec4Array;
            indices.reserve(totalIndices); commands.reserve(totalCommands); bounds->reserve(totalCommands * 3);
            for (const auto& primitive : original.getPrimitiveSetList())
            {
                if (!primitive || !primitive->getDrawElements() || primitive->getMode() != GL_TRIANGLES
                    || primitive->getNumInstances() != 0 || primitive->getNumIndices() % 3
                    || primitive->getNumIndices() > MaximumIndices - indices.size()) return false;
                for (std::size_t begin = 0; begin < primitive->getNumIndices(); begin += ClusterIndices)
                {
                    const auto end = (std::min)(begin + ClusterIndices, static_cast<std::size_t>(primitive->getNumIndices()));
                    const unsigned first = static_cast<unsigned>(indices.size());
                    osg::BoundingBox box;
                    for (std::size_t i = begin; i < end; ++i)
                    {
                        const unsigned index = primitive->index(static_cast<unsigned>(i));
                        if (index >= vertices->size()) return false;
                        const auto& vertex = (*vertices)[index];
                        if (!std::isfinite(vertex.x()) || !std::isfinite(vertex.y()) || !std::isfinite(vertex.z())) return false;
                        indices.push_back(index); box.expandBy(vertex);
                    }
                    const auto count = static_cast<unsigned>(end - begin);
                    commands.emplace_back(count, 1, first, 0, 0);
                    bounds->push_back(osg::Vec4(box._min, 0)); bounds->push_back(osg::Vec4(box._max, 0));
                    bounds->push_back(osg::Vec4(std::bit_cast<float>(count), std::bit_cast<float>(first), 0, 0));
                    if (commands.size() > MaximumCommands) return false;
                }
            }
            if (commands.size() < 2) return false;
            osg::ref_ptr<osg::Geometry> candidate = new osg::Geometry(original, osg::CopyOp::SHALLOW_COPY);
            candidate->setDrawCallback(nullptr);
            candidate->removePrimitiveSet(0, candidate->getNumPrimitiveSets());
            candidate->addPrimitiveSet(new Primitive(std::move(indices), std::move(commands), bounds, original.getStateSet(), gpu, capacity, original.getPrimitiveSetList()));
            original.setDrawCallback(new Draw(candidate, original, capacity));
            Counters::built.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        catch (const std::bad_alloc&)
        {
            return false;
        }
    }

    // The paging caller must certify the ORIGINAL ancestry before CopyOp and
    // pass only its private optimized graph. Preserve material/transform nodes;
    // attach the same revision-checked callback to eligible geometry leaves.
    // Controller-stripped clones alone are not a source certificate.
    inline unsigned installCertifiedSubtrees(osg::Node& original, unsigned maximumNodes = 1024,
        bool requested = indirectEnabled(), bool gpu = visibilityEnabled())
    {
        if (!requested) return 0;
        unsigned nodes = 0, installed = 0;
        const auto visit = [&](const auto& self, osg::Node& node, unsigned depth) -> void {
            const auto* state = node.getStateSet();
            if (++nodes > maximumNodes || depth > 16 || node.getDataVariance() == osg::Object::DYNAMIC
                || node.getUpdateCallback() || node.getEventCallback() || node.getCullCallback()
                || node.getComputeBoundingSphereCallback() || !PersistentOpaquePackets::opaque(state)
                || (state && state->getEventCallback()))
            { ++Counters::nestedBranchDeclines; return; }
            if (auto* geometry = node.asGeometry())
            {
                ++Counters::nestedGeometry;
                if (install(*geometry, true, gpu)) { ++installed; ++Counters::nestedBuilt; }
                return;
            }
            auto* group = node.asGroup();
            if (!group || (typeid(node) != typeid(osg::Group) && typeid(node) != typeid(osg::Geode)
                && typeid(node) != typeid(osg::MatrixTransform)))
            { ++Counters::nestedBranchDeclines; return; }
            if (const auto* transform = dynamic_cast<const osg::MatrixTransform*>(&node))
            {
                if (transform->getReferenceFrame() != osg::Transform::RELATIVE_RF)
                { ++Counters::nestedBranchDeclines; return; }
                for (unsigned row = 0; row < 4; ++row) for (unsigned column = 0; column < 4; ++column)
                    if (!std::isfinite(transform->getMatrix()(row, column)))
                    { ++Counters::nestedBranchDeclines; return; }
            }
            for (unsigned i = 0; i < group->getNumChildren() && nodes < maximumNodes; ++i)
                self(self, *group->getChild(i), depth + 1);
        };
        visit(visit, original, 0);
        return installed;
    }
}
#endif
