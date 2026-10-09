#ifndef OPENMW_SCENEUTIL_GENERATEDSTATICLOD_H
#define OPENMW_SCENEUTIL_GENERATEDSTATICLOD_H

#include <osg/Group>
#include <osg/Geometry>
#include <osgUtil/IncrementalCompileOperation>
#include <atomic>
#include <memory>
#include <mutex>
#include <array>
#include <string_view>
#include <string>
#include <span>
#include <vector>
#include <osg/observer_ptr>
#include <osg/Camera>
#include <osg/StateSet>
namespace osg { class Camera; class Program; }
namespace osgUtil { class CullVisitor; }
namespace Resource::AssetGpuBudget { class Reservation; }
namespace Resource { struct SpeculativeReservationReceipt; }

namespace SceneUtil::GeneratedStaticLod
{
    enum class Refusal { None, Graph, Material, Transform, Layout, Capacity, NoReduction, Count };
    enum class BuildStep { None, Scope, Inspection, VertexOwnership, Peak, HostAdmission, Cook, Reduction,
        GpuAdmission, GpuRetention, Lineage, Replay, Guard, Publication, Count };
    const char* buildStepName(BuildStep value);
    enum class Detail { None, ShaderFamily, ShaderEpoch, ShaderSource, ShaderStage, ShaderBindings, ShaderDefines,
        ShaderFeatures, ShaderCapacity, Alpha, Blend, Dynamic, GraphCallback, GraphType, HiddenGraph, Layout, Other, Count };
    enum class GuardPhase { None, Preflight, Snapshot, Publication };
    enum class GuardRefusal { None, PathCapacity, NullNode, ChildCapacity, StreamCapacity, StateCapacity,
        UniformRecipe, DefineCapacity, AttributeRecipe, TextureCapacity, TextureClass, TextureImage,
        TextureMips, TextureFormat, TextureDynamic, MetadataCapacity, TransformRecipe, ShaderMissing,
        ShaderImports, ShaderCertificate, SnapshotAllocation, SnapshotException, Stale, Count };
    const char* guardRefusalName(GuardRefusal value);
    const char* guardPhaseName(GuardPhase value);
    // Fixed copied context survives rollback/destruction. Never retain a failed
    // graph or an unbounded source/material string just to diagnose refusal.
    struct GuardRefusalInfo
    {
        GuardRefusal reason = GuardRefusal::None;
        std::array<char, 64> nodeClass{}, attributeClass{};
        std::array<char, 128> nodeName{}, programName{};
        unsigned textureUnit = ~0u, attributeType = ~0u, shaderRefusal = 0;
        std::uint64_t observed = 0, limit = 0;
    };
    struct RefusalInfo
    {
        Detail detail = Detail::None;
        const osg::Node* node = nullptr;
        const osg::Program* program = nullptr;
    };
    enum class PostOptimizerStep { Chunks, Contributor, ContributorMissingSource, ContributorNonmerge,
        ContributorOccluder, ContributorOther, CandidateMaterial, OrdinaryMaterial, MixedMaterial,
        IdentityCapacity, TemplateCensusCapacity, MergeRootState, Children, MaterialOwnership,
        PrivateNodeCapacity, PrivateNodeParents, PrivateGeometryCapacity, PrivateTemplateGeometry,
        PrivateGeometryParents, PrivateEmpty, Certification, Candidate, Count };
    const char* postOptimizerStepName(PostOptimizerStep step);
    void notePostOptimizer(PostOptimizerStep step, const osg::Node* node = nullptr,
        std::uint64_t observed = 0, std::uint64_t limit = 0, std::string_view reason = {}) noexcept;
    void notePostOptimizerCertification(Refusal refusal, const RefusalInfo& info) noexcept;
    std::string postOptimizerSummary();
    struct Counters
    {
        inline static std::atomic_uint64_t requests{0}, certified{0}, built{0}, diskHits{0};
        inline static std::atomic_uint64_t originalIndices{0}, derivedIndices{0}, coarseViews{0}, originalViews{0};
        inline static std::atomic_uint64_t derivedDraws{0}, submittedIndices{0};
        inline static std::atomic_uint64_t materialCullFallbacks{0}, materialDrawFallbacks{0};
        inline static std::atomic_uint64_t controlDraws{0}, controlSubmittedIndices{0};
        // Ownership diagnostics count chunk proofs, not distinct source models.
        inline static std::atomic_uint64_t materialOwnerUnknown{0}, materialOwnerNull{0}, materialOwnerCapacity{0};
        inline static std::atomic_uint64_t materialOwnerShared{0}, materialOwnerCohesions{0}, materialOwnerCohesiveInstances{0};
        inline static std::atomic_uint64_t postOptimizedCandidates{0}, privateArrayCopies{0}, privateVertexBuffers{0};
        inline static std::atomic_uint64_t newIndexGpuBytes{0};
        inline static std::atomic_uint64_t guardRefused[static_cast<unsigned>(GuardRefusal::Count)]{};
        inline static std::atomic_bool firstGuardFailure[static_cast<unsigned>(GuardRefusal::Count)]{};
        inline static std::atomic_uint64_t buildFailures[static_cast<unsigned>(BuildStep::Count)]{};
        inline static std::atomic_bool firstBuildFailure[static_cast<unsigned>(BuildStep::Count)]{};
        inline static std::atomic_uint64_t refused[static_cast<unsigned>(Refusal::Count)]{};
        inline static std::atomic_uint64_t postOptimizer[static_cast<unsigned>(PostOptimizerStep::Count)]{};
        inline static std::atomic_bool firstPostOptimizer[static_cast<unsigned>(PostOptimizerStep::Count)]{};
        inline static std::atomic_uint64_t postCertification[static_cast<unsigned>(Refusal::Count)]{};
        inline static std::atomic_uint64_t postCertificationDetail[static_cast<unsigned>(Detail::Count)]{};
        inline static std::atomic_bool firstPostCertificationDetail[static_cast<unsigned>(Detail::Count)]{};
    };
    // Run on the original SceneManager template, before paging CopyOp erases provenance.
    Refusal certify(const osg::Node& original, RefusalInfo* info = nullptr);
    Detail shaderDetail(unsigned reason);
    const char* detailName(Detail value);
    // A replay owns only original nodes, never its derived callback owner.
    // Paths preserve effective state and relative transforms for the exact leaf.
    struct OriginalDraw
    {
        osg::ref_ptr<osg::Geometry> geometry;
        std::vector<osg::ref_ptr<const osg::Node>> path;
        osg::Matrixd matrix;
    };
    std::vector<OriginalDraw> originalDraws(const osg::Node& root);
    // Candidate-private control receipt, installed before immutable guard capture.
    // No original loader template or disabled lane is decorated.
    bool installOriginalDrawReceipts(osg::Node& root);
    void drawOriginals(osg::RenderInfo& info, std::span<const OriginalDraw> draws,
        const osg::Matrixd& derivedMatrix, const osg::StateSet* restoreLeaf,
        std::span<const osg::ref_ptr<const osg::StateSet>> candidateStates = {});
    class MaterialGuard
    {
    public:
        static std::shared_ptr<MaterialGuard> capture(const osg::Node& original, const osg::Node& derived,
            bool requireShader = true, GuardRefusalInfo* refusal = nullptr);
        static std::shared_ptr<MaterialGuard> captureDraws(std::span<const OriginalDraw> original,
            const OriginalDraw& derived, bool requireShader = true, GuardRefusalInfo* refusal = nullptr);
        // Same bounded recipe contract as final capture, on the exact ordinary
        // post-optimizer control. Final capture is repeated after copying and
        // installing lineage/callbacks; this does not authorize a tier.
        static bool preflight(const osg::Node& original, GuardRefusalInfo* refusal = nullptr);
        bool current(osg::State* state = nullptr,
            const std::vector<const osg::StateSet*>& inherited = {}) const;
    private:
        struct Impl;
        explicit MaterialGuard(std::shared_ptr<Impl> impl) : mImpl(std::move(impl)) {}
        std::shared_ptr<Impl> mImpl;
    };
    std::size_t sourceBytes(const osg::Node& original);
    // Conservative immutable clone, peak leaf simplifier/cache overlap and
    // metadata estimate; zero means the original graph/layout is unqualified.
    std::uint64_t cookPeakBytes(const osg::Node& original);
    bool positiveUniformTransform(const osg::Matrixd& matrix);
    bool isDelegatingDrawCallback(const osg::Drawable::DrawCallback* callback);
    // Temporal coverage must describe the geometry that the exact callback
    // will submit. A stale derived receipt replays original draws instead.
    // This only reads the frozen recipe; it never refreshes a failed guard.
    bool temporalDrawRecipeCurrent(const osg::Geometry& geometry,
        const std::vector<const osg::StateSet*>& inherited = {}, osg::State* drawState = nullptr);
    struct MaterialOwner { const osg::StateSet* state = nullptr; unsigned owners = 0; };
    // Bit0 is ordinary population; bits1..16 are bounded private partitions.
    // Unknown ownership refuses all partitions. Content-equivalent states must
    // retain their whole-chunk strong merge opportunity.
    unsigned conflictingMaterialOwners(std::span<const MaterialOwner> materials, bool known = true);
    struct MaterialOwnershipReport
    {
        unsigned conflicting = 0, firstOwners = 0, secondOwners = 0;
        bool unknown = false, nullMaterial = false, capacity = false, shared = false;
    };
    enum class MaterialOwnershipRefusal { None, GlobalUnknown, GlobalNull, GlobalCapacity, InvalidOwner,
        ChildUnknown, ChildNull, ChildCapacity, ChildEmpty, MaterialNotFound, OrdinaryOwner, MixedOwner,
        DifferentOwner, Count };
    const char* materialOwnershipRefusalName(MaterialOwnershipRefusal reason);
    struct MaterialOwnershipInfo
    {
        MaterialOwnershipRefusal reason = MaterialOwnershipRefusal::None;
        unsigned requestedOwner = 0, observedOwners = 0;
        std::size_t originalMaterials = 0, childMaterials = 0;
    };
    void notePostOptimizerOwnership(const osg::Node& node, const MaterialOwnershipInfo& refusal) noexcept;
    // Asset-only proof on the actual CopyOp output. It does not change the
    // control AnalyzeVisitor's historical merge benefit heuristic. Each leaf
    // gets its complete effective state including its own StateSet, with exact
    // OSG OVERRIDE/PROTECTED precedence and branch-local ancestor restoration.
    // Metadata is bounded to 256 distinct states/4 MiB within the existing
    // optional chunk Stage; attributes/uniform payloads are borrowed, not copied.
    class EffectiveMaterialOwners
    {
    public:
        void observe(const osg::Node& root, unsigned owner, unsigned mask = ~0u);
        void replaceOwner(unsigned from, unsigned to);
        // The complete optimized child must consist of effective materials
        // proved on approved original contributors before CopyOp. A material
        // also used by an ordinary/unapproved contributor refuses the child.
        bool exclusivelyOwns(const osg::Node& root, unsigned owner, unsigned mask = ~0u,
            MaterialOwnershipInfo* refusal = nullptr) const;
        std::span<const MaterialOwner> materials() const { return mMaterials; }
        MaterialOwnershipReport report() const;
    private:
        std::vector<osg::ref_ptr<osg::StateSet>> mStates;
        std::vector<MaterialOwner> mMaterials;
        std::size_t mBytes = 0;
        bool mUnknown = false, mNull = false, mCapacity = false;
    };
    struct Readiness
    {
        std::atomic_bool ready{false}, everReady{false};
        unsigned assetMask = 0;
    };
    class CompileCompleted final : public osgUtil::IncrementalCompileOperation::CompileCompletedCallback
    {
    public:
        explicit CompileCompleted(std::shared_ptr<Readiness> readiness) : mReadiness(std::move(readiness)) {}
        bool compileCompleted(osgUtil::IncrementalCompileOperation::CompileSet*) override;
    private:
        std::shared_ptr<Readiness> mReadiness;
    };
    // Shared main-view selector also supports a separately certified HLOD representation.
    class Group final : public osg::Group
    {
    public:
        Group() = default;
        Group(const Group& copy, const osg::CopyOp& op);
        META_Node(SceneUtil, Group)
        Group(osg::Node& original, osg::Node& derived, float absoluteError,
            std::shared_ptr<Readiness> readiness, osg::Camera* mainCamera, bool preserveControlPopulation = false,
            GuardRefusalInfo* refusal = nullptr);
        void traverse(osg::NodeVisitor& visitor) override;
        osg::BoundingSphere computeBound() const override;
        void releaseGLObjects(osg::State* state = nullptr) const override;
        osg::Node* original() { return getNumChildren() ? getChild(0) : nullptr; }
        const osg::Node* original() const { return getNumChildren() ? getChild(0) : nullptr; }
        osg::Node* derived() { return getNumChildren() > 1 ? getChild(1) : nullptr; }
        const osg::Node* derived() const { return getNumChildren() > 1 ? getChild(1) : nullptr; }
        bool wantsDerived(osgUtil::CullVisitor& visitor);
        float error() const noexcept { return mError; }
        bool compilationReady() const noexcept
        { return mReadiness && mReadiness->ready.load(std::memory_order_acquire); }
        bool materialGuardCurrent() const
        { return mMaterialGuard && mMaterialGuard->current(); }
    private:
        float mError = 0.f;
        std::shared_ptr<Readiness> mReadiness;
        std::shared_ptr<MaterialGuard> mMaterialGuard;
        osg::observer_ptr<osg::Camera> mMainCamera;
        bool mPreserveControlPopulation = false;
        struct View { const osgUtil::CullVisitor* owner = nullptr; bool coarse = false; };
        std::array<View, 8> mViews{};
        std::mutex mViewMutex;
    };
    struct Result
    {
        osg::ref_ptr<Group> node;
        std::shared_ptr<Readiness> readiness;
        std::size_t originalIndices = 0, derivedIndices = 0;
        std::uint64_t newIndexBytes = 0;
        Refusal refusal = Refusal::None;
        bool retryable = false;
        BuildStep failureStep = BuildStep::None;
        std::uint32_t gpuRefusalReasons = 0;
        bool allocationFailure = false;
        GuardPhase guardPhase = GuardPhase::None;
        GuardRefusalInfo guardRefusal;
        std::shared_ptr<Resource::AssetGpuBudget::Reservation> gpuReservation;
    };
    // One bounded first receipt per failed stage and inner guard reason;
    // subsequent failures retain fixed counters. GPU retirement uses this gate.
    void recordBuildFailure(BuildStep step, Result& result, std::string_view sourceIdentity,
        std::uint64_t peakBytes = 0, const Resource::SpeculativeReservationReceipt* cpuReceipt = nullptr) noexcept;
    // Worker-only; caller supplies a certified, chunk-private optimized control
    // graph. Its immutable arrays/buffers may be shared with ordinary templates;
    // no shared template drawable/callback/user-data owner may be decorated.
    // Every stream remains owned by original; only primitive indices are replaced.
    // reserveGpu=false is for unsubmitted CPU/GL fixtures, never production paging.
    Result build(osg::Node& original, std::string_view sourceIdentity, osg::Camera* mainCamera, bool reserveGpu = true);
}
#endif
