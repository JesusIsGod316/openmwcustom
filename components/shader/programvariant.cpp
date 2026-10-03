#include "programvariant.hpp"

#include <algorithm>
#include <cstdlib>
#include <mutex>
#include <string_view>

#include <components/debug/hitchtrace.hpp>

namespace Shader
{
    bool exactProgramVariantsEnabled()
    {
        static const bool enabled = [] {
            const char* value = std::getenv("OPENMW_EXACT_PROGRAM_VARIANTS");
            return value && std::string_view(value) == "1";
        }();
        return enabled;
    }

    ProgramVariantStats& programVariantStats()
    {
        static ProgramVariantStats stats;
        return stats;
    }

    std::optional<ProgramLinkIdentity::CaptureSize> ProgramLinkIdentity::boundedCaptureSize(
        const osg::Program& program)
    {
        if ((program.getProgramBinary() && program.getProgramBinary()->getSize())
            || program.getNumShaders() > sMaxShaders || program.getNumTransformFeedBackVaryings() > sMaxBindings)
            return {};
        CaptureSize result;
        const auto add = [&](const std::string& text) {
            if (text.size() > sMaxNameBytes || text.size() > sMaxMetadataBytes - result.metadata) return false;
            result.metadata += text.size();
            return true;
        };
        const auto bindings = [&](const auto& values) {
            if (values.size() > sMaxBindings) return false;
            for (const auto& [name, value] : values)
            {
                (void)value;
                if (!add(name)) return false;
            }
            return true;
        };
        const auto names = [&](const auto& values) {
            if (values.size() > sMaxDefines) return false;
            for (const auto& name : values) if (!add(name)) return false;
            return true;
        };
        if (!bindings(program.getAttribBindingList()) || !bindings(program.getFragDataBindingList())
            || !bindings(program.getUniformBlockBindingList()) || !names(program.getShaderDefines()))
            return {};
        for (unsigned i = 0; i < program.getNumTransformFeedBackVaryings(); ++i)
            if (!add(program.getTransformFeedBackVarying(i))) return {};
        for (unsigned i = 0; i < program.getNumShaders(); ++i)
        {
            const auto* shader = program.getShader(i);
            if (!shader || shader->getShaderSource().size() > 2u * 1024u * 1024u - result.sources
                || !names(shader->getShaderDefines()) || !names(shader->getShaderRequirements()))
                return {};
            result.sources += shader->getShaderSource().size();
        }
        return result;
    }

    std::optional<ProgramLinkIdentity> ProgramLinkIdentity::capture(const osg::Program& program)
    {
        // A ProgramBinary belongs to Program, not to its separate define PCPs.
        // This first source-link candidate cannot safely adopt populated binaries.
        if (!boundedCaptureSize(program))
            return std::nullopt;
        ProgramLinkIdentity result;
        result.attributes = program.getAttribBindingList();
        result.fragments = program.getFragDataBindingList();
        result.blocks = program.getUniformBlockBindingList();
        result.defines = program.getShaderDefines();
        result.feedbackMode = program.getTransformFeedBackMode();
        result.geometryVertices = program.getParameter(GL_GEOMETRY_VERTICES_OUT_EXT);
        result.geometryInput = program.getParameter(GL_GEOMETRY_INPUT_TYPE_EXT);
        result.geometryOutput = program.getParameter(GL_GEOMETRY_OUTPUT_TYPE_EXT);
        if (const auto* variant = dynamic_cast<const VariantProgram*>(&program))
            result.generation = variant->variantGeneration();
        for (unsigned int i = 0; i < program.getNumTransformFeedBackVaryings(); ++i)
            result.feedback.push_back(program.getTransformFeedBackVarying(i));
        for (unsigned int i = 0; i < program.getNumShaders(); ++i)
        {
            const auto* shader = program.getShader(i);
            if (!shader || shader->getShaderSource().size() > 2u * 1024u * 1024u - result.sourceBytes)
                return std::nullopt;
            result.sourceBytes += shader->getShaderSource().size();
            result.sources.push_back({ shader, shader->getShaderSource(), shader->getShaderDefines(),
                shader->getShaderRequirements(), shader->getType(), shader->getShaderDefinesMode() });
        }
        return result;
    }

    bool ProgramLinkIdentity::matches(const osg::Program& program) const
    {
        if ((program.getProgramBinary() && program.getProgramBinary()->getSize())
            || program.getNumShaders() != sources.size() || attributes != program.getAttribBindingList()
            || fragments != program.getFragDataBindingList() || blocks != program.getUniformBlockBindingList()
            || defines != program.getShaderDefines() || feedbackMode != program.getTransformFeedBackMode()
            || geometryVertices != program.getParameter(GL_GEOMETRY_VERTICES_OUT_EXT)
            || geometryInput != program.getParameter(GL_GEOMETRY_INPUT_TYPE_EXT)
            || geometryOutput != program.getParameter(GL_GEOMETRY_OUTPUT_TYPE_EXT)
            || feedback.size() != program.getNumTransformFeedBackVaryings())
            return false;
        if (const auto* variant = dynamic_cast<const VariantProgram*>(&program);
            variant && generation != variant->variantGeneration())
            return false;
        for (unsigned int i = 0; i < sources.size(); ++i)
        {
            const auto& source = sources[i];
            const auto* shader = program.getShader(i);
            if (shader != source.shader.get() || shader->getShaderSource() != source.text
                || shader->getType() != source.type || shader->getShaderDefinesMode() != source.mode
                || shader->getShaderDefines() != source.defines || shader->getShaderRequirements() != source.requirements)
                return false;
        }
        for (unsigned int i = 0; i < feedback.size(); ++i)
            if (feedback[i] != program.getTransformFeedBackVarying(i))
                return false;
        return true;
    }

    ProgramDefineSnapshot ProgramDefineSnapshot::capture(osg::State& state, const osg::Program& program)
    {
        ProgramDefineSnapshot result;
        if (!ProgramLinkIdentity::boundedCaptureSize(program))
        {
            result.bounded = false;
            return result;
        }
        std::size_t metadata = 0;
        for (const auto& name : program.getShaderDefines())
        {
            const osg::StateSet::DefinePair* value = nullptr;
            const auto& map = state.getDefineMap();
            if (map.changed)
            {
                const auto found = map.map.find(name);
                if (found != map.map.end() && !found->second.defineVec.empty()
                    && (found->second.defineVec.back().second & osg::StateAttribute::ON))
                    value = &found->second.defineVec.back();
            }
            else if (const auto found = map.currentDefines.find(name); found != map.currentDefines.end())
                value = &found->second;
            if (value)
            {
                if (value->first.size() > ProgramLinkIdentity::sMaxValueBytes)
                {
                    result.bounded = false;
                    return result;
                }
                const auto bytes = name.size() + value->first.size();
                if (bytes > ProgramLinkIdentity::sMaxMetadataBytes - metadata)
                {
                    result.bounded = false;
                    return result;
                }
                metadata += bytes;
            }
        }
        result.string = state.getDefineString(program.getShaderDefines());
        // Only the defines imported/required by this Program affect its PCP.
        osg::ref_ptr<osg::State> resolved = new osg::State;
        osg::ref_ptr<osg::StateSet> values = new osg::StateSet;
        const auto& active = state.getDefineMap().currentDefines;
        for (const auto& name : program.getShaderDefines())
            if (const auto found = active.find(name); found != active.end())
                values->setDefine(name, found->second.first, osg::StateAttribute::ON);
        resolved->pushStateSet(values);
        resolved->getDefineString(program.getShaderDefines());
        result.map = resolved->getDefineMap();
        return result;
    }

    std::optional<std::size_t> ProgramDefineSnapshot::boundedPathMetadata(const osg::Program& program,
        const std::vector<const osg::StateSet*>& path)
    {
        if (path.size() > 64 || !ProgramLinkIdentity::boundedCaptureSize(program)) return {};
        std::size_t metadata = 0, entries = 0;
        for (const auto* source : path)
        {
            if (!source) continue;
            const auto& values = source->getDefineList();
            if (values.size() > 512 || values.size() > 1024 - entries) return {};
            entries += values.size();
            for (const auto& [name, value] : values)
            {
                const auto bytes = name.size() + value.first.size();
                if (name.size() > ProgramLinkIdentity::sMaxNameBytes
                    || value.first.size() > ProgramLinkIdentity::sMaxValueBytes
                    || bytes > ProgramLinkIdentity::sMaxMetadataBytes - metadata)
                    return {};
                metadata += bytes;
            }
        }
        return metadata;
    }

    ProgramDefineSnapshot ProgramDefineSnapshot::fromStateSets(const osg::Program& program,
        const std::vector<const osg::StateSet*>& path)
    {
        if (!boundedPathMetadata(program, path))
        {
            ProgramDefineSnapshot refused;
            refused.bounded = false;
            return refused;
        }
        osg::ref_ptr<osg::State> state = new osg::State;
        std::vector<osg::ref_ptr<osg::StateSet>> values;
        for (const auto* source : path)
        {
            if (!source)
                continue;
            auto copy = new osg::StateSet;
            copy->setDefineList(source->getDefineList());
            values.emplace_back(copy);
            state->pushStateSet(copy);
        }
        return capture(*state, program);
    }

    ProgramDefineScope::ProgramDefineScope(osg::State& state, const ProgramDefineSnapshot& snapshot)
        : mState(state)
        , mPrevious(snapshot.map)
    {
        // Copy the replacement before touching the live define stacks. If an
        // allocation fails, construction never removes the original state.
        // The commit/restoration swaps below allocate nothing.
        auto& live = mState.getDefineMap();
        mPrevious.map.swap(live.map);
        mPrevious.currentDefines.swap(live.currentDefines);
        std::swap(mPrevious.changed, live.changed);
    }

    ProgramDefineScope::~ProgramDefineScope()
    {
        auto& live = mState.getDefineMap();
        mPrevious.map.swap(live.map);
        mPrevious.currentDefines.swap(live.currentDefines);
        std::swap(mPrevious.changed, live.changed);
    }

    struct VariantProgram::Prepared
    {
        struct Receipt
        {
            osg::observer_ptr<osg::GraphicsContext> context;
            unsigned int contextId = 0;
            osg::ref_ptr<osg::Program::PerContextProgram> pcp;
            GLuint handle = 0;
            std::string defines;
            osg::Program::AttribBindingList aliases;
            bool aliasing = false;
            bool builtinUniforms = false;
            std::uint64_t generation = 0, receipt = 0, linkEpoch = 0;
            bool consumed = false;

            bool matchesOwner(osg::State& state, const osg::Program::PerContextProgram& live,
                std::uint64_t programGeneration) const
            {
                return context.valid() && context.get() == state.getGraphicsContext()
                    && contextId == state.getContextID() && pcp.get() == &live
                    && handle == live.getHandle() && generation == programGeneration;
            }

            bool matches(osg::State& state, const osg::Program::PerContextProgram& live,
                std::uint64_t programGeneration) const
            {
                return matchesOwner(state, live, programGeneration) && defines == live.getDefineString()
                    && aliasing == state.getUseVertexAttributeAliasing()
                    && builtinUniforms == state.getUseModelViewAndProjectionUniforms()
                    && aliases == state.getAttributeBindingList();
            }
        };
        std::mutex mutex;
        std::vector<Receipt> receipts;
        std::uint64_t nextLinkEpoch = 1;
    };

    VariantProgram::VariantProgram()
    {
        if (exactProgramVariantsEnabled())
            mPrepared = std::make_unique<Prepared>();
    }
    VariantProgram::VariantProgram(const osg::Program& other, const osg::CopyOp& copyop)
        : osg::Program(other, copyop)
    {
        if (exactProgramVariantsEnabled())
            mPrepared = std::make_unique<Prepared>();
    }
    VariantProgram::VariantProgram(const VariantProgram& other, const osg::CopyOp& copyop)
        : VariantProgram(static_cast<const osg::Program&>(other), copyop)
    {
    }
    VariantProgram::~VariantProgram() = default;

    std::uint64_t VariantProgram::variantGeneration() const
    {
        return mVariantGeneration.load(std::memory_order_acquire);
    }

    void VariantProgram::invalidateVariantPreparations() const
    {
        mVariantGeneration.fetch_add(1, std::memory_order_acq_rel);
        if (mPrepared)
        {
            std::lock_guard lock(mPrepared->mutex);
            mPrepared->receipts.clear();
        }
    }

    void VariantProgram::releaseGLObjects(osg::State* state) const
    {
        // Do not preserve handles across OSG release/context teardown. The epoch
        // also invalidates CPU jobs already captured against the old context.
        invalidateVariantPreparations();
        osg::Program::releaseGLObjects(state);
    }

    void VariantProgram::notePrepared(osg::State& state, std::uint64_t receipt, bool relinked) const
    {
        auto* context = state.getGraphicsContext();
        auto* pcp = getPCP(state);
        if (!context || !context->isCurrent() || !pcp || !pcp->getHandle()
            || pcp->needsLink() || !pcp->isLinked())
            return;
        if (!mPrepared)
            return;
        std::lock_guard lock(mPrepared->mutex);
        auto& entries = mPrepared->receipts;
        const auto found = std::find_if(entries.begin(), entries.end(),
            [&](const auto& old) { return old.pcp.get() == pcp; });
        // Small receipt bound; OSG itself owns the canonical PCP lifetime.
        const bool existing = found != entries.end();
        if (!existing && entries.size() >= 8)
            entries.erase(entries.begin());
        const bool sameLink = existing && !relinked && found->matches(state, *pcp, variantGeneration());
        Prepared::Receipt value{ context, state.getContextID(), pcp, pcp->getHandle(), pcp->getDefineString(),
            state.getAttributeBindingList(), state.getUseVertexAttributeAliasing(),
            state.getUseModelViewAndProjectionUniforms(), variantGeneration(), receipt,
            sameLink ? found->linkEpoch : mPrepared->nextLinkEpoch++, sameLink && found->consumed };
        if (!existing)
            entries.push_back(std::move(value));
        else
            *found = std::move(value);
    }

    std::uint64_t VariantProgram::preparedLinkEpoch(osg::State& state,
        const osg::Program::PerContextProgram& pcp) const
    {
        if (!mPrepared || !state.getGraphicsContext() || !state.getGraphicsContext()->isCurrent()
            || pcp.needsLink() || !pcp.isLinked())
            return 0;
        std::lock_guard lock(mPrepared->mutex);
        for (const auto& entry : mPrepared->receipts)
            // The steady draw path reads an epoch from at most eight owner
            // tuples. Text/alias identity remains a first-use acceptance check.
            if (entry.matchesOwner(state, pcp, variantGeneration()))
                return entry.linkEpoch;
        return 0;
    }

    PreparedVariantUse VariantProgram::takePreparedFirstUse(osg::State& state,
        const osg::Program::PerContextProgram& pcp) const
    {
        if (!mPrepared || !state.getGraphicsContext() || !state.getGraphicsContext()->isCurrent()
            || pcp.needsLink() || !pcp.isLinked())
            return {};
        std::lock_guard lock(mPrepared->mutex);
        for (auto& entry : mPrepared->receipts)
            if (entry.matches(state, pcp, variantGeneration()))
            {
                if (entry.consumed)
                {
                    programVariantStats().reuseUses.fetch_add(1, std::memory_order_relaxed);
                    return { false, entry.receipt, entry.linkEpoch, true };
                }
                entry.consumed = true;
                programVariantStats().firstUses.fetch_add(1, std::memory_order_relaxed);
                return { true, entry.receipt, entry.linkEpoch };
            }
        return {};
    }

    bool compileExactProgramVariant(osg::Program& program, osg::State& state, std::uint64_t receipt)
    {
        if (!exactProgramVariantsEnabled())
        {
            program.compileGLObjects(state);
            return false;
        }
        auto* variant = dynamic_cast<VariantProgram*>(&program);
        auto* context = state.getGraphicsContext();
        if (!variant || !context || !context->isCurrent() || program.isFixedFunction())
        {
            programVariantStats().unsupported.fetch_add(1, std::memory_order_relaxed);
            program.compileGLObjects(state);
            return false;
        }
        auto* before = program.getPCP(state);
        const bool reusable = before && before->isLinked() && !before->needsLink();
        Debug::HitchTrace::Scope work("shader.variant_prepare", Debug::HitchTrace::CpuTiming::WallEnvelope);
        work.setLink(receipt);
        work.setMetric(0, "context", state.getContextID());
        work.setMetric(1, "already_linked", reusable ? 1 : 0);
        if (!reusable)
            program.compileGLObjects(state);
        auto* pcp = program.getPCP(state);
        if (!pcp || !pcp->getHandle() || !pcp->isLinked() || pcp->needsLink())
        {
            programVariantStats().failed.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        // LINK_STATUS completion in OSG, not merely request submission. No claim
        // is made that later glUseProgram/drawing cannot stall the driver.
        variant->notePrepared(state, receipt ? receipt : work.id(), !reusable);
        programVariantStats().prepared.fetch_add(1, std::memory_order_relaxed);
        if (reusable)
            programVariantStats().reused.fetch_add(1, std::memory_order_relaxed);
        work.setMetric(2, "program_handle", pcp->getHandle());
        work.retain();
        return true;
    }
}
