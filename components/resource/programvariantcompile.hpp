#ifndef OPENMW_COMPONENTS_RESOURCE_PROGRAMVARIANTCOMPILE_H
#define OPENMW_COMPONENTS_RESOURCE_PROGRAMVARIANTCOMPILE_H

#include "v321classifiedcompileset.hpp"

#include <components/debug/hitchtrace.hpp>
#include <components/shader/programvariant.hpp>

#include <osg/Camera>
#include <osg/Drawable>
#include <osg/NodeVisitor>
#include <osgUtil/RenderBin>

#include <algorithm>
#include <atomic>
#include <set>
#include <vector>

namespace Resource
{
    inline bool knownProgramProducerDefines(const osg::Program& program,
        const Shader::ProgramDefineSnapshot& snapshot)
    {
        if (!snapshot.bounded) return false;
        for (const auto& name : program.getShaderDefines())
            if (name != "FORCE_OPAQUE" && name != "DISTORTION"
                && snapshot.map.currentDefines.find(name) == snapshot.map.currentDefines.end())
                return false;
        return true;
    }

    struct ExactProgramVariantRequest
    {
        inline static std::atomic<std::size_t> capturedSourceBytes{ 0 };
        inline static std::atomic<std::size_t> capturedMetadataBytes{ 0 }, liveRequests{ 0 };
        static constexpr std::size_t sMaxCapturedSources = 8u * 1024u * 1024u;
        static constexpr std::size_t sMaxCapturedMetadata = 4u * 1024u * 1024u, sMaxLiveRequests = 512;
        osg::ref_ptr<osg::Program> program;
        osg::observer_ptr<osg::GraphicsContext> context;
        unsigned int contextId = 0;
        Shader::ProgramDefineSnapshot defines;
        Shader::ProgramLinkIdentity identity;
        std::shared_ptr<V321ResourceCompileLifetime> lifetime;
        std::size_t chargedSources = 0;
        std::size_t chargedMetadata = 0;
        bool chargedRequest = false;
        std::uint64_t receipt = 0;

        ~ExactProgramVariantRequest()
        {
            // The cap bounds copied shader text in this queue, not total RAM,
            // driver allocations, PCP bytes, or physical pages returned.
            program = nullptr;
            identity.sources.clear();
            if (chargedSources)
                capturedSourceBytes.fetch_sub(chargedSources, std::memory_order_acq_rel);
            if (chargedMetadata)
                capturedMetadataBytes.fetch_sub(chargedMetadata, std::memory_order_acq_rel);
            if (chargedRequest)
                liveRequests.fetch_sub(1, std::memory_order_acq_rel);
        }

        static bool reserve(std::atomic<std::size_t>& counter, std::size_t amount, std::size_t maximum)
        {
            auto used = counter.load(std::memory_order_acquire);
            do
            {
                if (amount > maximum - (std::min)(used, maximum)) return false;
            } while (!counter.compare_exchange_weak(used, used + amount, std::memory_order_acq_rel));
            return true;
        }

        static std::shared_ptr<ExactProgramVariantRequest> create(osg::GraphicsContext* context,
            osg::Program* program, const std::vector<const osg::StateSet*>& path,
            std::shared_ptr<V321ResourceCompileLifetime> lifetime = {})
        try
        {
            if (!Shader::exactProgramVariantsEnabled() || !context || !context->getState()
                || !dynamic_cast<Shader::VariantProgram*>(program) || program->isFixedFunction())
                return {};
            const auto size = Shader::ProgramLinkIdentity::boundedCaptureSize(*program);
            const auto pathBytes = Shader::ProgramDefineSnapshot::boundedPathMetadata(*program, path);
            if (!size || !pathBytes) return {};
            // Bounds cover text payload and cardinality, not STL allocator,
            // driver/PCP storage or physical pages. Reserve before copying any
            // shader text, define stacks, bindings or optional CPU snapshots.
            const auto bytes = size->sources;
            const auto metadata = size->metadata + *pathBytes * 3;
            auto result = std::make_shared<ExactProgramVariantRequest>();
            if (!reserve(liveRequests, 1, sMaxLiveRequests)) return {};
            result->chargedRequest = true;
            if (!reserve(capturedSourceBytes, bytes, sMaxCapturedSources)) return {};
            result->chargedSources = bytes;
            if (!reserve(capturedMetadataBytes, metadata, sMaxCapturedMetadata)) return {};
            result->chargedMetadata = metadata;
            auto defines = Shader::ProgramDefineSnapshot::fromStateSets(*program, path);
            if (!knownProgramProducerDefines(*program, defines))
            {
                Shader::programVariantStats().unsupported.fetch_add(1, std::memory_order_relaxed);
                return {};
            }
            auto identity = Shader::ProgramLinkIdentity::capture(*program);
            if (!identity)
                return {};
            result->identity = std::move(*identity);
            result->program = program;
            result->context = context;
            result->contextId = context->getState()->getContextID();
            result->defines = std::move(defines);
            result->lifetime = std::move(lifetime);
            Debug::HitchTrace::Scope admit("shader.variant_request", Debug::HitchTrace::CpuTiming::WallEnvelope);
            admit.setMetric(0, "source_bytes", bytes);
            admit.setMetric(1, "context", result->contextId);
            admit.retain();
            result->receipt = admit.id();
            Shader::programVariantStats().requests.fetch_add(1, std::memory_order_relaxed);
            return result;
        }
        catch (const std::bad_alloc&)
        {
            // Optional producer preparation cannot make allocation failure
            // discard the ordinary consumer/ICO compile path.
            Shader::programVariantStats().unsupported.fetch_add(1, std::memory_order_relaxed);
            return {};
        }
    };

    class ExactProgramVariantCompileOp final : public osgUtil::IncrementalCompileOperation::CompileProgramOp
    {
    public:
        explicit ExactProgramVariantCompileOp(std::shared_ptr<ExactProgramVariantRequest> request)
            : CompileProgramOp(request->program)
            , mRequest(std::move(request))
        {
        }
        bool compile(osgUtil::IncrementalCompileOperation::CompileInfo& info) override
        {
            auto* state = info.getState();
            if (!state || !mRequest->context.valid() || mRequest->context.get() != state->getGraphicsContext()
                || mRequest->contextId != state->getContextID() || !state->getGraphicsContext()->isCurrent()
                || (mRequest->lifetime && mRequest->lifetime->cancelled())
                || !mRequest->identity.matches(*mRequest->program))
            {
                Shader::programVariantStats().stale.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
            Shader::ProgramDefineScope defines(*state, mRequest->defines);
            if (state->getDefineString(mRequest->program->getShaderDefines()) != mRequest->defines.string)
            {
                Shader::programVariantStats().stale.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
            Shader::compileExactProgramVariant(*mRequest->program, *state, mRequest->receipt);
            return true;
        }
    private:
        std::shared_ptr<ExactProgramVariantRequest> mRequest;
    };

    inline osg::ref_ptr<osgUtil::IncrementalCompileOperation::CompileOp> exactProgramCompileOp(
        osg::GraphicsContext* context, osg::Program* program, const std::vector<const osg::StateSet*>& path,
        std::shared_ptr<V321ResourceCompileLifetime> lifetime = {})
    {
        auto request = ExactProgramVariantRequest::create(context, program, path, std::move(lifetime));
        return request ? new ExactProgramVariantCompileOp(std::move(request)) : nullptr;
    }

    // Invoked only before this producer-owned CompileSet enters ICO. The visitor
    // reads value state and Programs; it does not invoke callbacks or cull, and
    // no graph, Drawable, StateSet, borrowed visitor or pass owner enters a job.
    inline void installExactProgramVariants(osgUtil::IncrementalCompileOperation::CompileSet& set,
        osg::Node& node, const osgUtil::IncrementalCompileOperation::ContextSet& contexts)
    {
        if (!Shader::exactProgramVariantsEnabled() || contexts.size() != 1)
            return;
        auto* context = *contexts.begin();
        struct Collector final : osg::NodeVisitor
        {
            explicit Collector(osg::GraphicsContext* context)
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN), context(context), state(new osg::State)
            {
                setNodeMaskOverride(~0u);
            }
            osg::GraphicsContext* context;
            osg::ref_ptr<osg::State> state;
            std::vector<const osg::StateSet*> path;
            std::vector<std::shared_ptr<ExactProgramVariantRequest>> requests;
            std::set<std::pair<const osg::Program*, std::string>> seen;
            unsigned int visited = 0;

            void collect()
            {
                const auto found = state->getAttributeMap().find({ osg::StateAttribute::PROGRAM, 0 });
                if (found == state->getAttributeMap().end() || found->second.attributeVec.empty())
                    return;
                auto* program = const_cast<osg::Program*>(
                    dynamic_cast<const osg::Program*>(found->second.attributeVec.back().first));
                if (!program)
                    return;
                // Bound attempted identities too: global admission refusal
                // must not turn this local dedup set into an unbounded queue.
                if (seen.size() >= 64)
                    return;
                // The exact known Distortion bin is an owned startup producer
                // state. Preview FORCE_OPAQUE and unknown custom bins are not
                // guessed; consumption validates and takes ordinary fallback.
                auto definePath = path;
                for (auto it = path.rbegin(); it != path.rend(); ++it)
                    if ((*it)->getRenderBinMode() != osg::StateSet::INHERIT_RENDERBIN_DETAILS)
                    {
                        if ((*it)->getBinName() == "Distortion")
                            if (auto* bin = osgUtil::RenderBin::getRenderBinPrototype("Distortion"))
                                if (bin->getStateSet())
                                    definePath.insert(definePath.begin(), bin->getStateSet());
                        break;
                    }
                auto snapshot = Shader::ProgramDefineSnapshot::fromStateSets(*program, definePath);
                if (!snapshot.bounded || !seen.emplace(program, snapshot.string).second)
                    return;
                if (auto request = ExactProgramVariantRequest::create(context, program, definePath))
                    requests.push_back(std::move(request));
            }
            void apply(osg::Node& node) override
            {
                if (++visited > 4096 || dynamic_cast<osg::Camera*>(&node))
                    return;
                auto* values = node.getStateSet();
                if (values)
                {
                    path.push_back(values);
                    state->pushStateSet(values);
                    collect();
                }
                traverse(node);
                if (values)
                {
                    state->popStateSet();
                    path.pop_back();
                }
            }
            void apply(osg::Drawable& drawable) override
            {
                if (++visited > 4096)
                    return;
                auto* values = drawable.getStateSet();
                if (values)
                {
                    path.push_back(values);
                    state->pushStateSet(values);
                }
                collect();
                if (values)
                {
                    state->popStateSet();
                    path.pop_back();
                }
            }
        } collector(context);
        node.accept(collector);
        if (collector.requests.empty())
            return;
        std::set<const osg::Program*> replaced;
        osgUtil::IncrementalCompileOperation::CompileList prepared;
        for (auto& request : collector.requests)
        {
            replaced.insert(request->program.get());
            prepared.add(new ExactProgramVariantCompileOp(std::move(request)));
        }
        const auto existing = set._compileMap.find(context);
        if (existing == set._compileMap.end())
            ++set._numberCompileListsToCompile;
        auto& list = set._compileMap[context];
        for (const auto& op : list._compileOps)
        {
            const auto* program = dynamic_cast<const osgUtil::IncrementalCompileOperation::CompileProgramOp*>(op.get());
            if (!program || replaced.find(program->_program.get()) == replaced.end())
                prepared.add(op.get());
        }
        list._compileOps.swap(prepared._compileOps);
    }

    // Small resource-only lifetime; callers retain/cancel it at the material's
    // update/teardown seams. One context only; unknown context ownership skips.
    inline std::shared_ptr<V321ResourceCompileLifetime> prepareExactProgram(
        osgUtil::IncrementalCompileOperation* ico, osg::Program* program,
        const std::vector<const osg::StateSet*>& path)
    {
        if (!ico || !Shader::exactProgramVariantsEnabled() || ico->getContextSet().size() != 1)
            return {};
        auto lifetime = std::make_shared<V321ResourceCompileLifetime>();
        auto* context = *ico->getContextSet().begin();
        auto op = exactProgramCompileOp(context, program, path, lifetime);
        if (!op)
            return {};
        auto set = new V321ClassifiedCompileSet(lifetime, V321CompileClass::GenericModel, V321CompileUrgency::NearFuture);
        set->_compileMap[context].add(op.get());
        ++set->_numberCompileListsToCompile;
        struct Completed final : osgUtil::IncrementalCompileOperation::CompileCompletedCallback
        {
            explicit Completed(std::shared_ptr<V321ResourceCompileLifetime> owner) : owner(std::move(owner)) {}
            std::shared_ptr<V321ResourceCompileLifetime> owner;
            bool compileCompleted(osgUtil::IncrementalCompileOperation::CompileSet*) override
            {
                owner->complete();
                return true;
            }
        };
        set->_compileCompletedCallback = new Completed(lifetime);
        ico->add(set, false);
        return lifetime;
    }
}

#endif
