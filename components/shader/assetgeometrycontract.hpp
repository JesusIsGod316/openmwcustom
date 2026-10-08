#ifndef OPENMW_SHADER_ASSETGEOMETRYCONTRACT_H
#define OPENMW_SHADER_ASSETGEOMETRYCONTRACT_H

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>
#include <osg/Program>
#include <osg/StateSet>

namespace osg { class State; }
namespace Shader::AssetGeometry
{
    // Guards mutable Shader/Program producer recipes and their certificate
    // publication. Readers may nest, including under a same-thread writer.
    // Never call ShaderManager while holding a read scope: manager mutations
    // acquire its mutex before this lease. There is no read-to-write upgrade.
    class ProducerReadScope
    {
    public:
        ProducerReadScope();
        ~ProducerReadScope();
        ProducerReadScope(const ProducerReadScope&) = delete;
        ProducerReadScope& operator=(const ProducerReadScope&) = delete;
    private:
        std::shared_lock<std::shared_mutex> mLock;
    };
    class ProducerMutationScope
    {
    public:
        ProducerMutationScope();
        ~ProducerMutationScope();
        ProducerMutationScope(const ProducerMutationScope&) = delete;
        ProducerMutationScope& operator=(const ProducerMutationScope&) = delete;
    private:
        std::unique_lock<std::shared_mutex> mLock;
    };
    // This contract authorizes index LOD / material atlases. A separate temporal
    // domain may borrow only its exact source provenance; this is never an
    // indirect draw, visibility, deformation or temporal projection contract.
    enum class Family { Stock, Pbr };
    enum class Refusal { None, Missing, Epoch, Source, Stage, Bindings, Defines, Features, Capacity, Count };
    using Defines = std::map<std::string, std::string>;
    using Samplers = std::map<std::string, int>;
    bool enabled();
    const char* refusalName(Refusal value);
    const char* familyName(Family value);
    inline constexpr unsigned Stock = 1, Pbr = 2, AllFamilies = Stock | Pbr;
    inline constexpr std::size_t MaximumMetadataBytes = 4u * 1024u * 1024u;
    inline constexpr std::size_t MaximumRecordBytes = 64u * 1024u;
    std::uint64_t liveMetadataBytes();

    struct Epoch { std::atomic_uint64_t value{1}; };
    // Called on the exact bytes consumed by ShaderManager's existing parser;
    // manifests and filenames alone cannot establish this proof.
    struct RawProof
    {
        RawProof();
        unsigned families = AllFamilies;
        std::size_t files = 0, bytes = 0;
        std::string recipe;
        std::shared_ptr<const void> admission;
        void observe(std::string_view path, std::string_view raw);
    };
    struct TemplateProof
    {
        unsigned families = 0;
        std::string rawDigest, expandedDigest;
        std::shared_ptr<const void> admission;
    };
    TemplateProof finish(const RawProof& raw, std::string_view expanded);
    void attachSource(osg::Shader& shader, std::string_view name, const TemplateProof& proof,
        const Defines& local, const Defines& global, const std::shared_ptr<Epoch>& epoch);
    struct SourceStatus
    {
        Refusal refusal = Refusal::Missing;
        unsigned families = 0;
        std::string_view name;
        std::shared_ptr<const void> owner;
    };
    // Bounded diagnostic view of this Shader's existing proof; no disk access
    // and no source copy. The Shader retains the returned name's owner.
    SourceStatus sourceStatus(const osg::Shader& shader);
    // ShaderManager alone replaces approved @ substitutions. Before it writes
    // a cached producer, validate its entire prior immutable recipe against the
    // exact owner/version that has just been invalidated. This is never a way to
    // validate an old candidate snapshot against a new generation.
    bool canRefreshSource(const osg::Shader& shader, const std::shared_ptr<Epoch>& epoch,
        std::uint64_t previousVersion);

    // SamplerProgram's private uniforms are part of its actual linking/apply
    // behavior. A generic Program clone cannot borrow its asset certificate.
    class SamplerProducer
    {
    public:
        virtual bool assetSamplersMatch(const Samplers& values) const = 0;
    protected:
        virtual ~SamplerProducer() = default;
    };
    Refusal certify(osg::Program& program, const Samplers& samplers);
    bool canRefreshProgram(const osg::Program& program, const std::shared_ptr<Epoch>& epoch,
        std::uint64_t previousVersion);
    // Only after canRefreshProgram succeeded before producer regeneration.
    // Retain the prior private sampler and linking schema, then certify all new
    // exact producer recipes. Publication replaces a slot, not its old data.
    Refusal refreshProgram(osg::Program& program, const std::shared_ptr<Epoch>& epoch,
        std::uint64_t previousVersion);
    Refusal validate(const osg::Program& program);
    bool matches(const osg::Program& program);

    struct SnapshotData;
    class Snapshot
    {
    public:
        Snapshot() = default;
        explicit operator bool() const { return bool(mData); }
        Family family() const;
        std::string_view recipe() const;
        std::string_view define(std::string_view name) const;
        const Samplers& samplers() const;
        std::size_t metadataBytes() const;
    private:
        explicit Snapshot(std::shared_ptr<const SnapshotData> data) : mData(std::move(data)) {}
        std::shared_ptr<const SnapshotData> mData;
        friend std::optional<Snapshot> capture(const osg::Program&, const std::vector<const osg::StateSet*>&);
        friend Refusal current(const Snapshot&, const osg::Program&, osg::State*);
        friend Refusal current(const Snapshot&, const osg::Program&, const std::vector<const osg::StateSet*>&);
    };
    std::optional<Snapshot> capture(const osg::Program& program,
        const std::vector<const osg::StateSet*>& path = {});
    Refusal current(const Snapshot& snapshot, const osg::Program& program, osg::State* state = nullptr);
    Refusal current(const Snapshot& snapshot, const osg::Program& program,
        const std::vector<const osg::StateSet*>& path);
}
#endif
