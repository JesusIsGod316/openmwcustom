#ifndef OPENMW_SHADER_ASSETGEOMETRYCONTRACT_H
#define OPENMW_SHADER_ASSETGEOMETRYCONTRACT_H

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <osg/Program>
#include <osg/StateSet>

namespace osg { class State; }
namespace Shader::AssetGeometry
{
    // This contract authorizes index LOD / material atlases only. It is not an
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
    };
    // Bounded diagnostic view of this Shader's existing proof; no disk access
    // and no source copy. The Shader retains the returned name's owner.
    SourceStatus sourceStatus(const osg::Shader& shader);

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
