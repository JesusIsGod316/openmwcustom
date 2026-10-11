#include "compositemaprenderer.hpp"

#include <osg/FrameBufferObject>
#include <osg/RenderInfo>
#include <osg/Texture2D>
#include <osg/Program>
#include <components/shader/programvariant.hpp>
#include <osg/FrameStamp>
#include <osg/observer_ptr>

#include <components/debug/v3diagnostics.hpp>
#include <components/debug/hitchtrace.hpp>
#include <components/debug/debuglog.hpp>
#include <components/resource/openmwcompileoperation.hpp>
#include <components/resource/preparedterraintexture.hpp>
#include <components/resource/v321classifiedcompileset.hpp>
#include <components/sceneutil/bufferpreparationscope.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string_view>
#include <unordered_set>

namespace Terrain
{
    struct CompositeMetadataLease;
    // This bounds additional queued preparation ownership, not measured GPU
    // residency or new OS allocation. Cancelled queued owners keep their debit
    // until ICO actually releases the last operation; cancellation is no refund.
    struct CompositeStepBudget
    {
        struct Debt
        {
            std::shared_ptr<CompositeMetadataLease> metadata;
            std::vector<osg::observer_ptr<osg::Texture::TextureObject>> textures;
            std::vector<osg::observer_ptr<osg::Program::PerContextProgram>> programs;
            std::size_t bytes = 0;
        };
        static constexpr std::size_t MaximumBytes = 32u * 1024u * 1024u;
        std::atomic_size_t bytes{0}, maps{0};
        std::atomic_uint64_t creditGeneration{1};
        std::mutex debtMutex;
        std::vector<Debt> debts;
        std::atomic_bool debtFailed{false};
        std::array<std::atomic_uint64_t, 10> pressureRefusals{};
        std::array<std::atomic_uint64_t, 5> pressureStates{};
        std::atomic_uint64_t pressureRefusalChecks{0};
        std::atomic_uint64_t pressureUnreportedState{0};
        bool reserve(std::size_t amount)
        {
            auto current = bytes.load(std::memory_order_relaxed);
            while (amount <= MaximumBytes && current <= MaximumBytes - amount)
                if (bytes.compare_exchange_weak(current, current + amount, std::memory_order_acq_rel)) return true;
            return false;
        }
        bool canReserve(std::size_t amount) const
        {
            const auto current = bytes.load(std::memory_order_acquire);
            return amount <= MaximumBytes && current <= MaximumBytes - amount;
        }
        void release(std::size_t amount)
        {
            bytes.fetch_sub(amount, std::memory_order_acq_rel);
            creditGeneration.fetch_add(1, std::memory_order_release);
        }
        void pruneDebt()
        {
            std::lock_guard lock(debtMutex);
            for (auto it = debts.begin(); it != debts.end();)
            {
                std::erase_if(it->textures, [](const auto& object) { return !object.valid(); });
                std::erase_if(it->programs, [](const auto& object) { return !object.valid(); });
                if (it->textures.empty() && it->programs.empty())
                { release(it->bytes); it = debts.erase(it); }
                else ++it;
            }
        }
        bool retainDebt(Debt debt) noexcept
        {
            if (debt.textures.empty() && debt.programs.empty()) return false;
            std::lock_guard lock(debtMutex);
            try
            {
#ifdef P9_COMPILE_TRACE_TEST_HOOKS
                if (const auto* value = std::getenv("P9_COMPOSITE_DEBT_FAILURE");
                    value && std::string_view(value) == "1") throw std::bad_alloc();
#endif
                if (debts.size() >= 16) { debtFailed.store(true, std::memory_order_release); return true; }
                debts.push_back(std::move(debt));
            }
            catch (...) { debtFailed.store(true, std::memory_order_release); }
            // Any bookkeeping failure strands conservative credit and disables
            // new speculation. Required rendering never depends on this ledger.
            return true;
        }
        bool accepting() const { return !debtFailed.load(std::memory_order_acquire); }
        void consumed(const std::vector<osg::ref_ptr<osg::Texture::TextureObject>>& textures,
            const std::vector<osg::ref_ptr<osg::Program::PerContextProgram>>& programs)
        {
            std::lock_guard lock(debtMutex);
            for (auto it = debts.begin(); it != debts.end();)
            {
                std::erase_if(it->textures, [&](const auto& object) {
                    osg::ref_ptr<osg::Texture::TextureObject> owner;
                    return !object.lock(owner) || std::any_of(textures.begin(), textures.end(),
                        [&](const auto& used) { return used == owner; }); });
                std::erase_if(it->programs, [&](const auto& object) {
                    osg::ref_ptr<osg::Program::PerContextProgram> owner;
                    return !object.lock(owner) || std::any_of(programs.begin(), programs.end(),
                        [&](const auto& used) { return used == owner; }); });
                if (it->textures.empty() && it->programs.empty())
                { release(it->bytes); it = debts.erase(it); }
                else ++it;
            }
        }
    };

    struct CompositeMetadataLease
    {
        std::weak_ptr<CompositeStepBudget> budget;
        std::size_t bytes = 0;
        ~CompositeMetadataLease()
        {
            if (auto owner = budget.lock())
            { owner->maps.fetch_sub(1, std::memory_order_acq_rel); owner->release(bytes); }
        }
    };

    struct CompositeDependencyWave
    {
        std::shared_ptr<CompositeStepBudget> budget;
        std::shared_ptr<CompositeMetadataLease> metadata;
        std::vector<osg::observer_ptr<osg::Texture::TextureObject>> newTextureObjects;
        std::vector<osg::observer_ptr<osg::Program::PerContextProgram>> newPrograms;
        // No StateSet/map ownership: a pass would secretly retain all its other
        // textures. Only this admitted wave pins exact immutable payloads.
        std::vector<osg::ref_ptr<Resource::PreparedTerrainTexture>> textures;
        std::vector<osg::ref_ptr<osg::Image>> images;
        std::vector<osg::ref_ptr<osg::Program>> programs;
        std::shared_ptr<Resource::V321ResourceCompileLifetime> lifetime
            = std::make_shared<Resource::V321ResourceCompileLifetime>();
        std::size_t bytes = 0, begin = 0, end = 0, layerBegin = 0, layerEnd = 0;
        std::uint64_t receipt = 0;
        bool terminal = false, completed = false, consumed = false;
        ~CompositeDependencyWave()
        {
            // Payloads release before their conservative admission debit.
            textures.clear(); programs.clear(); images.clear();
            if (budget && (consumed || !budget->retainDebt(
                {metadata, std::move(newTextureObjects), std::move(newPrograms), bytes})))
                budget->release(bytes);
        }
    };

    struct CompositeStepPreparation
    {
        struct Dependency
        {
            osg::observer_ptr<Resource::PreparedTerrainTexture> texture;
            osg::observer_ptr<osg::Image> image;
            osg::observer_ptr<osg::Program> program;
            osg::observer_ptr<osg::StateSet> pass;
            osg::observer_ptr<osg::Drawable> drawable;
            osg::observer_ptr<osg::Program::PerContextProgram> submittedProgram;
            unsigned unit = 0, imageRevision = 0;
            GLuint programName = 0;
            std::size_t sourceBytes = 0, retainedBytes = 0;
            std::size_t layer = 0;
        };
        std::shared_ptr<CompositeStepBudget> budget;
        std::shared_ptr<CompositeMetadataLease> metadata;
        std::vector<Dependency> dependencies;
        std::shared_ptr<CompositeDependencyWave> wave;
        std::shared_ptr<CompositeDependencyWave> targetDebt;
        Resource::SpeculativeScope::Context pressure;
        std::size_t cursor = 0, metadataBytes = 0;
        std::array<int, 3> targetShape{};
        ~CompositeStepPreparation()
        {
            wave.reset(); targetDebt.reset(); dependencies.clear();
            metadata.reset();
        }
    };

    struct CompositePreparation
    {
        struct Dependency
        {
            osg::ref_ptr<Resource::PreparedTerrainTexture> texture;
            osg::ref_ptr<osg::Program> program;
            osg::ref_ptr<osg::StateSet> pass;
            osg::observer_ptr<osg::Drawable> drawable;
            unsigned unit = 0;
            osg::observer_ptr<osg::Program::PerContextProgram> submittedProgram;
            GLuint programName = 0;
        };
        std::vector<Dependency> dependencies;
        std::shared_ptr<CompositeStepPreparation> step;
        osg::ref_ptr<osg::FrameBufferObject> fbo = new osg::FrameBufferObject;
        osg::observer_ptr<osg::Texture2D> target;
        osg::observer_ptr<osg::GraphicsContext> context;
        std::size_t bytes = 0;
        std::uint64_t generation = 0;
        std::uint64_t receipt = 0;
        std::shared_ptr<Resource::V321ResourceCompileLifetime> lifetime
            = std::make_shared<Resource::V321ResourceCompileLifetime>();
        bool completed = false, failed = false;
        bool terminalReported = false;
        std::string_view terminalReason = "pending";
        GLuint fboName = 0;
        // Draw-context progress, including target allocation and FBO. This is
        // submission debt, not a promise that later GL calls cannot stall.
        std::array<std::uint64_t, 9> submitted{};
        void markSubmitted(std::size_t index) { submitted[index / 64] |= std::uint64_t(1) << (index % 64); }
        std::size_t submittedCount() const
        {
            std::size_t result = 0;
            for (auto word : submitted) result += std::popcount(word);
            return result;
        }
    };

    namespace
    {
        std::uint64_t progressTick()
        {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
        }
        void reportFirstDemand(CompositeMap& map)
        {
            const auto now = progressTick();
            const auto generation = map.mProgressReceiptGeneration.load(std::memory_order_acquire);
            const auto admitted = map.mProgressAdmittedTick.load(std::memory_order_acquire);
            const auto first = map.mProgressFirstLayerTick.load(std::memory_order_acquire);
            const auto complete = map.mProgressCompletedTick.load(std::memory_order_acquire);
            const bool coherent = generation != 0
                && generation == map.mProgressReceiptGeneration.load(std::memory_order_acquire);
            Debug::HitchTrace::Scope receipt("composite.first_demand", Debug::HitchTrace::CpuTiming::WallEnvelope);
            receipt.retain(); receipt.setLink(map.mPreparationReceipt);
            constexpr auto unknown = std::numeric_limits<std::uint64_t>::max();
            receipt.setMetric(0, "admit_to_demand_us", coherent && admitted && now >= admitted ? (now - admitted) / 1000 : unknown);
            receipt.setMetric(1, "first_layer_lead_us", coherent && first && now >= first ? (now - first) / 1000 : unknown);
            receipt.setMetric(2, "complete_lead_us", coherent && complete && now >= complete ? (now - complete) / 1000 : unknown);
            receipt.setMetric(3, "generation", coherent ? generation : 0);
        }
        void reportProgressTerminal(CompositeMap& map, std::string_view reason)
        {
            const auto& preparation = map.mPreparation;
            if (!preparation || preparation->terminalReported) return;
            preparation->terminalReported = true;
            Debug::HitchTrace::Scope receipt("composite.progress_terminal",
                Debug::HitchTrace::CpuTiming::WallEnvelope, reason);
            receipt.retain();
            receipt.setLink(preparation->receipt);
            receipt.setMetric(0, "generation", preparation->generation);
            receipt.setMetric(1, "compiled_layers", map.mCompiled);
            receipt.setMetric(2, "submitted", preparation->submittedCount());
            receipt.setMetric(3, "required", map.mRequired.load(std::memory_order_acquire));
        }
        bool retainedDependency(const CompositePreparation::Dependency& dep)
        {
            if (!dep.pass || !dep.drawable.valid() || dep.drawable->getStateSet() != dep.pass.get())
                return false;
            if (dep.texture)
                return dep.pass->getTextureAttribute(dep.unit, osg::StateAttribute::TEXTURE) == dep.texture.get();
            return dep.pass->getAttribute(osg::StateAttribute::PROGRAM) == dep.program.get();
        }

        struct LockedStepDependency
        {
            osg::ref_ptr<osg::Drawable> drawable;
            osg::ref_ptr<osg::StateSet> pass;
            osg::ref_ptr<Resource::PreparedTerrainTexture> texture;
            osg::ref_ptr<osg::Image> image;
            osg::ref_ptr<osg::Program> program;
        };
        bool lockStepDependency(const CompositeStepPreparation::Dependency& dep, LockedStepDependency& owner)
        {
            if (!dep.drawable.lock(owner.drawable) || !dep.pass.lock(owner.pass)
                || owner.drawable->getStateSet() != owner.pass.get()) return false;
            if (dep.sourceBytes)
                return dep.texture.lock(owner.texture) && dep.image.lock(owner.image)
                    && owner.pass->getTextureAttribute(dep.unit, osg::StateAttribute::TEXTURE) == owner.texture.get()
                    && owner.texture->getImage() == owner.image.get()
                    && owner.image->getModifiedCount() == dep.imageRevision
                    && owner.image->getTotalDataSize() == dep.sourceBytes;
            return dep.program.lock(owner.program)
                && owner.pass->getAttribute(osg::StateAttribute::PROGRAM) == owner.program.get();
        }

        bool stepPressureUsable(const Resource::SpeculativeScope::Context& pressure)
        {
            // The ResourceSystem callback reads its published cached sample;
            // there is no OS probe or wait for a fresh sample on this path.
            return pressure.sample && Resource::SpeculativeBudget::newOwnerWorkUsable(
                pressure.sample(pressure.owner), Resource::SpeculativePriority::Background);
        }

        void reportPressureEvidence(const CompositePressureEvidence& evidence, std::uint64_t link,
            std::string_view stage)
        {
            if (!Debug::HitchTrace::requested()) return;
            const auto& sample = evidence.sample;
            Debug::HitchTrace::Scope receipt("composite.pressure_sample",
                Debug::HitchTrace::CpuTiming::WallEnvelope, stage);
            receipt.retain(); receipt.setLink(link);
            receipt.setMetric(0, "sample_generation", sample.generation);
            receipt.setMetric(1, "reported_state", evidence.samplerPresent && sample.generation
                ? static_cast<std::uint64_t>(sample.decision.state) : std::numeric_limits<std::uint64_t>::max());
            receipt.setMetric(2, "validity_flags", evidence.validityFlags());
            receipt.setMetric(3, "admissions_per_sample", sample.decision.admissionsPerSample);
            {
                Debug::HitchTrace::Scope physical("composite.pressure_memory",
                    Debug::HitchTrace::CpuTiming::WallEnvelope);
                physical.retain(); physical.setLink(receipt.id());
                physical.setMetric(0, "physical_total", sample.memory.physicalTotal);
                physical.setMetric(1, "physical_available", sample.memory.physicalAvailable);
                physical.setMetric(2, "commit_available", sample.memory.commitAvailable);
                physical.setMetric(3, "system_commit_available", sample.memory.systemCommitAvailable);
            }
            {
                Debug::HitchTrace::Scope predicate("composite.pressure_predicate",
                    Debug::HitchTrace::CpuTiming::WallEnvelope);
                predicate.retain(); predicate.setLink(receipt.id());
                predicate.setMetric(0, "refusal_mask", evidence.refusal);
                predicate.setMetric(1, "sampled_at_ms", sample.sampledAtMs);
                predicate.setMetric(2, "physical_reserve", sample.decision.limits.physicalReserve);
                predicate.setMetric(3, "physical_critical", sample.decision.limits.physicalCritical);
            }
            {
                Debug::HitchTrace::Scope limits("composite.pressure_limits",
                    Debug::HitchTrace::CpuTiming::WallEnvelope);
                limits.retain(); limits.setLink(receipt.id());
                limits.setMetric(0, "commit_reserve", sample.decision.limits.commitReserve);
                limits.setMetric(1, "commit_critical", sample.decision.limits.commitCritical);
                limits.setMetric(2, "physical_recovery", sample.decision.limits.physicalRecovery);
                limits.setMetric(3, "commit_recovery", sample.decision.limits.commitRecovery);
            }
        }

        void recordPressureRefusal(CompositeStepBudget& budget, const CompositePressureEvidence& evidence)
        {
            budget.pressureRefusalChecks.fetch_add(1, std::memory_order_relaxed);
            for (std::size_t bit = 0; bit < budget.pressureRefusals.size(); ++bit)
                if (evidence.refusal & (1u << bit))
                    budget.pressureRefusals[bit].fetch_add(1, std::memory_order_relaxed);
            const auto state = static_cast<std::size_t>(evidence.sample.decision.state);
            if (evidence.samplerPresent && evidence.sample.generation && state < budget.pressureStates.size())
                budget.pressureStates[state].fetch_add(1, std::memory_order_relaxed);
            else budget.pressureUnreportedState.fetch_add(1, std::memory_order_relaxed);
        }

        bool stepTargetUnchanged(const CompositePreparation& preparation, const CompositeMap& map)
        {
            const auto& shape = preparation.step->targetShape;
            return preparation.target.get() == map.mTexture.get()
                && map.mTexture->getTextureWidth() == shape[0] && map.mTexture->getTextureHeight() == shape[1]
                && map.mTexture->getInternalFormat() == shape[2];
        }

        bool consumedStepUnchanged(const CompositePreparation& preparation, const CompositeMap& map, osg::State& state)
        {
            if (preparation.context.get() != state.getGraphicsContext() || !stepTargetUnchanged(preparation, map))
                return false;
            auto* target = dynamic_cast<Resource::PreparedTerrainTexture*>(map.mTexture.get());
            if (map.mCompiled && (!target || !target->preparationUnchanged(state))) return false;
            for (const auto& dep : preparation.step->dependencies)
            {
                if (dep.layer >= map.mCompiled) break;
                LockedStepDependency owner;
                if (!lockStepDependency(dep, owner)) return false;
                if (owner.texture && !owner.texture->preparationUnchanged(state)) return false;
                if (owner.program)
                {
                    state.pushStateSet(owner.pass);
                    auto* pcp = owner.program->getPCP(state);
                    const bool valid = pcp && pcp == dep.submittedProgram.get() && pcp->getHandle() == dep.programName
                        && pcp->isLinked() && !pcp->needsLink();
                    state.popStateSet();
                    if (!valid) return false;
                }
            }
            return true;
        }

        class CompositeStepCompileOp final : public Resource::OpenMWCompositeCompileOp
        {
        public:
            CompositeStepCompileOp(CompositeMap* map, std::shared_ptr<CompositePreparation> preparation,
                std::shared_ptr<CompositeDependencyWave> wave, std::size_t dependency, Kind kind, std::size_t bytes)
                : mMap(map), mPreparation(std::move(preparation)), mWave(std::move(wave)),
                  mDependency(dependency), mKind(kind), mBytes(bytes) {}
            Kind kind() const override { return mKind; }
            std::size_t resourceBytes() const override { return mBytes; }
            bool required() const override
            {
                osg::ref_ptr<CompositeMap> map;
                return mMap.lock(map) && map->mRequired.load(std::memory_order_acquire);
            }
            bool cancelled() const override
            {
                osg::ref_ptr<CompositeMap> map;
                return !mMap.lock(map) || mPreparation->lifetime->cancelled() || mWave->lifetime->cancelled()
                    || map->mPreparationGeneration.load(std::memory_order_acquire) != mPreparation->generation
                    || map->mTexture.get() != mPreparation->target.get();
            }
            bool reusable(const osgUtil::IncrementalCompileOperation::CompileInfo& info) const override
            {
                const auto* ico = dynamic_cast<const Resource::OpenMWIncrementalCompileOperation*>(
                    info.incrementalCompileOperation);
                if (!ico || !ico->progressRepairEnabled()) return false;
                if (cancelled() || sourceDispatchInvalid(info)) return true;
                osg::ref_ptr<CompositeMap> map;
                if (!mMap.lock(map) || !info.getState() || !mPreparation->step
                    || mPreparation->context.get() != info.getState()->getGraphicsContext()
                    || !stepTargetUnchanged(*mPreparation, *map)
                    || !stepPressureUsable(mPreparation->step->pressure)
                    || info.incrementalCompileOperation->getForceTextureDownloadGeometry()) return false;
                const auto& step = *mPreparation->step;
                if (mDependency < step.dependencies.size())
                {
                    LockedStepDependency owner;
                    return lockStepDependency(step.dependencies[mDependency], owner) && owner.texture
                        && owner.texture->preparationUnchanged(*info.getState());
                }
                auto* target = dynamic_cast<Resource::PreparedTerrainTexture*>(map->mTexture.get());
                // Program variants and FBO state need ordinary budgeted compile.
                return mWave->terminal && mKind == Kind::Texture && target
                    && target->preparationUnchanged(*info.getState());
            }
            bool tryCompleteWithoutGL(osgUtil::IncrementalCompileOperation::CompileInfo& info) override
            {
                if (cancelled()) return true;
                if (sourceDispatchInvalid(info))
                {
                    mPreparation->lifetime->cancel(); mWave->lifetime->cancel();
                    return true;
                }
                if (!reusable(info)) return false;
                mPreparation->markSubmitted(mDependency);
                return true;
            }
            bool bootstrapReady(const osgUtil::IncrementalCompileOperation::CompileInfo& info) const override
            {
                osg::ref_ptr<CompositeMap> map;
                if (!mPreparation->step || !info.incrementalCompileOperation
                    || !mWave->terminal || (mKind != Kind::Texture && mKind != Kind::Target)
                    || mDependency < mPreparation->step->dependencies.size() || !mMap.lock(map)
                    || cancelled() || required() || !info.getState()
                    || info.incrementalCompileOperation->getForceTextureDownloadGeometry()
                    || info.incrementalCompileOperation->getContextSet().size() != 1
                    || mPreparation->context.get() != info.getState()->getGraphicsContext()
                    || !stepTargetUnchanged(*mPreparation, *map)
                    || !stepPressureUsable(mPreparation->step->pressure)) return false;
                const auto* target = dynamic_cast<Resource::PreparedTerrainTexture*>(map->mTexture.get());
                return target && (mKind == Kind::Texture || target->preparationUnchanged(*info.getState()));
            }
            double estimatedTimeForCompile(osgUtil::IncrementalCompileOperation::CompileInfo&) const override
            { return mKind == Kind::Program ? .001 : .00025; }
            bool compile(osgUtil::IncrementalCompileOperation::CompileInfo& info) override
            {
                osg::ref_ptr<CompositeMap> map;
                if (!mMap.lock(map) || cancelled()) return true;
                auto& state = *info.getState();
                const auto& step = *mPreparation->step;
                if (info.incrementalCompileOperation->getContextSet().size() != 1
                    || mPreparation->context.get() != state.getGraphicsContext()
                    || !stepTargetUnchanged(*mPreparation, *map))
                {
                    mPreparation->lifetime->cancel(); mWave->lifetime->cancel();
                    return true;
                }
                const auto pressure = CompositePressureEvidence::read(step.pressure);
                if (!pressure.usable())
                {
                    map->mProgressPressure = pressure;
                    mPreparation->terminalReason = "dispatch_pressure";
                    if (const auto* ico = dynamic_cast<const Resource::OpenMWIncrementalCompileOperation*>(
                        info.incrementalCompileOperation); ico && ico->progressRepairEnabled())
                    {
                        recordPressureRefusal(*step.budget, pressure);
                        reportPressureEvidence(pressure, mPreparation->receipt, "dispatch_cancel");
                    }
                    mPreparation->lifetime->cancel(); mWave->lifetime->cancel();
                    return true;
                }
                // Recheck every owned identity immediately before its call.
                // Admission and previous submission never bypass demand apply.
                const auto* dep = mDependency < step.dependencies.size() ? &step.dependencies[mDependency] : nullptr;
                LockedStepDependency owner;
                if (dep && !lockStepDependency(*dep, owner))
                {
                    mPreparation->lifetime->cancel(); mWave->lifetime->cancel();
                    return true;
                }
                // The supplied chain can be unchanged while its separately
                // retained original source has changed. Cancel optional work
                // before apply could upload the original fallback here.
                if (owner.texture && (!owner.texture->preparationDispatchEligible(state)
                    || (dep && Resource::retainedTerrainTextureBytes(*owner.texture) != dep->retainedBytes)))
                {
                    mPreparation->lifetime->cancel(); mWave->lifetime->cancel();
                    return true;
                }
                Debug::HitchTrace::Scope submission("composite.dependency_step", Debug::HitchTrace::CpuTiming::WallEnvelope,
                    {}, owner.image ? std::string_view(owner.image->getFileName()) : std::string_view{});
                submission.setLink(mPreparation->receipt);
                submission.setMetric(0, "dependency_index", mDependency);
                submission.setMetric(1, "kind", static_cast<unsigned>(mKind));
                submission.setMetric(2, "source_bytes", mBytes);
                submission.setMetric(3, "generation", mPreparation->generation);
                if (owner.texture)
                {
                    osg::ref_ptr<osg::Texture::TextureObject> before = owner.texture->getTextureObject(state.getContextID());
                    const bool allocated = before && before->isAllocated();
                    osg::ref_ptr<Resource::PreparedTerrainTextureCompileOp> op
                        = new Resource::PreparedTerrainTextureCompileOp(owner.texture);
                    const bool result = op->compile(info);
                    if (auto* object = owner.texture->getTextureObject(state.getContextID()); object && object->isAllocated())
                        if (!allocated || object != before.get())
                            mWave->newTextureObjects.emplace_back(object);
                    if (result) mPreparation->markSubmitted(mDependency);
                    return result;
                }
                if (dep)
                {
                    state.pushStateSet(owner.pass);
                    struct Pop { osg::State& state; ~Pop() { state.popStateSet(); } } pop{state};
                    osg::ref_ptr<osg::Program::PerContextProgram> before = owner.program->getPCP(state);
                    const bool linked = before && before->isLinked() && !before->needsLink();
                    Shader::compileExactProgramVariant(*owner.program, state, submission.id());
                    auto* pcp = owner.program->getPCP(state);
                    auto& result = mPreparation->step->dependencies[mDependency];
                    if (pcp && pcp->isLinked() && !pcp->needsLink())
                    { result.submittedProgram = pcp; result.programName = pcp->getHandle();
                        if (!linked || pcp != before.get()) mWave->newPrograms.emplace_back(pcp); }
                    else mPreparation->failed = true;
                    mPreparation->markSubmitted(mDependency);
                    return true;
                }
                auto* target = dynamic_cast<Resource::PreparedTerrainTexture*>(map->mTexture.get());
                if (mKind == Kind::Texture)
                {
                    if (!target) { mPreparation->failed = true; return true; }
                    osg::ref_ptr<osg::Texture::TextureObject> before = target->getTextureObject(state.getContextID());
                    const bool allocated = before && before->isAllocated();
                    osg::ref_ptr<Resource::PreparedTerrainTextureCompileOp> op
                        = new Resource::PreparedTerrainTextureCompileOp(target);
                    const bool result = op->compile(info);
                    if (auto* object = target->getTextureObject(state.getContextID()); object && object->isAllocated())
                        if (!allocated || object != before.get())
                            mWave->newTextureObjects.emplace_back(object);
                    if (result) mPreparation->markSubmitted(mDependency);
                    return result;
                }
                auto* ext = state.get<osg::GLExtensions>();
                if (!ext->isFrameBufferObjectSupported || !target || !target->preparationUnchanged(state))
                    mPreparation->failed = true;
                else
                {
                    mPreparation->fbo->setAttachment(osg::Camera::COLOR_BUFFER, osg::FrameBufferAttachment(target));
                    mPreparation->fbo->apply(state, osg::FrameBufferObject::DRAW_FRAMEBUFFER);
                    mPreparation->failed = ext->glCheckFramebufferStatus(GL_FRAMEBUFFER_EXT) != GL_FRAMEBUFFER_COMPLETE_EXT;
                    mPreparation->fboName = mPreparation->fbo->getHandle(state.getContextID());
                    ext->glBindFramebuffer(GL_FRAMEBUFFER_EXT,
                        state.getGraphicsContext() ? state.getGraphicsContext()->getDefaultFboId() : 0);
                    state.haveAppliedTextureAttribute(state.getActiveTextureUnit(), osg::StateAttribute::TEXTURE);
                }
                mPreparation->completed = true;
                mPreparation->markSubmitted(mDependency);
                return true;
            }
        private:
            bool sourceDispatchInvalid(const osgUtil::IncrementalCompileOperation::CompileInfo& info) const
            {
                if (!info.getState() || !mPreparation->step
                    || mPreparation->context.get() != info.getState()->getGraphicsContext()
                    || mDependency >= mPreparation->step->dependencies.size()) return false;
                const auto& dependency = mPreparation->step->dependencies[mDependency];
                LockedStepDependency owner;
                if (!lockStepDependency(dependency, owner)) return true;
                return owner.texture && (!owner.texture->preparationDispatchEligible(*info.getState())
                    || Resource::retainedTerrainTextureBytes(*owner.texture) != dependency.retainedBytes);
            }
            osg::observer_ptr<CompositeMap> mMap;
            std::shared_ptr<CompositePreparation> mPreparation;
            std::shared_ptr<CompositeDependencyWave> mWave;
            std::size_t mDependency;
            Kind mKind;
            std::size_t mBytes;
        };

        struct CompositeWaveCompleted final : osgUtil::IncrementalCompileOperation::CompileCompletedCallback
        {
            CompositeWaveCompleted(CompositeMap* map, std::uint64_t generation,
                std::shared_ptr<CompositeDependencyWave> wave)
                : mMap(map), mGeneration(generation), mWave(std::move(wave)) {}
            bool compileCompleted(osgUtil::IncrementalCompileOperation::CompileSet*) override
            {
                mWave->completed = true;
                mWave->lifetime->complete();
                osg::ref_ptr<CompositeMap> map;
                if (mMap.lock(map) && map->mPreparationGeneration.load(std::memory_order_acquire) == mGeneration)
                    map->mProgressWake.fetch_add(1, std::memory_order_release);
                return true;
            }
            osg::observer_ptr<CompositeMap> mMap;
            std::uint64_t mGeneration;
            std::shared_ptr<CompositeDependencyWave> mWave;
        };

        class CompositeDependencyCompileOp final : public Resource::OpenMWCompositeCompileOp
        {
        public:
            CompositeDependencyCompileOp(CompositeMap* map, std::shared_ptr<CompositePreparation> preparation,
                std::size_t dependency, Kind kind, std::size_t bytes)
                : mMap(map), mPreparation(std::move(preparation)), mDependency(dependency), mKind(kind), mBytes(bytes) {}
            Kind kind() const override { return mKind; }
            std::size_t resourceBytes() const override { return mBytes; }
            bool required() const override { return mMap->mRequired.load(std::memory_order_acquire); }
            bool cancelled() const override
            {
                return mPreparation->lifetime->cancelled()
                    || mMap->mPreparationGeneration.load(std::memory_order_acquire) != mPreparation->generation
                    || mMap->mTexture.get() != mPreparation->target.get();
            }
            bool reusable(const osgUtil::IncrementalCompileOperation::CompileInfo& info) const override
            {
                if (cancelled())
                    return true;
                // CompileInfo is const for prediction, but its State is the
                // owning draw context's mutable stack. This balanced push/pop
                // selects only this bounded producer pass's OSG variant; it
                // applies no GL state and creates no resource cache.
                auto& state = const_cast<osg::State&>(*info.getState());
                if (mPreparation->context.get() != state.getGraphicsContext())
                    return false;
                if (mDependency < mPreparation->dependencies.size())
                {
                    const auto& dep = mPreparation->dependencies[mDependency];
                    if (!retainedDependency(dep))
                        return false;
                    if (dep.texture)
                    {
                        const bool ready = !info.incrementalCompileOperation->getForceTextureDownloadGeometry()
                            && dep.texture->preparationUnchanged(state);
                        if (ready) mPreparation->markSubmitted(mDependency);
                        return ready;
                    }
                    state.pushStateSet(dep.pass);
                    auto* program = dep.program->getPCP(state);
                    const bool ready = program && program->isLinked() && !program->needsLink();
                    if (ready)
                    {
                        auto& retained = mPreparation->dependencies[mDependency];
                        retained.submittedProgram = program;
                        retained.programName = program->getHandle();
                        mPreparation->markSubmitted(mDependency);
                    }
                    state.popStateSet();
                    return ready;
                }
                const auto* target = dynamic_cast<Resource::PreparedTerrainTexture*>(mMap->mTexture.get());
                const bool ready = mKind == Kind::Texture && target
                    && !info.incrementalCompileOperation->getForceTextureDownloadGeometry()
                    && target->preparationUnchanged(state);
                if (ready) mPreparation->markSubmitted(mDependency);
                return ready;
            }
            double estimatedTimeForCompile(osgUtil::IncrementalCompileOperation::CompileInfo&) const override
            {
                // A conservative initial estimate; existing ICO size tiers and
                // measured EMA/risk replace it after actual submissions.
                return mKind == Kind::Program ? .001 : .00025;
            }
            bool compile(osgUtil::IncrementalCompileOperation::CompileInfo& info) override
            {
                if (info.incrementalCompileOperation->getContextSet().size() != 1)
                {
                    mPreparation->lifetime->cancel();
                    return true;
                }
                if (cancelled() || mPreparation->context.get() != info.getState()->getGraphicsContext())
                    return true;
                auto& state = *info.getState();
                Debug::HitchTrace::Scope submission("composite.dependency_prepare", Debug::HitchTrace::CpuTiming::WallEnvelope);
                submission.setLink(mPreparation->receipt);
                submission.setMetric(0, "dependency_index", mDependency);
                submission.setMetric(1, "kind", static_cast<unsigned>(mKind));
                submission.setMetric(2, "required", required());
                submission.setMetric(3, "generation", mPreparation->generation);
                if (mDependency < mPreparation->dependencies.size())
                {
                    auto& dep = mPreparation->dependencies[mDependency];
                    if (!retainedDependency(dep))
                    {
                        mPreparation->lifetime->cancel();
                        return true;
                    }
                    if (dep.texture)
                    {
                        if (!dep.texture->preparationDispatchEligible(state))
                        {
                            mPreparation->lifetime->cancel();
                            return true;
                        }
                        osg::ref_ptr<Resource::PreparedTerrainTextureCompileOp> op
                            = new Resource::PreparedTerrainTextureCompileOp(dep.texture);
                        const bool complete = op->compile(info);
                        if (complete) mPreparation->markSubmitted(mDependency);
                        return complete;
                    }
                    // Push the producer's actual pass for the same OSG define
                    // variant. Applying the whole State here would also upload
                    // unrelated textures before their individual admission.
                    state.pushStateSet(dep.pass);
                    Shader::compileExactProgramVariant(*dep.program, state);
                    auto* program = dep.program->getPCP(state);
                    if (program && program->isLinked() && !program->needsLink())
                    {
                        dep.submittedProgram = program;
                        dep.programName = program->getHandle();
                    }
                    else
                        mPreparation->failed = true;
                    state.popStateSet();
                    mPreparation->markSubmitted(mDependency);
                    return true;
                }
                auto* target = dynamic_cast<Resource::PreparedTerrainTexture*>(mMap->mTexture.get());
                if (mKind == Kind::Texture)
                {
                    if (target)
                    {
                        osg::ref_ptr<Resource::PreparedTerrainTextureCompileOp> op
                            = new Resource::PreparedTerrainTextureCompileOp(target);
                        const bool complete = op->compile(info);
                        if (complete) mPreparation->markSubmitted(mDependency);
                        return complete;
                    }
                    mPreparation->failed = true;
                    return true;
                }
                auto* ext = state.get<osg::GLExtensions>();
                if (!ext->isFrameBufferObjectSupported || !target || !target->preparationUnchanged(state))
                    mPreparation->failed = true;
                else
                {
                    mPreparation->fbo->setAttachment(osg::Camera::COLOR_BUFFER, osg::FrameBufferAttachment(target));
                    mPreparation->fbo->apply(state, osg::FrameBufferObject::DRAW_FRAMEBUFFER);
                    mPreparation->failed = ext->glCheckFramebufferStatus(GL_FRAMEBUFFER_EXT) != GL_FRAMEBUFFER_COMPLETE_EXT;
                    mPreparation->fboName = mPreparation->fbo->getHandle(state.getContextID());
                    const GLuint original = state.getGraphicsContext() ? state.getGraphicsContext()->getDefaultFboId() : 0;
                    ext->glBindFramebuffer(GL_FRAMEBUFFER_EXT, original);
                    state.haveAppliedTextureAttribute(state.getActiveTextureUnit(), osg::StateAttribute::TEXTURE);
                }
                mPreparation->completed = true;
                mPreparation->markSubmitted(mDependency + 1);
                return true;
            }

        private:
            osg::ref_ptr<CompositeMap> mMap;
            std::shared_ptr<CompositePreparation> mPreparation;
            std::size_t mDependency;
            Kind mKind;
            std::size_t mBytes;
        };

        struct CompositeCompileCompleted final : osgUtil::IncrementalCompileOperation::CompileCompletedCallback
        {
            explicit CompositeCompileCompleted(std::shared_ptr<Resource::V321ResourceCompileLifetime> lifetime)
                : mLifetime(std::move(lifetime)) {}
            // There is no duplicate scene publication/merge queue for these
            // producer-owned dependencies.
            bool compileCompleted(osgUtil::IncrementalCompileOperation::CompileSet*) override
            {
                mLifetime->complete();
                return true;
            }
            std::shared_ptr<Resource::V321ResourceCompileLifetime> mLifetime;
        };
    }

    void CompositeMapRenderer::configurePreparation(Resource::OpenMWIncrementalCompileOperation* ico,
        Resource::SpeculativeScope::Context pressure)
    {
        mICO = ico;
        mAdmission = ico ? ico->compositeAdmission() : nullptr;
        mPreparationEnabled = bool(mAdmission);
        mPressureContext = pressure;
    }

    bool CompositeMapRenderer::preparationSupported(const osg::State& state) const
    {
        return mPreparationEnabled && mICO && mICO->getContextSet().size() == 1
            && state.getGraphicsContext() && *mICO->getContextSet().begin() == state.getGraphicsContext();
    }
    bool CompositeMapRenderer::progressEnabled() const
    {
        return mIncrementalPreparationEnabled && mICO && mICO->progressRepairEnabled();
    }

    CompositeMapRenderer::PreparationStats CompositeMapRenderer::preparationStats() const
    {
        auto result = mPreparationStats;
        result.scheduled = mPreparationScheduled.load(std::memory_order_relaxed);
        result.pendingBytes = mPreparationBytes.load(std::memory_order_relaxed)
            + mStepBudget->bytes.load(std::memory_order_relaxed);
        result.chargedMs = mAdmission ? mAdmission->chargedMs() : 0;
        result.dependencyWaves = mDependencyWaves.load(std::memory_order_relaxed);
        result.pressureYields = mPressureYields.load(std::memory_order_relaxed);
        result.descriptorBuilds = mDescriptorBuilds.load(std::memory_order_relaxed);
        result.capacityDeclines = mCapacityDeclines.load(std::memory_order_relaxed);
        result.pressureRefusalChecks = mStepBudget->pressureRefusalChecks.load(std::memory_order_relaxed);
        result.pressureUnreportedState = mStepBudget->pressureUnreportedState.load(std::memory_order_relaxed);
        for (std::size_t i = 0; i < result.pressureRefusals.size(); ++i)
            result.pressureRefusals[i] = mStepBudget->pressureRefusals[i].load(std::memory_order_relaxed);
        for (std::size_t i = 0; i < result.pressureStates.size(); ++i)
            result.pressureStates[i] = mStepBudget->pressureStates[i].load(std::memory_order_relaxed);
        return result;
    }

    bool CompositeMapRenderer::checkStepPressure(CompositeMap& map,
        const Resource::SpeculativeScope::Context& pressure, std::string_view stage) const
    {
        map.mProgressPressure = CompositePressureEvidence::read(pressure);
        if (map.mProgressPressure.usable()) return true;
        mPressureYields.fetch_add(1, std::memory_order_relaxed);
        map.mProgressRefusal = CompositeProgressWait::Reason::Pressure;
        if (progressEnabled())
        {
            recordPressureRefusal(*mStepBudget, map.mProgressPressure);
            // A changed cached publication or predicate is a useful receipt;
            // repeated checks of that same denial are only aggregate counts.
            const auto signature = map.mProgressPressure.signature();
            const auto generation = map.mProgressPressure.sample.generation;
            if (!map.mPressureReceiptKnown || map.mPressureReceiptGeneration != generation
                || map.mPressureReceiptSignature != signature)
            {
                map.mPressureReceiptKnown = true;
                map.mPressureReceiptGeneration = generation;
                map.mPressureReceiptSignature = signature;
                reportPressureEvidence(map.mProgressPressure,
                    map.mPreparation ? map.mPreparation->receipt : map.mPreparationReceipt, stage);
            }
        }
        return false;
    }

    void CompositeMapRenderer::retirePreparations() const
    {
        // At most sixteen weak-debt entries; expiry must progress even if every
        // queued map is parked and there are no fresh producer admissions.
        if (progressEnabled()) mStepBudget->pruneDebt();
        std::vector<osg::ref_ptr<CompositeMap>> retiredMaps;
        std::vector<std::shared_ptr<CompositePreparation>> retiredPreparations;
        std::vector<std::vector<osg::ref_ptr<osg::Drawable>>> retiredProducerGraphs;
        retiredProducerGraphs.reserve(sMaxPreparationMaps);
        {
        std::lock_guard<std::mutex> pendingLock(mPreparationMutex);
        for (auto it = mPendingPreparations.begin(); it != mPendingPreparations.end();)
        {
            auto& map = **it;
            auto& preparation = map.mPreparation;
            // Only this map's own target reference is included in the descriptor.
            // Loss of all external consumers cancels speculative queued work.
            if (preparation && map.mTexture.get() == preparation->target.get()
                && map.mTexture->referenceCount() <= (preparation->fbo->hasAttachment(osg::Camera::COLOR_BUFFER) ? 2 : 1))
            {
                preparation->lifetime->cancel();
                ++mPreparationStats.cancelled;
            }
            if (!preparation || preparation->lifetime->cancelled()
                || map.mDrawables.empty())
            {
                if (preparation)
                    mPreparationBytes.fetch_sub(preparation->bytes, std::memory_order_relaxed);
                if (map.mDrawables.empty() && preparation)
                {
                    // A completed map keeps its output, not a hidden residency
                    // cache of every source layer/program/preparation FBO.
                    preparation->lifetime->cancel();
                    map.mPreparationGeneration.fetch_add(1, std::memory_order_acq_rel);
                    retiredPreparations.push_back(std::move(map.mPreparation));
                }
                retiredMaps.push_back(std::move(*it));
                it = mPendingPreparations.erase(it);
            }
            else
                ++it;
        }
        } // Final payload/charge releases occur after the admission lock.
        {
            std::lock_guard<std::mutex> pendingLock(mPreparationMutex);
            for (auto it = mPendingStepPreparations.begin(); it != mPendingStepPreparations.end();)
            {
                osg::ref_ptr<CompositeMap> map;
                if (!it->lock(map)) { it = mPendingStepPreparations.erase(it); continue; }
                auto preparation = map->mPreparation;
                if (!preparation || !preparation->step) { it = mPendingStepPreparations.erase(it); continue; }
                int targetOwners = 1;
                if (preparation->fbo->hasAttachment(osg::Camera::COLOR_BUFFER)
                    && preparation->fbo->getAttachment(osg::Camera::COLOR_BUFFER).getTexture() == map->mTexture.get())
                    ++targetOwners;
                if (mFBO->hasAttachment(osg::Camera::COLOR_BUFFER)
                    && mFBO->getAttachment(osg::Camera::COLOR_BUFFER).getTexture() == map->mTexture.get())
                    ++targetOwners;
                // A terminal wave also owns the target until its actual release.
                const bool waveOwnsTarget = preparation->target.get() == map->mTexture.get()
                    && (bool(preparation->step->targetDebt)
                        || (preparation->step->wave && preparation->step->wave->terminal));
                if (map->mTexture.get() == preparation->target.get()
                    && map->mTexture->referenceCount() <= targetOwners + int(waveOwnsTarget))
                {
                    preparation->lifetime->cancel(); ++mPreparationStats.cancelled;
                    preparation->terminalReason = "consumer_retired";
                    // This seam still knows every internal target owner. Once
                    // detached, a queued canceled wave's reference must not be
                    // mistaken for a new external consumer by compileUntil.
                    map->mCompiled = map->mDrawables.size();
                    retiredProducerGraphs.emplace_back();
                    retiredProducerGraphs.back().swap(map->mDrawables);
                }
                if (preparation->lifetime->cancelled() || map->mDrawables.empty())
                {
                    if (progressEnabled()) reportProgressTerminal(*map, preparation->terminalReason == "pending"
                        ? (map->mDrawables.empty() ? "completed" : "cancelled") : preparation->terminalReason);
                    preparation->lifetime->cancel();
                    if (preparation->step->wave) preparation->step->wave->lifetime->cancel();
                    map->mPreparationGeneration.fetch_add(1, std::memory_order_acq_rel);
                    retiredPreparations.push_back(std::move(map->mPreparation));
                    it = mPendingStepPreparations.erase(it);
                }
                else ++it;
            }
        }
    }

    bool CompositeMapRenderer::dependenciesReady(const CompositeMap& map, osg::State& state) const
    {
        const auto& preparation = map.mPreparation;
        if (!preparation || !preparation->completed || preparation->failed
            || preparation->lifetime->cancelled()
            || preparation->generation != map.mPreparationGeneration.load(std::memory_order_acquire)
            || preparation->context.get() != state.getGraphicsContext()
            || preparation->target.get() != map.mTexture.get()
            || preparation->fboName == 0 || preparation->fbo->getHandle(state.getContextID()) != preparation->fboName)
            return false;
        auto* target = dynamic_cast<Resource::PreparedTerrainTexture*>(map.mTexture.get());
        if (!target || !target->preparationUnchanged(state))
            return false;
        if (preparation->step)
        {
            if (!stepTargetUnchanged(*preparation, map)) return false;
            const auto& wave = preparation->step->wave;
            if (!wave || wave->terminal || !wave->completed || wave->consumed
                || map.mCompiled != wave->layerBegin) return false;
            for (std::size_t index = wave->begin; index < wave->end; ++index)
            {
                const auto& dep = preparation->step->dependencies[index];
                LockedStepDependency owner;
                if (!lockStepDependency(dep, owner)) return false;
                if (owner.texture)
                {
                    if (!owner.texture->preparationUnchanged(state)) return false;
                }
                else
                {
                    state.pushStateSet(owner.pass);
                    auto* pcp = owner.program->getPCP(state);
                    const bool ready = pcp && pcp == dep.submittedProgram.get()
                        && pcp->getHandle() == dep.programName && pcp->isLinked() && !pcp->needsLink();
                    state.popStateSet();
                    if (!ready) return false;
                }
            }
            return true;
        }
        for (const auto& dep : preparation->dependencies)
        {
            if (!retainedDependency(dep))
                return false;
            if (dep.texture)
            {
                if (!dep.texture->preparationUnchanged(state))
                    return false;
            }
            else
            {
                state.pushStateSet(dep.pass);
                auto* pcp = dep.program->getPCP(state);
                const bool ready = pcp && pcp == dep.submittedProgram.get() && pcp->getHandle() == dep.programName
                    && pcp->isLinked() && !pcp->needsLink();
                state.popStateSet();
                if (!ready)
                    return false;
            }
        }
        return true;
    }

    bool CompositeMapRenderer::prepare(CompositeMap& map, osg::RenderInfo& info) const
    {
        auto& state = *info.getState();
        if (map.mPreparation && map.mPreparation->step
            && !checkStepPressure(map, map.mPreparation->step->pressure, "existing_wave_cancel"))
        {
            map.mPreparation->terminalReason = "pressure";
            map.mPreparation->lifetime->cancel();
            if (map.mPreparation->step->wave) map.mPreparation->step->wave->lifetime->cancel();
            map.mCompiled = 0;
            retirePreparations();
            return false;
        }
        if (dependenciesReady(map, state))
            return true;
        if (map.mPreparation)
        {
            if (map.mPreparation->step
                && !map.mPreparation->lifetime->cancelled()
                && map.mPreparation->target.get() == map.mTexture.get()
                && map.mPreparation->context.get() == state.getGraphicsContext())
            {
                auto& step = *map.mPreparation->step;
                if (!checkStepPressure(map, step.pressure, "advance_existing_wave"))
                {
                    map.mPreparation->lifetime->cancel();
                    if (step.wave) step.wave->lifetime->cancel();
                }
                else
                {
                    if (map.mPreparation->failed) return true;
                    if (step.wave && !step.wave->completed) return false;
                    if (step.wave)
                    {
                        if (step.wave->terminal)
                            step.targetDebt = std::move(step.wave);
                        else if (!step.wave->consumed)
                        {
                            // The submission is ready, but its source debt is
                            // still outstanding until the actual private layer
                            // draw and caller restoration complete.
                            if (dependenciesReady(map, state)) return true;
                            map.mPreparation->lifetime->cancel();
                            step.wave->lifetime->cancel();
                            return false;
                        }
                        else
                        {
                            step.cursor = step.wave->end;
                            step.wave.reset();
                        }
                    }
                    return scheduleDependencyWave(map, state.getGraphicsContext());
                }
            }
            if (map.mPreparation->completed && map.mPreparation->failed
                && map.mPreparation->context.get() == state.getGraphicsContext()
                && map.mPreparation->target.get() == map.mTexture.get())
                return true; // preserve complete old rendering after preparation failure
            if (!map.mPreparation->completed && !map.mPreparation->lifetime->cancelled()
                && map.mPreparation->target.get() == map.mTexture.get()
                && map.mPreparation->context.get() == state.getGraphicsContext())
                return false;
            map.mPreparation->lifetime->cancel();
            map.mPreparation->terminalReason = "invalidated";
            if (progressEnabled()) reportProgressTerminal(map, "invalidated");
            ++mPreparationStats.invalidated;
            retirePreparations();
            map.mPreparation.reset();
            map.mCompiled = 0; // retained candidate layers rebuild the complete new revision
        }
        return schedulePreparation(map, state.getGraphicsContext());
    }

    void CompositeMapRenderer::prepareAtCreation(CompositeMap& map) const
    {
        if (!producerPreparationEnabled() || !mICO || mICO->getContextSet().size() != 1)
            return;
        // ChunkManager calls this before publication. No live queued map's
        // draw-owned fields are read or changed by a producer.
        if (!map.mBatchingEligible || map.mPreparation || map.mCompiled != 0 || map.mRequired.load(std::memory_order_acquire))
            return;
        Debug::HitchTrace::Scope receipt("composite.producer_prepare", Debug::HitchTrace::CpuTiming::WallEnvelope);
        receipt.retain();
        map.mPreparationReceipt = receipt.id();
        schedulePreparation(map, *mICO->getContextSet().begin(), true);
        receipt.setMetric(0, "accepted", bool(map.mPreparation));
        receipt.setMetric(1, "dependencies", map.mPreparation
            ? (map.mPreparation->step ? map.mPreparation->step->dependencies.size() : map.mPreparation->dependencies.size()) : 0);
        receipt.setMetric(2, "retained_bytes", map.mPreparation
            ? (map.mPreparation->step ? map.mPreparation->step->metadataBytes
                + (map.mPreparation->step->wave ? map.mPreparation->step->wave->bytes : 0) : map.mPreparation->bytes) : 0);
        receipt.setMetric(3, "generation", map.mPreparationGeneration.load(std::memory_order_relaxed));
    }

    bool CompositeMapRenderer::schedulePreparation(CompositeMap& map, osg::GraphicsContext* context, bool atCreation) const
    {
        if (mIncrementalPreparationEnabled)
            return scheduleStepPreparation(map, context, atCreation);
        // Reserve and publish the bounded descriptor together. Producers only
        // append new unpublished maps; retirement/progress remains draw-owned.
        std::lock_guard<std::mutex> pendingLock(mPreparationMutex);
        // The readiness design is deliberately limited to one actual graphics
        // context. Unsupported contexts/resources use the exact original bake.
        if (!mICO || mICO->getContextSet().size() != 1
            || !mICO->getContextSet().contains(context)
            || !dynamic_cast<Resource::PreparedTerrainTexture*>(map.mTexture.get()))
            return true;
        if (mPendingPreparations.size() >= sMaxPreparationMaps)
            return false;

        auto preparation = std::make_shared<CompositePreparation>();
        preparation->target = map.mTexture;
        preparation->context = context;
        preparation->generation = map.mPreparationGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
        preparation->bytes = static_cast<std::size_t>(map.mTexture->getTextureWidth())
            * static_cast<std::size_t>(map.mTexture->getTextureHeight()) * 4;
        std::unordered_set<const osg::Texture*> seen;
        for (std::size_t i = map.mCompiled; i < map.mDrawables.size(); ++i)
        {
            auto* pass = map.mDrawables[i] ? map.mDrawables[i]->getStateSet() : nullptr;
            if (!pass)
                continue;
            const auto& attributes = pass->getTextureAttributeList();
            for (unsigned unit = 0; unit < attributes.size(); ++unit)
            {
                auto* texture = dynamic_cast<osg::Texture*>(pass->getTextureAttribute(unit, osg::StateAttribute::TEXTURE));
                if (!texture)
                    continue;
                auto* prepared = dynamic_cast<Resource::PreparedTerrainTexture*>(texture);
                if (!prepared || !context->getState() || !prepared->preparationAdmissionEligible(*context->getState()))
                    return true;
                if (prepared->getDataVariance() == osg::Object::DYNAMIC
                    || (prepared->getImage() && (prepared->getImage()->getDataVariance() == osg::Object::DYNAMIC
                        || prepared->getImage()->requiresUpdateCall())))
                    return true;
                CompositePreparation::Dependency dep;
                dep.texture = prepared;
                dep.pass = pass;
                dep.drawable = map.mDrawables[i];
                dep.unit = unit;
                preparation->dependencies.push_back(std::move(dep));
                if (preparation->dependencies.size() > 512)
                    return true;
                if (seen.insert(texture).second && prepared->getImage())
                    preparation->bytes += Resource::retainedTerrainTextureBytes(*prepared);
            }
            if (auto* program = dynamic_cast<osg::Program*>(pass->getAttribute(osg::StateAttribute::PROGRAM)))
            {
                CompositePreparation::Dependency dep;
                dep.program = program;
                dep.pass = pass;
                dep.drawable = map.mDrawables[i];
                preparation->dependencies.push_back(std::move(dep));
                if (preparation->dependencies.size() > 512)
                    return true;
            }
        }
        if (preparation->bytes > sMaxPreparationBytes)
            return true; // no speculative oversized allocation; demand keeps exact fallback
        if (preparation->bytes > sMaxPreparationBytes - mPreparationBytes.load(std::memory_order_relaxed))
            return false;
        Debug::HitchTrace::Scope receipt("composite.prepare_admit", Debug::HitchTrace::CpuTiming::WallEnvelope);
        receipt.retain();
        receipt.setLink(map.mPreparationReceipt);
        preparation->receipt = receipt.id();
        receipt.setMetric(0, "generation", preparation->generation);
        receipt.setMetric(1, "dependencies", preparation->dependencies.size());
        receipt.setMetric(2, "retained_bytes", preparation->bytes);
        receipt.setMetric(3, "early_producer", atCreation);
        osg::ref_ptr<Resource::V321ClassifiedCompileSet> set = new Resource::V321ClassifiedCompileSet(
            preparation->lifetime, Resource::V321CompileClass::Terrain, Resource::V321CompileUrgency::Background);
        auto& list = set->_compileMap[context];
        for (std::size_t i = 0; i < preparation->dependencies.size(); ++i)
        {
            const auto& dep = preparation->dependencies[i];
            const auto kind = dep.texture ? Resource::OpenMWCompositeCompileOp::Kind::Texture
                : Resource::OpenMWCompositeCompileOp::Kind::Program;
            list.add(new CompositeDependencyCompileOp(&map, preparation, i, kind,
                dep.texture && dep.texture->getImage() ? dep.texture->getImage()->getTotalDataSize() : 0));
        }
        const auto end = preparation->dependencies.size();
        list.add(new CompositeDependencyCompileOp(&map, preparation, end,
            Resource::OpenMWCompositeCompileOp::Kind::Texture,
            static_cast<std::size_t>(map.mTexture->getTextureWidth()) * map.mTexture->getTextureHeight() * 4));
        list.add(new CompositeDependencyCompileOp(&map, preparation, end,
            Resource::OpenMWCompositeCompileOp::Kind::Target, 0));
        ++set->_numberCompileListsToCompile;
        set->_compileCompletedCallback = new CompositeCompileCompleted(preparation->lifetime);
        map.mPreparation = preparation;
        mPendingPreparations.push_back(&map);
        mPreparationBytes.fetch_add(preparation->bytes, std::memory_order_relaxed);
        mPreparationScheduled.fetch_add(1, std::memory_order_relaxed);
        mICO->add(set, false);
        return false;
    }

    bool CompositeMapRenderer::scheduleStepPreparation(CompositeMap& map, osg::GraphicsContext* context, bool atCreation) const
    {
        if (!map.mBatchingEligible || !mICO || mICO->getContextSet().size() != 1 || !mICO->getContextSet().contains(context)
            || !dynamic_cast<Resource::PreparedTerrainTexture*>(map.mTexture.get())
            || mICO->getForceTextureDownloadGeometry()) return true;
        map.mProgressRefusal = CompositeProgressWait::Reason::None;
        if (!checkStepPressure(map, mPressureContext, "descriptor_admission"))
        {
            return false;
        }
        const std::size_t metadata = 256u * 1024u + 512u * sizeof(CompositeStepPreparation::Dependency);
        if (progressEnabled())
        {
            std::lock_guard<std::mutex> pendingLock(mPreparationMutex);
            // Early refusal only: the final locked reserve below remains the
            // publication proof. No dependency vector is built for a full pool.
            if (mStepBudget->maps.load(std::memory_order_acquire) >= sMaxPreparationMaps
                || !mStepBudget->accepting() || !mStepBudget->canReserve(metadata))
            {
                if (progressEnabled()) mCapacityDeclines.fetch_add(1, std::memory_order_relaxed);
                map.mProgressRefusal = CompositeProgressWait::Reason::Capacity;
                return false;
            }
        }
        if (progressEnabled()) mDescriptorBuilds.fetch_add(1, std::memory_order_relaxed);

        auto preparation = std::make_shared<CompositePreparation>();
        auto step = std::make_shared<CompositeStepPreparation>();
        step->pressure = mPressureContext;
        step->dependencies.reserve(512);
        for (std::size_t index = map.mCompiled; index < map.mDrawables.size(); ++index)
        {
            auto* drawable = map.mDrawables[index].get();
            auto* pass = drawable ? drawable->getStateSet() : nullptr;
            if (!pass) return true;
            const auto& attributes = pass->getTextureAttributeList();
            for (unsigned unit = 0; unit < attributes.size(); ++unit)
            {
                auto* attribute = pass->getTextureAttribute(unit, osg::StateAttribute::TEXTURE);
                if (!attribute) continue;
                auto* texture = dynamic_cast<Resource::PreparedTerrainTexture*>(attribute);
                auto* image = texture ? texture->getImage() : nullptr;
                if (!texture || !context->getState() || !texture->preparationAdmissionEligible(*context->getState())
                    || !image || !image->data() || texture->getSubloadCallback()
                    || texture->getUpdateCallback() || texture->getEventCallback() || texture->getReadPBuffer()
                    || texture->getDataVariance() == osg::Object::DYNAMIC
                    || image->getDataVariance() == osg::Object::DYNAMIC || image->requiresUpdateCall()) return true;
                if (step->dependencies.size() == 512) return true;
                CompositeStepPreparation::Dependency dep;
                dep.texture = texture; dep.image = image; dep.imageRevision = image->getModifiedCount();
                dep.sourceBytes = image->getTotalDataSize();
                dep.retainedBytes = Resource::retainedTerrainTextureBytes(*texture);
                dep.pass = pass; dep.drawable = drawable;
                dep.unit = unit; dep.layer = index;
                if (!dep.sourceBytes || dep.retainedBytes > sMaxPreparationBytes) return true;
                step->dependencies.push_back(std::move(dep));
            }
            if (auto* program = dynamic_cast<osg::Program*>(pass->getAttribute(osg::StateAttribute::PROGRAM)))
            {
                if (step->dependencies.size() == 512) return true;
                CompositeStepPreparation::Dependency dep;
                dep.program = program; dep.pass = pass; dep.drawable = drawable; dep.layer = index;
                step->dependencies.push_back(std::move(dep));
            }
        }
        // All future payloads remain weak. This estimate includes the bounded
        // descriptor/queued operation bookkeeping, separately from source bytes.
        const auto width = map.mTexture->getTextureWidth(), height = map.mTexture->getTextureHeight();
        const auto targetBytes = width > 0 && height > 0 ? std::size_t(width) * std::size_t(height) * 4 : 0;
        if (!targetBytes || targetBytes > sMaxPreparationBytes - metadata) return true;
        step->targetShape = {width, height, map.mTexture->getInternalFormat()};
        // A window is one COMPLETE layer, including every bound source. Never
        // publish a half-layer window or prepare it over several refunded waves.
        std::size_t layer = 0, layerBytes = 0;
        std::unordered_set<const osg::Texture*> layerTextures;
        std::unordered_set<const osg::Program*> layerPrograms;
        for (const auto& dep : step->dependencies)
        {
            if (dep.layer != layer)
            { layer = dep.layer; layerBytes = 0; layerTextures.clear(); layerPrograms.clear(); }
            LockedStepDependency owner;
            if (!lockStepDependency(dep, owner)) return true;
            if (owner.texture && layerTextures.insert(owner.texture.get()).second) layerBytes += dep.retainedBytes;
            if (owner.program && layerPrograms.insert(owner.program.get()).second)
            {
                layerBytes += 64u * 1024u;
                for (unsigned shader = 0; shader < owner.program->getNumShaders(); ++shader)
                    if (const auto* source = owner.program->getShader(shader)) layerBytes += source->getShaderSource().size();
                    else return true;
            }
            if (layerBytes > sMaxPreparationBytes - metadata - targetBytes) return true;
        }
        auto metadataLease = std::make_shared<CompositeMetadataLease>();
        {
            std::lock_guard<std::mutex> pendingLock(mPreparationMutex);
            mStepBudget->pruneDebt();
            if (mStepBudget->maps.load(std::memory_order_relaxed) >= sMaxPreparationMaps
                || !mStepBudget->accepting() || !mStepBudget->reserve(metadata))
            {
                if (progressEnabled()) mCapacityDeclines.fetch_add(1, std::memory_order_relaxed);
                map.mProgressRefusal = CompositeProgressWait::Reason::Capacity;
                return false;
            }
            step->budget = mStepBudget; step->metadataBytes = metadata;
            mStepBudget->maps.fetch_add(1, std::memory_order_acq_rel);
            step->metadata = std::move(metadataLease);
            step->metadata->budget = mStepBudget; step->metadata->bytes = metadata;
            preparation->step = step;
            preparation->target = map.mTexture; preparation->context = context;
            preparation->generation = map.mPreparationGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
            Debug::HitchTrace::Scope receipt("composite.step_admit", Debug::HitchTrace::CpuTiming::WallEnvelope);
            receipt.retain(); receipt.setLink(map.mPreparationReceipt);
            preparation->receipt = receipt.id();
            receipt.setMetric(0, "generation", preparation->generation);
            receipt.setMetric(1, "dependencies", step->dependencies.size());
            receipt.setMetric(2, "descriptor_bytes", metadata);
            receipt.setMetric(3, "early_producer", atCreation);
            map.mPreparation = preparation;
            if (progressEnabled())
            {
                // Cull observes either one whole generation or an explicit
                // unknown snapshot; it never mixes former layer times with a
                // new admission. Zero brackets the independent atomic fields.
                map.mProgressReceiptGeneration.store(0, std::memory_order_release);
                map.mProgressAdmittedTick.store(progressTick(), std::memory_order_release);
                map.mProgressFirstLayerTick.store(0, std::memory_order_release);
                map.mProgressCompletedTick.store(0, std::memory_order_release);
                map.mProgressReceiptGeneration.store(preparation->generation, std::memory_order_release);
            }
            mPendingStepPreparations.push_back(&map);
            mPreparationScheduled.fetch_add(1, std::memory_order_relaxed);
        }
        return scheduleDependencyWave(map, context);
    }

    bool CompositeMapRenderer::scheduleDependencyWave(CompositeMap& map, osg::GraphicsContext* context) const
    {
        map.mProgressRefusal = CompositeProgressWait::Reason::Capacity;
        std::lock_guard<std::mutex> pendingLock(mPreparationMutex);
        const auto preparation = map.mPreparation;
        if (!preparation || !preparation->step || preparation->lifetime->cancelled()) return false;
        auto& step = *preparation->step;
        if (step.wave) return false;
        mStepBudget->pruneDebt();
        if (!mStepBudget->accepting()) return false;
        if (!checkStepPressure(map, step.pressure, "wave_admission"))
        {
            return false;
        }
        auto* state = context ? context->getState() : nullptr;
        if (!state || preparation->context.get() != context
            || !stepTargetUnchanged(*preparation, map))
        { preparation->lifetime->cancel(); return false; }
        // Previous submissions remain revision/context qualified. A changed
        // source/pass/program invalidates the entire descriptor, not merely the
        // current cursor. The next admitted descriptor restarts the exact bake.
        for (std::size_t index = 0; index < step.cursor; ++index)
        {
            const auto& dep = step.dependencies[index];
            LockedStepDependency owner;
            if (!lockStepDependency(dep, owner)) { preparation->lifetime->cancel(); return false; }
            if (owner.texture && !owner.texture->preparationUnchanged(*state))
            { preparation->lifetime->cancel(); return false; }
            if (owner.program)
            {
                state->pushStateSet(owner.pass);
                auto* pcp = owner.program->getPCP(*state);
                const bool valid = pcp && pcp == dep.submittedProgram.get() && pcp->getHandle() == dep.programName
                    && pcp->isLinked() && !pcp->needsLink();
                state->popStateSet();
                if (!valid) { preparation->lifetime->cancel(); return false; }
            }
        }
        auto wave = std::make_shared<CompositeDependencyWave>();
        wave->begin = step.cursor; wave->end = step.cursor;
        const auto available = sMaxPreparationBytes - std::min(sMaxPreparationBytes,
            mStepBudget->bytes.load(std::memory_order_acquire));
        std::unordered_set<const osg::Image*> images;
        std::unordered_set<const osg::Program*> programs;
        std::unordered_set<const osg::Texture*> textures;
        wave->terminal = !preparation->completed;
        wave->layerBegin = map.mCompiled;
        wave->layerEnd = map.mCompiled + 1;
        for (std::size_t index = step.cursor; !wave->terminal && index < step.dependencies.size(); ++index)
        {
            const auto& dep = step.dependencies[index];
            if (dep.layer != wave->layerBegin) break;
            LockedStepDependency owner;
            if (!lockStepDependency(dep, owner)) { preparation->lifetime->cancel(); return false; }
            if (owner.texture && !owner.texture->preparationDispatchEligible(*state))
            { preparation->lifetime->cancel(); return false; }
            if (owner.texture)
            {
                const auto bytes = textures.contains(owner.texture.get()) ? 0 : dep.retainedBytes;
                if (bytes > available - std::min(available, wave->bytes)) return false;
                if (textures.insert(owner.texture.get()).second) { wave->textures.push_back(owner.texture); wave->bytes += bytes; }
                if (images.insert(owner.image.get()).second) wave->images.push_back(owner.image);
            }
            else
            {
                const auto& program = owner.program;
                if (!programs.contains(program.get()))
                {
                    std::size_t bytes = 64u * 1024u;
                    for (unsigned shader = 0; shader < program->getNumShaders(); ++shader)
                        bytes += program->getShader(shader)->getShaderSource().size();
                    if (bytes > available - std::min(available, wave->bytes)) return false;
                    programs.insert(program.get()); wave->programs.push_back(program); wave->bytes += bytes;
                }
            }
            wave->end = index + 1;
        }
        if (wave->terminal)
        {
            auto* target = dynamic_cast<Resource::PreparedTerrainTexture*>(map.mTexture.get());
            const auto width = target ? target->getTextureWidth() : 0;
            const auto height = target ? target->getTextureHeight() : 0;
            const auto bytes = width > 0 && height > 0 ? std::size_t(width) * std::size_t(height) * 4 : 0;
            if (!bytes || bytes > available) return false;
            wave->textures.push_back(target); wave->bytes = bytes;
        }
        else if (wave->end == wave->begin) return false;
        wave->newTextureObjects.reserve(wave->textures.size());
        wave->newPrograms.reserve(wave->programs.size());
        if (!mStepBudget->reserve(wave->bytes)) return false;
        wave->budget = mStepBudget;
        wave->metadata = step.metadata;
        osg::ref_ptr<Resource::V321ClassifiedCompileSet> set = new Resource::V321ClassifiedCompileSet(
            wave->lifetime, Resource::V321CompileClass::Terrain, Resource::V321CompileUrgency::Background);
        auto& list = set->_compileMap[context];
        for (std::size_t index = wave->begin; !wave->terminal && index < wave->end; ++index)
        {
            const auto& dep = step.dependencies[index];
            list.add(new CompositeStepCompileOp(&map, preparation, wave, index,
                dep.texture.valid() ? Resource::OpenMWCompositeCompileOp::Kind::Texture
                                    : Resource::OpenMWCompositeCompileOp::Kind::Program, dep.sourceBytes));
        }
        if (wave->terminal)
        {
            list.add(new CompositeStepCompileOp(&map, preparation, wave, step.dependencies.size(),
                Resource::OpenMWCompositeCompileOp::Kind::Texture, wave->bytes));
            list.add(new CompositeStepCompileOp(&map, preparation, wave, step.dependencies.size() + 1,
                Resource::OpenMWCompositeCompileOp::Kind::Target, 0));
        }
        ++set->_numberCompileListsToCompile;
        set->_compileCompletedCallback = new CompositeWaveCompleted(&map, preparation->generation, wave);
        Debug::HitchTrace::Scope receipt("composite.wave_admit", Debug::HitchTrace::CpuTiming::WallEnvelope);
        receipt.retain(); receipt.setLink(preparation->receipt);
        wave->receipt = receipt.id();
        receipt.setMetric(0, "begin", wave->begin); receipt.setMetric(1, "end", wave->end);
        receipt.setMetric(2, "source_bytes", wave->bytes); receipt.setMetric(3, "terminal", wave->terminal);
        if (progressEnabled()) reportPressureEvidence(map.mProgressPressure, wave->receipt, "wave_admit");
        step.wave = wave;
        map.mProgressRefusal = CompositeProgressWait::Reason::Dependency;
        mDependencyWaves.fetch_add(1, std::memory_order_relaxed);
        mICO->add(set, false);
        return false;
    }

    CompositeMapRenderer::CompositeMapRenderer()
        : mCompositeBatchEnabled([] {
            const char* value = std::getenv("OPENMW_HITCH_COMPOSITE_BATCH");
            return value && std::string_view(value) == "1";
        }())
        , mProducerPreparationEnabled([] {
            const char* value = std::getenv("OPENMW_HITCH_COMPOSITE_EARLY");
            return value && std::string_view(value) == "1";
        }())
        , mIncrementalPreparationEnabled([] {
            const char* value = std::getenv("OPENMW_HITCH_COMPOSITE_STEP");
            return value && std::string_view(value) == "1";
        }())
        , mCpuMipUseEvidenceEnabled([] {
            const char* value = std::getenv("OPENMW_HITCH_COMPOSITE_CPU_MIPS");
            return value && std::string_view(value) == "1";
        }())
        , mTargetFrameRate(120)
        , mMinimumTimeAvailable(0.0025)
        , mStepBudget(std::make_shared<CompositeStepBudget>())
    {
        setSupportsDisplayList(false);
        setCullingActive(false);
        setName("TerrainCompositeMapRenderer");

        mFBO = new osg::FrameBufferObject;
    }

    CompositeMapRenderer::~CompositeMapRenderer()
    {
        if (progressEnabled())
        {
            const auto stats = preparationStats();
            Log(Debug::Info) << std::dec << "OptimizedMW composite progress: scheduled=" << stats.scheduled
                << " descriptor_builds=" << stats.descriptorBuilds << " capacity_declines=" << stats.capacityDeclines
                << " polls=" << stats.polls << " parked_skips=" << stats.parkedSkips
                << " consumed_layers=" << stats.consumedLayers << " completed_maps=" << stats.completedMaps
                << " required_fallbacks=" << stats.requiredFallbacks << " invalidated=" << stats.invalidated
                << " cancelled=" << stats.cancelled << " pressure_yields=" << stats.pressureYields
                << " pending_bytes=" << stats.pendingBytes;
            Log(Debug::Info) << std::dec << "OptimizedMW composite pressure refusals: checks=" << stats.pressureRefusalChecks
                << " missing_generation=" << stats.pressureRefusals[0]
                << " physical_invalid=" << stats.pressureRefusals[1]
                << " physical_total_zero=" << stats.pressureRefusals[2]
                << " physical_available_invalid=" << stats.pressureRefusals[3]
                << " commit_invalid=" << stats.pressureRefusals[4]
                << " low_memory_notification=" << stats.pressureRefusals[5]
                << " background_non_normal=" << stats.pressureRefusals[6]
                << " background_admissions_zero=" << stats.pressureRefusals[7]
                << " near_future_disallowed=" << stats.pressureRefusals[8]
                << " missing_sampler=" << stats.pressureRefusals[9]
                << " reported_normal=" << stats.pressureStates[0]
                << " reported_caution=" << stats.pressureStates[1]
                << " reported_critical=" << stats.pressureStates[2]
                << " reported_recovering=" << stats.pressureStates[3]
                << " reported_degraded=" << stats.pressureStates[4]
                << " unreported_state=" << stats.pressureUnreportedState;
        }
        std::lock_guard<std::mutex> pendingLock(mPreparationMutex);
        for (const auto& map : mPendingPreparations)
            if (map->mPreparation)
            {
                map->mPreparation->lifetime->cancel();
                map->mPreparationGeneration.fetch_add(1, std::memory_order_acq_rel);
            }
        for (const auto& reference : mPendingStepPreparations)
            if (osg::ref_ptr<CompositeMap> map; reference.lock(map) && map->mPreparation && map->mPreparation->step)
            {
                if (progressEnabled()) reportProgressTerminal(*map, "shutdown");
                map->mPreparation->lifetime->cancel();
                if (map->mPreparation->step->wave) map->mPreparation->step->wave->lifetime->cancel();
                map->mPreparationGeneration.fetch_add(1, std::memory_order_acq_rel);
            }
    }

    void CompositeMapRenderer::releaseGLObjects(osg::State* state) const
    {
        std::lock_guard<std::mutex> lock(mMutex);
        std::lock_guard<std::mutex> pendingLock(mPreparationMutex);
        for (const auto& map : mPendingPreparations)
            if (map->mPreparation)
            {
                map->mPreparation->lifetime->cancel();
                map->mPreparationGeneration.fetch_add(1, std::memory_order_acq_rel);
                map->mPreparation->fbo->releaseGLObjects(state);
                map->mCompiled = 0;
            }
        for (const auto& reference : mPendingStepPreparations)
            if (osg::ref_ptr<CompositeMap> map; reference.lock(map) && map->mPreparation && map->mPreparation->step)
            {
                if (progressEnabled()) reportProgressTerminal(*map, "context_release");
                map->mPreparation->lifetime->cancel();
                if (map->mPreparation->step->wave) map->mPreparation->step->wave->lifetime->cancel();
                map->mPreparationGeneration.fetch_add(1, std::memory_order_acq_rel);
                map->mPreparation->fbo->releaseGLObjects(state);
                map->mCompiled = 0;
            }
        mFBO->releaseGLObjects(state);
        osg::Drawable::releaseGLObjects(state);
    }

    void CompositeMapRenderer::drawImplementation(osg::RenderInfo& renderInfo) const
    {
        using Clock = Debug::V3Diagnostics::Clock;
        auto& telemetryWriter = Debug::V3Diagnostics::p9CompositeWriter();
        const bool telemetryEnabled = telemetryWriter.enabled();
        const auto telemetryStart = telemetryEnabled ? Clock::now() : Clock::time_point{};
        const std::uint64_t yieldsStart = mBackgroundYields.load(std::memory_order_relaxed);
        CompileTelemetry telemetry;
        const bool preparationActive = preparationSupported(*renderInfo.getState());
        const osg::FrameStamp* ownerStamp = renderInfo.getState()->getFrameStamp();
        const bool progressActive = preparationActive && progressEnabled() && ownerStamp;
        const unsigned ownerFrame = ownerStamp ? ownerStamp->getFrameNumber() : 0;
        if (preparationActive)
        {
            const double headroom = std::max(0.0, mICO->compositeTargetFrameMs()
                - renderInfo.getState()->getGraphicsContext()->getTimeSinceLastClear() * 1000.0);
            mAdmission->begin(renderInfo.getState()->getGraphicsContext(), ownerFrame,
                std::min(mICO->compositeBudgetMs(), headroom * mICO->compositeHeadroomRatio()));
            const auto maintenanceStart = Clock::now();
            retirePreparations();
            if (progressActive) mAdmission->charge(Debug::V3Diagnostics::elapsedMs(maintenanceStart));
        }

        double dt = mTimer.time_s();
        dt = std::min(dt, 0.2);
        mTimer.setStartTick();
        double targetFrameTime = 1.0 / static_cast<double>(mTargetFrameRate);
        double conservativeTimeRatio(0.75);
        double availableTime = std::max((targetFrameTime - dt) * conservativeTimeRatio, mMinimumTimeAvailable);

        const auto frameDeadline = std::chrono::steady_clock::now()
            + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(availableTime));
        std::unique_lock<std::mutex> lock(mMutex);

        if (mImmediateCompileSet.empty() && mCompileSet.empty())
            return;

        const std::size_t immediateStart = mImmediateCompileSet.size();
        const std::size_t queuedStart = mCompileSet.size();
        if (preparationActive && !progressActive)
            for (const auto& map : mCompileSet)
                if (!map->mQueueFrameKnown)
                {
                    map->mQueuedFrame = ownerFrame;
                    map->mQueueFrameKnown = true;
                }

        while (!mImmediateCompileSet.empty())
        {
            osg::ref_ptr<CompositeMap> node = *mImmediateCompileSet.begin();
            mImmediateCompileSet.erase(node);

            lock.unlock();
            compileUntil(*node, renderInfo, Deadline::max(), telemetryEnabled ? &telemetry : nullptr);
            lock.lock();
        }

        const auto deadline = mCooperativeBackgroundCompile ? frameDeadline
            : std::chrono::steady_clock::now()
                + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(availableTime));
        std::unordered_set<CompositeMap*> attempted;
        if (progressActive)
            advanceBackground(renderInfo, deadline, ownerFrame, &telemetry, lock);
        else while (!mCompileSet.empty() && std::chrono::steady_clock::now() < deadline)
        {
            auto selected = mCompileSet.begin();
            if (preparationActive)
            {
                selected = mCompileSet.end();
                for (auto it = mCompileSet.begin(); it != mCompileSet.end(); ++it)
                    if (!attempted.contains(it->get()) && (selected == mCompileSet.end()
                        || (*it)->mQueuedFrame < (*selected)->mQueuedFrame))
                        selected = it;
                if (selected == mCompileSet.end())
                    break;
            }
            osg::ref_ptr<CompositeMap> node = *selected;
            attempted.insert(node.get());
            mCompileSet.erase(node);

            lock.unlock();
            compileUntil(*node, renderInfo, mCooperativeBackgroundCompile ? deadline : Deadline::max(),
                telemetryEnabled ? &telemetry : nullptr);
            lock.lock();

            if (node->mCompiled < node->mDrawables.size())
            {
                // We did not compile the map fully.
                // Place it back to queue to continue work in the next time.
                if ((mCooperativeBackgroundCompile || preparationActive) && node->mRequired.load(std::memory_order_acquire))
                    mImmediateCompileSet.insert(node);
                else
                    mCompileSet.insert(node);
            }
        }
        // A cull-side promotion may have happened after compileUntil decided
        // to yield while the map was temporarily outside both queues. Drain
        // those required maps before this renderer returns to visible drawing.
        if (mCooperativeBackgroundCompile || preparationActive)
        {
            while (!mImmediateCompileSet.empty())
            {
                osg::ref_ptr<CompositeMap> node = *mImmediateCompileSet.begin();
                mImmediateCompileSet.erase(node);
                lock.unlock();
                compileUntil(*node, renderInfo, Deadline::max(), telemetryEnabled ? &telemetry : nullptr);
                lock.lock();
            }
        }

        const std::size_t immediateEnd = mImmediateCompileSet.size();
        const std::size_t queuedEnd = mCompileSet.size();
        unsigned oldestAge = 0;
        if (preparationActive && (!progressActive || telemetryEnabled))
            for (const auto& map : mCompileSet)
                if (map->mQueueFrameKnown && ownerFrame >= map->mQueuedFrame)
                    oldestAge = std::max(oldestAge, ownerFrame - map->mQueuedFrame);
        lock.unlock();
        if (preparationActive)
        {
            const auto maintenanceStart = Clock::now();
            retirePreparations();
            if (progressActive) mAdmission->charge(Debug::V3Diagnostics::elapsedMs(maintenanceStart));
        }
        mTimer.setStartTick();

        if (telemetryEnabled)
        {
            const double totalMs = Debug::V3Diagnostics::elapsedMs(telemetryStart);
            if (totalMs >= 0.20 || telemetry.maps != 0)
            {
                osg::State* state = renderInfo.getState();
                const osg::FrameStamp* stamp = state ? state->getFrameStamp() : nullptr;
                const unsigned frame = stamp ? stamp->getFrameNumber() : 0;
                const unsigned context = state ? state->getContextID() : 0;
                const std::uint64_t yieldsEnd = mBackgroundYields.load(std::memory_order_relaxed);
                std::ostringstream row;
                row << frame << ',' << Debug::V3Diagnostics::epochMs() << ',' << context << ','
                    << std::fixed << std::setprecision(4) << totalMs << ',' << availableTime * 1000.0 << ','
                    << immediateStart << ',' << queuedStart << ',' << telemetry.maps << ','
                    << telemetry.requiredMaps << ',' << telemetry.drawables << ',' << telemetry.fboMs << ','
                    << telemetry.stateMs << ',' << telemetry.drawMs << ','
                    << (yieldsEnd >= yieldsStart ? yieldsEnd - yieldsStart : 0) << ','
                    << immediateEnd << ',' << queuedEnd << ',' << mPreparationScheduled.load(std::memory_order_relaxed) << ','
                    << mPreparationStats.invalidated << ',' << mPreparationStats.cancelled << ','
                    << mPreparationStats.requiredFallbacks << ',' << mPreparationStats.ageProgress << ','
                    << mPreparationStats.budgetYields << ',' << preparationStats().pendingBytes << ','
                    << (preparationActive ? mAdmission->chargedMs() : 0) << ',' << oldestAge << ','
                    << mDescriptorBuilds.load(std::memory_order_relaxed) << ','
                    << mCapacityDeclines.load(std::memory_order_relaxed) << ','
                    << mPreparationStats.polls << ',' << mPreparationStats.parkedSkips << ','
                    << mPreparationStats.consumedLayers << ',' << mPreparationStats.completedMaps << ','
                    << mPressureYields.load(std::memory_order_relaxed);
                telemetryWriter.writeLine(row.str());
            }
        }
    }

    void CompositeMapRenderer::advanceBackground(osg::RenderInfo& info, Deadline deadline, unsigned frame,
        CompileTelemetry* telemetry, std::unique_lock<std::mutex>& lock) const
    {
        // Canonical mCompileSet membership is retained while parked. Cull-side
        // setImmediate therefore finds parked maps, and the existing in-flight
        // requiredness check covers promotion while this owner is unlocked.
        constexpr std::size_t MaximumVisits = 16;
        constexpr double MaximumMaintenanceMs = .25;
        if (mAdmission->compositeMaintenanceVisits() >= MaximumVisits || mAdmission->bootstrapUsed()
            || mAdmission->remainingMs() <= 0) return;
        const double maintenanceLimit = std::min(MaximumMaintenanceMs, mAdmission->remainingMs() * .125);
        double maintenanceMs = mAdmission->compositeMaintenanceMs();
        if (maintenanceMs >= maintenanceLimit) return;
        const auto pressureGeneration = mPressureContext.sample
            ? mPressureContext.sample(mPressureContext.owner).generation : 0;
        std::array<osg::ref_ptr<CompositeMap>, sMaxPreparationMaps> ready;
        std::size_t readyCount = 0, readyIndex = 0;
        const auto readyStart = std::chrono::steady_clock::now();
        {
            std::lock_guard pendingLock(mPreparationMutex);
            // Only the admitted eight-map pool is inspected, rather than the
            // entire waiting set. Completion publishes a weak qualified wake.
            for (const auto& reference : mPendingStepPreparations)
            {
                osg::ref_ptr<CompositeMap> map;
                if (!reference.lock(map) || !mCompileSet.contains(map) || !map->mPreparation
                    || !map->mPreparation->step) continue;
                const auto& wave = map->mPreparation->step->wave;
                if ((wave && wave->completed) || map->mProgressWait.eligible(frame,
                    map->mProgressWake.load(std::memory_order_acquire),
                    mStepBudget->creditGeneration.load(std::memory_order_acquire), pressureGeneration))
                {
                    if (readyCount == ready.size()) break;
                    ready[readyCount++] = std::move(map);
                }
            }
        }
        const double readyMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - readyStart).count();
        maintenanceMs += readyMs;
        mAdmission->chargeCompositeMaintenance(readyMs);
        std::array<CompositeMap*, MaximumVisits> visited{};
        std::size_t visitedCount = 0;
        for (std::size_t visits = 0; visits < MaximumVisits && !mCompileSet.empty()
            && std::chrono::steady_clock::now() < deadline && !mAdmission->bootstrapUsed()
            && mAdmission->remainingMs() > 0 && maintenanceMs < maintenanceLimit
            && mAdmission->takeCompositeMaintenanceVisit(MaximumVisits); ++visits)
        {
            const auto selectionStart = std::chrono::steady_clock::now();
            CompileSet::iterator selected = mCompileSet.end();
            while (readyIndex < readyCount && selected == mCompileSet.end())
                selected = mCompileSet.find(ready[readyIndex++]);
            if (selected == mCompileSet.end())
            {
                osg::ref_ptr<CompositeMap> cursor;
                if (mProgressCursor.lock(cursor)) selected = mCompileSet.upper_bound(cursor);
                if (selected == mCompileSet.end()) selected = mCompileSet.begin();
            }
            osg::ref_ptr<CompositeMap> node = *selected;
            mProgressCursor = node;
            if (!node->mQueueFrameKnown)
            {
                node->mQueuedFrame = frame;
                node->mQueueFrameKnown = true;
            }
            const auto wakeBeforeCheck = node->mProgressWake.load(std::memory_order_acquire);
            const auto creditBeforeCheck = mStepBudget->creditGeneration.load(std::memory_order_acquire);
            const bool duplicate = std::find(visited.begin(), visited.begin() + visitedCount, node.get())
                != visited.begin() + visitedCount;
            const bool eligible = !duplicate && (node->mRequired.load(std::memory_order_acquire)
                || node->mProgressWait.eligible(frame, wakeBeforeCheck, creditBeforeCheck, pressureGeneration));
            const double selectedMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - selectionStart).count();
            maintenanceMs += selectedMs;
            mAdmission->chargeCompositeMaintenance(selectedMs);
            if (!eligible)
            {
                ++mPreparationStats.parkedSkips;
                continue;
            }
            visited[visitedCount++] = node.get();
            ++mPreparationStats.polls;
            node->mProgressRefusal = CompositeProgressWait::Reason::None;
            const auto drawablesBefore = telemetry ? telemetry->drawables : 0;
            mCompileSet.erase(selected);
            lock.unlock();
            const auto attemptStart = std::chrono::steady_clock::now();
            compileUntil(*node, info, deadline, telemetry);
            const double attemptMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - attemptStart).count();
            lock.lock();
            const auto bookkeepingStart = std::chrono::steady_clock::now();
            if (!telemetry || telemetry->drawables == drawablesBefore)
            {
                maintenanceMs += attemptMs;
                // compileUntil already charged actual submission time. Record
                // only the maintenance sub-account without charging it twice.
                mAdmission->chargeCompositeMaintenance(attemptMs, false);
            }
            if (node->mCompiled < node->mDrawables.size())
            {
                if (node->mRequired.load(std::memory_order_acquire))
                    mImmediateCompileSet.insert(node);
                else
                {
                    auto reason = node->mProgressRefusal;
                    if (reason == CompositeProgressWait::Reason::None)
                    {
                        const auto& preparation = node->mPreparation;
                        reason = preparation && preparation->step && preparation->step->wave
                            && !preparation->step->wave->completed
                            ? CompositeProgressWait::Reason::Dependency : CompositeProgressWait::Reason::FrameBudget;
                    }
                    if (node->mProgressWait.reason() != reason)
                    {
                        Debug::HitchTrace::Scope refusal("composite.progress_park", Debug::HitchTrace::CpuTiming::WallEnvelope);
                        refusal.retain(); refusal.setLink(node->mPreparation ? node->mPreparation->receipt : node->mPreparationReceipt);
                        refusal.setMetric(0, "generation", node->mPreparation ? node->mPreparation->generation : 0);
                        refusal.setMetric(1, "reason", static_cast<unsigned>(reason));
                        refusal.setMetric(2, "credit_generation", creditBeforeCheck);
                        refusal.setMetric(3, "pressure_generation", pressureGeneration);
                        if (reason == CompositeProgressWait::Reason::Pressure)
                            reportPressureEvidence(node->mProgressPressure, refusal.id(), "park");
                    }
                    node->mProgressWait.park(reason, frame, wakeBeforeCheck, creditBeforeCheck, pressureGeneration);
                    mCompileSet.insert(node);
                }
            }
            else
            {
                node->mProgressWait.ready();
                if (node->mProgressCompletedTick.load(std::memory_order_acquire)
                    && !node->mRequired.load(std::memory_order_acquire))
                    ++mPreparationStats.completedMaps;
            }
            const double bookkeepingMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - bookkeepingStart).count();
            maintenanceMs += bookkeepingMs;
            mAdmission->chargeCompositeMaintenance(bookkeepingMs);
        }
    }

    void CompositeMapRenderer::compile(CompositeMap& compositeMap, osg::RenderInfo& renderInfo) const
    {
        compileUntil(compositeMap, renderInfo, Deadline::max(), nullptr);
    }

    void CompositeMapRenderer::compileUntil(
        CompositeMap& compositeMap, osg::RenderInfo& renderInfo, Deadline deadline, CompileTelemetry* telemetry) const
    {
        if (compositeMap.mDrawables.empty()) return;
        const bool preparationActive = preparationSupported(*renderInfo.getState());
        const bool batchActive = mCompositeBatchEnabled && compositeMap.mBatchingEligible;
        const auto admissionStart = std::chrono::steady_clock::now();
        struct SubmissionCharge
        {
            Resource::P9DiscretionaryAdmission* admission;
            std::chrono::steady_clock::time_point start;
            ~SubmissionCharge()
            {
                if (admission)
                    admission->charge(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
            }
        } charge{ preparationActive ? mAdmission.get() : nullptr, admissionStart };
        if (telemetry)
        {
            ++telemetry->maps;
            if (compositeMap.mRequired.load(std::memory_order_acquire))
                ++telemetry->requiredMaps;
        }

        // if there are no more external references we can assume the texture is no longer required
        int retainedTargetReferences = 1;
        if (preparationActive)
        {
            if (compositeMap.mPreparation && compositeMap.mPreparation->fbo->hasAttachment(osg::Camera::COLOR_BUFFER)
                && compositeMap.mPreparation->fbo->getAttachment(osg::Camera::COLOR_BUFFER).getTexture() == compositeMap.mTexture.get())
                ++retainedTargetReferences;
            if (mFBO->hasAttachment(osg::Camera::COLOR_BUFFER)
                && mFBO->getAttachment(osg::Camera::COLOR_BUFFER).getTexture() == compositeMap.mTexture.get())
                ++retainedTargetReferences;
            if (compositeMap.mPreparation && compositeMap.mPreparation->step
                && compositeMap.mPreparation->target.get() == compositeMap.mTexture.get()
                && (compositeMap.mPreparation->step->targetDebt
                    || (compositeMap.mPreparation->step->wave && compositeMap.mPreparation->step->wave->terminal)))
                ++retainedTargetReferences;
        }
        if (compositeMap.mTexture->referenceCount() <= retainedTargetReferences)
        {
            compositeMap.mCompiled = compositeMap.mDrawables.size();
            if (preparationActive)
            {
                if (compositeMap.mPreparation)
                    compositeMap.mPreparation->lifetime->cancel();
                compositeMap.mDrawables.clear();
            }
            return;
        }

        const bool required = compositeMap.mRequired.load(std::memory_order_acquire);
        // Partial RTT pixels are reusable only while every consumed source,
        // selected program variant and target/context remains exactly current.
        // Required promotion cannot inherit stale pixels from an earlier layer.
        if (compositeMap.mPreparation && compositeMap.mPreparation->step
            && !consumedStepUnchanged(*compositeMap.mPreparation, compositeMap, *renderInfo.getState()))
        {
            compositeMap.mCompiled = 0;
            compositeMap.mPreparation->lifetime->cancel();
            if (compositeMap.mPreparation->step->wave) compositeMap.mPreparation->step->wave->lifetime->cancel();
            ++mPreparationStats.invalidated;
        }
        Debug::HitchTrace::Scope slice("composite.map_slice", Debug::HitchTrace::CpuTiming::WallEnvelope);
        slice.setLink(compositeMap.mPreparation ? compositeMap.mPreparation->receipt : compositeMap.mPreparationReceipt);
        slice.setMetric(0, "required", required);
        slice.setMetric(1, "remaining_passes", compositeMap.mDrawables.size() - compositeMap.mCompiled);
        const auto* ownedPreparation = compositeMap.mPreparation.get();
        const auto dependencyCount = ownedPreparation
            ? (ownedPreparation->step ? ownedPreparation->step->dependencies.size() : ownedPreparation->dependencies.size()) : 0;
        slice.setMetric(2, "pending_submissions", ownedPreparation
            ? dependencyCount + 2 - std::min(dependencyCount + 2, ownedPreparation->submittedCount()) : 0);
        slice.setMetric(3, "preparation_ready", preparationActive && dependenciesReady(compositeMap, *renderInfo.getState()));
        if (required) slice.retain();
        if (preparationActive && !required && !prepare(compositeMap, renderInfo))
            return;
        if (preparationActive && required && !dependenciesReady(compositeMap, *renderInfo.getState()))
        {
            // Required terrain retains the same complete synchronous bake. A
            // speculative preparation must never delay or partially publish it.
            ++mPreparationStats.requiredFallbacks;
            if (compositeMap.mPreparation)
            {
                compositeMap.mPreparation->terminalReason = "required_fallback";
                compositeMap.mPreparation->lifetime->cancel();
            }
        }
        const auto layerPreparation = compositeMap.mPreparation;
        const auto layerWave = layerPreparation && layerPreparation->step ? layerPreparation->step->wave : nullptr;
        const std::size_t bakeEnd = preparationActive && !required && layerWave && !layerWave->terminal
            && !layerPreparation->failed && !layerPreparation->lifetime->cancelled()
            ? layerWave->layerEnd : compositeMap.mDrawables.size();

        auto admitted = [&](bool includeFbo) {
            if (!preparationActive || compositeMap.mRequired.load(std::memory_order_acquire))
                return true;
            const double elapsed = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - admissionStart).count();
            const double predicted = std::max({ .05, mBakeEmaMs * 1.25,
                std::min(mBakeRiskMs, std::max(.25, mBakeEmaMs * 4.0)) })
                + (includeFbo ? std::max(mFboEmaMs * 1.25,
                    std::min(mFboRiskMs, std::max(.25, mFboEmaMs * 4.0))) : 0)
                + (batchActive ? std::max(mBatchCleanupEmaMs * 1.25,
                    std::min(mBatchCleanupRiskMs, std::max(.25, mBatchCleanupEmaMs * 4.0))) : 0);
            if (predicted <= std::max(0.0, mAdmission->remainingMs() - elapsed))
                return true;
            const auto* stamp = renderInfo.getState()->getFrameStamp();
            const unsigned frame = stamp ? stamp->getFrameNumber() : 0;
            const unsigned age = compositeMap.mQueueFrameKnown && frame >= compositeMap.mQueuedFrame
                ? frame - compositeMap.mQueuedFrame : 0;
            if (mAdmission->takeProgress(age, mICO->compositeMaxQueueAge()))
            {
                ++mPreparationStats.ageProgress;
                return true;
            }
            ++mPreparationStats.budgetYields;
            return false;
        };
        if (!admitted(true))
            return;

        osg::Timer timer;
        osg::State& state = *renderInfo.getState();
        osg::GLExtensions* ext = state.get<osg::GLExtensions>();
        std::vector<osg::ref_ptr<osg::Texture::TextureObject>> usedTextures;
        std::vector<osg::ref_ptr<osg::Program::PerContextProgram>> usedPrograms;
        auto recordProducerUseAfterCleanup = [&](std::size_t begin, std::size_t end) {
            if (!mCpuMipUseEvidenceEnabled) return;
            const bool demanded = compositeMap.mRequired.load(std::memory_order_acquire);
            for (std::size_t layer = begin; layer < end; ++layer)
            {
                const auto* drawable = compositeMap.mDrawables[layer].get();
                const auto* pass = drawable ? drawable->getStateSet() : nullptr;
                if (!pass) continue;
                for (unsigned unit = 0; unit < pass->getTextureAttributeList().size(); ++unit)
                    if (const auto* texture = dynamic_cast<const Resource::PreparedTerrainTexture*>(
                        pass->getTextureAttribute(unit, osg::StateAttribute::TEXTURE)))
                        texture->recordProducerDrawUse(state, demanded);
            }
        };
        auto recordLayerUse = [&](osg::StateSet* pass) {
            if (!mIncrementalPreparationEnabled || !pass) return;
            for (unsigned unit = 0; unit < pass->getTextureAttributeList().size(); ++unit)
                if (auto* texture = dynamic_cast<osg::Texture*>(pass->getTextureAttribute(unit, osg::StateAttribute::TEXTURE)))
                    if (auto* object = texture->getTextureObject(state.getContextID()); object
                        && std::none_of(usedTextures.begin(), usedTextures.end(), [&](const auto& old) { return old.get() == object; }))
                        usedTextures.emplace_back(object);
            if (auto* program = dynamic_cast<osg::Program*>(pass->getAttribute(osg::StateAttribute::PROGRAM)))
                if (auto* pcp = program->getPCP(state); pcp
                    && std::none_of(usedPrograms.begin(), usedPrograms.end(), [&](const auto& old) { return old.get() == pcp; }))
                    usedPrograms.emplace_back(pcp);
        };
        auto consumedAfterCleanup = [&] {
            if (!mIncrementalPreparationEnabled) return;
            const bool complete = compositeMap.mCompiled == compositeMap.mDrawables.size();
            if (complete)
                if (auto* object = compositeMap.mTexture->getTextureObject(state.getContextID())) usedTextures.emplace_back(object);
            // This receipt follows draw calls AND caller/array/default-FBO
            // restoration. It establishes actual layer use, never GPU completion.
            mStepBudget->consumed(usedTextures, usedPrograms);
            const auto waveUsed = [&](const CompositeDependencyWave& wave) {
                return std::all_of(wave.newTextureObjects.begin(), wave.newTextureObjects.end(), [&](const auto& weak) {
                    osg::ref_ptr<osg::Texture::TextureObject> owner;
                    return !weak.lock(owner) || std::any_of(usedTextures.begin(), usedTextures.end(),
                        [&](const auto& used) { return used == owner; }); })
                    && std::all_of(wave.newPrograms.begin(), wave.newPrograms.end(), [&](const auto& weak) {
                        osg::ref_ptr<osg::Program::PerContextProgram> owner;
                        return !weak.lock(owner) || std::any_of(usedPrograms.begin(), usedPrograms.end(),
                            [&](const auto& used) { return used == owner; }); });
            };
            if (layerWave && !layerWave->terminal && compositeMap.mCompiled >= layerWave->layerEnd && waveUsed(*layerWave))
            {
                const bool demanded = compositeMap.mRequired.load(std::memory_order_acquire);
                const bool cancelled = layerPreparation->lifetime->cancelled() || layerWave->lifetime->cancelled();
                const bool qualified = layerWave->completed && !cancelled && !demanded && !layerPreparation->failed
                    && layerPreparation->generation == compositeMap.mPreparationGeneration.load(std::memory_order_acquire)
                    && layerPreparation->context.get() == state.getGraphicsContext();
                if (qualified) ++mPreparationStats.consumedLayers;
                if (progressEnabled() && qualified && !compositeMap.mProgressFirstLayerTick.load(std::memory_order_relaxed))
                    compositeMap.mProgressFirstLayerTick.store(progressTick(), std::memory_order_release);
                layerWave->consumed = true;
                Debug::HitchTrace::Scope use("composite.layer_consumed", Debug::HitchTrace::CpuTiming::WallEnvelope);
                use.retain(); use.setLink(layerWave->receipt);
                use.setMetric(0, "layer", layerWave->layerBegin);
                use.setMetric(1, "source_bytes", layerWave->bytes);
                use.setMetric(2, "generation", layerPreparation->generation);
                use.setMetric(3, "map_complete", complete);
                if (progressEnabled())
                {
                    Debug::HitchTrace::Scope outcome("composite.layer_outcome", Debug::HitchTrace::CpuTiming::WallEnvelope);
                    outcome.retain(); outcome.setLink(use.id());
                    outcome.setMetric(0, "wave_completed", layerWave->completed);
                    outcome.setMetric(1, "required", demanded);
                    outcome.setMetric(2, "cancelled", cancelled);
                    outcome.setMetric(3, "qualified_optional", qualified);
                    if (complete && qualified)
                        compositeMap.mProgressCompletedTick.store(progressTick(), std::memory_order_release);
                }
            }
            if (complete && layerPreparation && layerPreparation->step)
            {
                const auto& target = layerPreparation->step->targetDebt;
                if (target && waveUsed(*target)) target->consumed = true;
                if (layerWave && layerWave->terminal && waveUsed(*layerWave)) layerWave->consumed = true;
            }
        };

        if (!mFBO)
            return;

        if (!ext->isFrameBufferObjectSupported)
            return;

        const auto fboStart = telemetry || preparationActive ? Debug::V3Diagnostics::Clock::now() : Debug::V3Diagnostics::Clock::time_point{};
        osg::FrameBufferObject* fbo = preparationActive && dependenciesReady(compositeMap, state)
            ? compositeMap.mPreparation->fbo.get() : mFBO.get();
        if (fbo == mFBO.get())
            fbo->setAttachment(osg::Camera::COLOR_BUFFER, osg::FrameBufferAttachment(compositeMap.mTexture));
        fbo->apply(state, osg::FrameBufferObject::DRAW_FRAMEBUFFER);

        GLenum status = ext->glCheckFramebufferStatus(GL_FRAMEBUFFER_EXT);
        if (telemetry)
            telemetry->fboMs += Debug::V3Diagnostics::elapsedMs(fboStart);
        if (preparationActive)
        {
            const double actual = Debug::V3Diagnostics::elapsedMs(fboStart);
            mFboEmaMs = mFboSamples++ == 0 ? actual : mFboEmaMs * .85 + actual * .15;
            mFboRiskMs = std::max(actual, mFboRiskMs * .9);
        }

        if (status != GL_FRAMEBUFFER_COMPLETE_EXT)
        {
            GLuint fboId = state.getGraphicsContext() ? state.getGraphicsContext()->getDefaultFboId() : 0;
            ext->glBindFramebuffer(GL_FRAMEBUFFER_EXT, fboId);
            OSG_ALWAYS << "Error attaching FBO" << std::endl;
            return;
        }

        // inform State that Texture attribute has changed due to compiling of FBO texture
        // should OSG be doing this on its own?
        state.haveAppliedTextureAttribute(state.getActiveTextureUnit(), osg::StateAttribute::TEXTURE);

        if (batchActive)
        {
            Debug::HitchTrace::Scope batchSpan("composite.batch", Debug::HitchTrace::CpuTiming::WallEnvelope);
            batchSpan.retain();
            batchSpan.setMetric(0, "start_layer", compositeMap.mCompiled);
            batchSpan.setMetric(1, "layers", compositeMap.mDrawables.size());
            batchSpan.setMetric(2, "required", compositeMap.mRequired.load(std::memory_order_acquire));
            // Only this map/FBO is batched. Every layer still resolves its own
            // inherited StateSet and draws in the original order. The map keeps
            // every producer alive until caller state and arrays are restored.
            struct BatchCleanup
            {
                osg::State& state;
                osg::GLExtensions& ext;
                CompileTelemetry* telemetry;
                bool measure;
                double& ema;
                double& risk;
                unsigned& samples;
                bool pushed = false, applied = false, finished = false;

                void finish()
                {
                    if (finished) return;
                    finished = true;
                    Debug::HitchTrace::Scope cleanupSpan("composite.batch_cleanup", Debug::HitchTrace::CpuTiming::WallEnvelope);
                    cleanupSpan.retain();
                    const auto start = measure ? Debug::V3Diagnostics::Clock::now()
                                               : Debug::V3Diagnostics::Clock::time_point{};
                    if (pushed) { state.popStateSet(); pushed = false; }
                    // Array/FBO retirement also executes if a user attribute's
                    // caller restore throws. Producers remain retained on unwind.
                    try
                    {
                        if (applied)
                        {
                            Debug::HitchTrace::Scope restoreSpan("composite.caller_restore", Debug::HitchTrace::CpuTiming::WallEnvelope);
                            state.apply();
                        }
                    }
                    catch (...)
                    {
                        {
                            Debug::HitchTrace::Scope arraysSpan("composite.array_retire", Debug::HitchTrace::CpuTiming::WallEnvelope);
                            state.disableAllVertexArrays();
                        }
                        state.haveAppliedAttribute(osg::StateAttribute::VIEWPORT);
                        ext.glBindFramebuffer(GL_FRAMEBUFFER_EXT,
                            state.getGraphicsContext() ? state.getGraphicsContext()->getDefaultFboId() : 0);
                        throw;
                    }
                    if (applied)
                    {
                        Debug::HitchTrace::Scope arraysSpan("composite.array_retire", Debug::HitchTrace::CpuTiming::WallEnvelope);
                        state.disableAllVertexArrays();
                    }
                    state.haveAppliedAttribute(osg::StateAttribute::VIEWPORT);
                    ext.glBindFramebuffer(GL_FRAMEBUFFER_EXT,
                        state.getGraphicsContext() ? state.getGraphicsContext()->getDefaultFboId() : 0);
                    if (measure)
                    {
                        const double actual = Debug::V3Diagnostics::elapsedMs(start);
                        if (telemetry) telemetry->stateMs += actual;
                        ema = samples++ == 0 ? actual : ema * .85 + actual * .15;
                        risk = std::max(actual, risk * .9);
                    }
                }
                ~BatchCleanup() noexcept
                {
                    // During a draw exception preserve the original exception;
                    // ordinary completion invokes finish explicitly below.
                    try { finish(); } catch (...) {}
                }
            } cleanup{ state, *ext, telemetry, telemetry || preparationActive,
                mBatchCleanupEmaMs, mBatchCleanupRiskMs, mBatchCleanupSamples };
            const std::size_t batchBegin = compositeMap.mCompiled;
            SceneUtil::prepareClientArrayDraw(state);
            bool firstBake = true;
            for (std::size_t i = batchBegin; i < bakeEnd; ++i)
            {
                if (!firstBake && !admitted(false)) break;
                firstBake = false;
                if (deadline != Deadline::max() && !compositeMap.mRequired.load(std::memory_order_acquire)
                    && std::chrono::steady_clock::now() >= deadline)
                {
                    mBackgroundYields.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
                osg::Drawable* drawable = compositeMap.mDrawables[i];
                osg::StateSet* pass = drawable->getStateSet();
                const auto bakeStart = std::chrono::steady_clock::now();
                if (pass) { state.pushStateSet(pass); cleanup.pushed = true; }
                cleanup.applied = true;
                const auto stateStart = telemetry ? Debug::V3Diagnostics::Clock::now()
                                                 : Debug::V3Diagnostics::Clock::time_point{};
                {
                    Debug::HitchTrace::Scope stateSpan("composite.pass_state_viewport", Debug::HitchTrace::CpuTiming::WallEnvelope);
                    stateSpan.setMetric(0, "layer", i);
                    state.apply();
                    if (telemetry) telemetry->stateMs += Debug::V3Diagnostics::elapsedMs(stateStart);
                    // Keep the old per-pass viewport override: a pass attribute or
                    // texture callback may have changed it during State::apply().
                    glViewport(0, 0, compositeMap.mTexture->getTextureWidth(), compositeMap.mTexture->getTextureHeight());
                }
                const auto drawStart = telemetry ? Debug::V3Diagnostics::Clock::now()
                                                : Debug::V3Diagnostics::Clock::time_point{};
                {
                    Debug::HitchTrace::Scope drawSpan("composite.pass_draw", Debug::HitchTrace::CpuTiming::WallEnvelope);
                    drawSpan.setMetric(0, "layer", i);
                    drawable->drawImplementation(renderInfo);
                }
                recordLayerUse(pass);
                if (telemetry)
                {
                    telemetry->drawMs += Debug::V3Diagnostics::elapsedMs(drawStart);
                    ++telemetry->drawables;
                }
                if (pass) { state.popStateSet(); cleanup.pushed = false; }
                if (preparationActive)
                {
                    const double actual = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - bakeStart).count();
                    mBakeEmaMs = mBakeSamples++ == 0 ? actual : mBakeEmaMs * .85 + actual * .15;
                    mBakeRiskMs = std::max(actual, mBakeRiskMs * .9);
                }
                ++compositeMap.mCompiled;
            }
            // Includes cleanup in SubmissionCharge and the measured state span.
            // The separate cleanup prior is reserved at every admission check.
            cleanup.finish();
            recordProducerUseAfterCleanup(batchBegin, compositeMap.mCompiled);
            consumedAfterCleanup();
            {
                Debug::HitchTrace::Scope retireSpan("composite.producer_retire", Debug::HitchTrace::CpuTiming::WallEnvelope);
                retireSpan.setMetric(0, "layers", compositeMap.mCompiled - batchBegin);
                if (!preparationActive)
                    for (std::size_t i = batchBegin; i < compositeMap.mCompiled; ++i)
                        compositeMap.mDrawables[i] = nullptr;
                if (compositeMap.mCompiled == compositeMap.mDrawables.size())
                    compositeMap.mDrawables.clear();
            }
            return;
        }

        const std::size_t normalBegin = compositeMap.mCompiled;
        SceneUtil::prepareClientArrayDraw(state);
        bool firstBake = true;
        for (size_t i = compositeMap.mCompiled; i < bakeEnd; ++i)
        {
            if (!firstBake && !admitted(false))
                break;
            firstBake = false;
            if (deadline != Deadline::max() && !compositeMap.mRequired.load(std::memory_order_acquire)
                && std::chrono::steady_clock::now() >= deadline)
            {
                mBackgroundYields.fetch_add(1, std::memory_order_relaxed);
                break;
            }
            osg::Drawable* drw = compositeMap.mDrawables[i];
            osg::StateSet* stateset = drw->getStateSet();
            const auto bakeStart = std::chrono::steady_clock::now();

            if (stateset)
                renderInfo.getState()->pushStateSet(stateset);

            const auto stateStart = telemetry ? Debug::V3Diagnostics::Clock::now() : Debug::V3Diagnostics::Clock::time_point{};
            renderInfo.getState()->apply();
            if (telemetry)
                telemetry->stateMs += Debug::V3Diagnostics::elapsedMs(stateStart);

            glViewport(0, 0, compositeMap.mTexture->getTextureWidth(), compositeMap.mTexture->getTextureHeight());
            const auto drawStart = telemetry ? Debug::V3Diagnostics::Clock::now() : Debug::V3Diagnostics::Clock::time_point{};
            drw->drawImplementation(renderInfo);
            recordLayerUse(stateset);
            if (telemetry)
            {
                telemetry->drawMs += Debug::V3Diagnostics::elapsedMs(drawStart);
                ++telemetry->drawables;
            }

            const auto restoreStart = telemetry ? Debug::V3Diagnostics::Clock::now()
                                                : Debug::V3Diagnostics::Clock::time_point{};
            if (stateset)
            {
                renderInfo.getState()->popStateSet();
                // State caches the last applied Program and PCP as raw pointers.
                // Restore the caller while this one-shot pass still owns them;
                // completing the map below can release its last source owner.
                renderInfo.getState()->apply();
            }
            // Composite quads use client arrays. Clear the dispatchers' raw
            // Array pointers before these temporary geometries can be retired.
            state.disableAllVertexArrays();
            if (telemetry)
                telemetry->stateMs += Debug::V3Diagnostics::elapsedMs(restoreStart);

            if (preparationActive)
            {
                const double actual = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - bakeStart).count();
                mBakeEmaMs = mBakeSamples++ == 0 ? actual : mBakeEmaMs * .85 + actual * .15;
                mBakeRiskMs = std::max(actual, mBakeRiskMs * .9);
            }

            ++compositeMap.mCompiled;

        }
        state.haveAppliedAttribute(osg::StateAttribute::VIEWPORT);

        GLuint fboId = state.getGraphicsContext() ? state.getGraphicsContext()->getDefaultFboId() : 0;
        ext->glBindFramebuffer(GL_FRAMEBUFFER_EXT, fboId);
        recordProducerUseAfterCleanup(normalBegin, compositeMap.mCompiled);
        consumedAfterCleanup();
        if (!preparationActive)
            for (std::size_t i = normalBegin; i < compositeMap.mCompiled; ++i)
                compositeMap.mDrawables[i] = nullptr;
        if (compositeMap.mCompiled == compositeMap.mDrawables.size())
            compositeMap.mDrawables = std::vector<osg::ref_ptr<osg::Drawable>>();
    }

    void CompositeMapRenderer::setMinimumTimeAvailableForCompile(double time)
    {
        mMinimumTimeAvailable = time;
    }

    void CompositeMapRenderer::setTargetFrameRate(float framerate)
    {
        mTargetFrameRate = framerate;
    }

    void CompositeMapRenderer::addCompositeMap(CompositeMap* compositeMap, bool immediate)
    {
        std::lock_guard<std::mutex> lock(mMutex);
        if (immediate)
        {
            if (progressEnabled())
            {
                const bool previouslyRequired = compositeMap->mRequired.exchange(true, std::memory_order_acq_rel);
                if (!previouslyRequired) reportFirstDemand(*compositeMap);
            }
            else compositeMap->mRequired.store(true, std::memory_order_release);
            mCompileSet.erase(compositeMap);
            mImmediateCompileSet.insert(compositeMap);
        }
        else if (!mImmediateCompileSet.contains(compositeMap))
            mCompileSet.insert(compositeMap);
    }

    void CompositeMapRenderer::setImmediate(CompositeMap* compositeMap)
    {
        if (progressEnabled())
        {
            const bool previouslyRequired = compositeMap->mRequired.exchange(true, std::memory_order_acq_rel);
            if (!previouslyRequired) reportFirstDemand(*compositeMap);
        }
        else compositeMap->mRequired.store(true, std::memory_order_release);
        std::lock_guard<std::mutex> lock(mMutex);
        CompileSet::iterator found = mCompileSet.find(compositeMap);
        if (found == mCompileSet.end())
            return;
        else
        {
            mImmediateCompileSet.insert(compositeMap);
            mCompileSet.erase(found);
        }
    }

    size_t CompositeMapRenderer::getCompileSetSize() const
    {
        std::lock_guard<std::mutex> lock(mMutex);
        return mCompileSet.size();
    }

    CompositeMap::CompositeMap()
        : mCompiled(0)
    {
    }

    CompositeMap::~CompositeMap()
    {
        if (mPreparation && mPreparation->step)
        {
            if (mProgressReceiptGeneration.load(std::memory_order_acquire))
                reportProgressTerminal(*this, "expired");
            mPreparation->lifetime->cancel();
            if (mPreparation->step->wave) mPreparation->step->wave->lifetime->cancel();
        }
    }

}
