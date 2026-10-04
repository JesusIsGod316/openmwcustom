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
#include <span>
#include <vector>
#include <osg/observer_ptr>
#include <osg/Camera>
namespace osg { class Camera; class Program; }
namespace osgUtil { class CullVisitor; }
namespace Resource::AssetGpuBudget { class Reservation; }

namespace SceneUtil::GeneratedStaticLod
{
    enum class Refusal { None, Graph, Material, Transform, Layout, Capacity, NoReduction, Count };
    enum class Detail { None, ShaderFamily, ShaderEpoch, ShaderSource, ShaderStage, ShaderBindings, ShaderDefines,
        ShaderFeatures, ShaderCapacity, Alpha, Blend, Dynamic, GraphCallback, GraphType, HiddenGraph, Layout, Other, Count };
    struct RefusalInfo
    {
        Detail detail = Detail::None;
        const osg::Node* node = nullptr;
        const osg::Program* program = nullptr;
    };
    struct Counters
    {
        inline static std::atomic_uint64_t requests{0}, certified{0}, built{0}, diskHits{0};
        inline static std::atomic_uint64_t originalIndices{0}, derivedIndices{0}, coarseViews{0}, originalViews{0};
        inline static std::atomic_uint64_t derivedDraws{0}, submittedIndices{0};
        inline static std::atomic_uint64_t materialCullFallbacks{0}, materialDrawFallbacks{0};
        inline static std::atomic_uint64_t controlDraws{0}, controlSubmittedIndices{0};
        inline static std::atomic_uint64_t refused[static_cast<unsigned>(Refusal::Count)]{};
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
            bool requireShader = true);
        static std::shared_ptr<MaterialGuard> captureDraws(std::span<const OriginalDraw> original,
            const OriginalDraw& derived, bool requireShader = true);
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
    struct MaterialOwner { const osg::StateSet* state = nullptr; unsigned owners = 0; };
    // Bit0 is ordinary population; bits1..16 are bounded private partitions.
    // Unknown ownership refuses all partitions. Content-equivalent states must
    // retain their whole-chunk strong merge opportunity.
    unsigned conflictingMaterialOwners(std::span<const MaterialOwner> materials, bool known = true);
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
            std::shared_ptr<Readiness> readiness, osg::Camera* mainCamera, bool preserveControlPopulation = false);
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
        Refusal refusal = Refusal::None;
        bool retryable = false;
        std::shared_ptr<Resource::AssetGpuBudget::Reservation> gpuReservation;
    };
    // Worker-only; caller supplies a certified, private, optimized partition.
    // Every stream remains owned by original; only primitive indices are replaced.
    // reserveGpu=false is for unsubmitted CPU/GL fixtures, never production paging.
    Result build(osg::Node& original, std::string_view sourceIdentity, osg::Camera* mainCamera, bool reserveGpu = true);
}
#endif
