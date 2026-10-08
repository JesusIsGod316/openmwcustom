#include "assetgeometrycontract.hpp"
#include "assetgeometrybaseline.hpp"
#include "programvariant.hpp"
#include "indirectprojectioncontract.hpp"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <new>
#include <stdexcept>
#include <osg/Shader>
#include <osg/UserDataContainer>
#include <components/resource/assetpolicy.hpp>
#include <components/resource/derivedassetcache.hpp>

namespace Shader::AssetGeometry
{
    namespace
    {
        std::shared_mutex producerMutex;
        thread_local unsigned producerReaders = 0, producerWriters = 0;
        constexpr const char* SourceKey = "OptimizedMW.AssetGeometry.Source.v1";
        constexpr const char* ProgramKey = "OptimizedMW.AssetGeometry.Program.v1";
        std::atomic_uint64_t metadata{0};
        struct Charge
        {
            std::uint64_t bytes;
            explicit Charge(std::uint64_t value) : bytes(value)
            {
                auto used = metadata.load(std::memory_order_relaxed);
                do
                {
                    if (value > MaximumMetadataBytes || used > MaximumMetadataBytes - value) throw std::bad_alloc{};
                } while (!metadata.compare_exchange_weak(used, used + value, std::memory_order_relaxed));
            }
            ~Charge() { metadata.fetch_sub(bytes, std::memory_order_relaxed); }
        };
        auto reserve(std::size_t bytes) { return std::make_shared<Charge>(bytes + sizeof(Charge) + 64); }
        std::string digest(std::string_view value) { return Resource::DerivedAssets::digest(value); }
        bool regenerationEpoch(const std::shared_ptr<Epoch>& epoch, std::uint64_t previousVersion)
        {
            return epoch && previousVersion && previousVersion != UINT64_MAX
                && epoch->value.load(std::memory_order_acquire) == previousVersion + 1;
        }
        bool numericParameters(std::string_view text)
        {
            // The reviewed object producer inputs are numeric literals or the
            // comma-separated shadow texture list. An empty list disables it.
            // Do not let an otherwise pinned template acquire GLSL statements
            // through a writable @ substitution or imported define value.
            if (text.empty()) return true;
            std::size_t position = 0;
            const auto digit = [](char value) {return value >= '0' && value <= '9';};
            for (;;)
            {
                while (position < text.size() && text[position] == ' ') ++position;
                if (position < text.size() && (text[position] == '+' || text[position] == '-')) ++position;
                bool digits = false;
                while (position < text.size() && digit(text[position])) {++position;digits = true;}
                if (position < text.size() && text[position] == '.')
                {
                    ++position;
                    while (position < text.size() && digit(text[position])) {++position;digits = true;}
                }
                if (!digits) return false;
                if (position < text.size() && (text[position] == 'e' || text[position] == 'E'))
                {
                    ++position;
                    if (position < text.size() && (text[position] == '+' || text[position] == '-')) ++position;
                    const auto begin = position;
                    while (position < text.size() && digit(text[position])) ++position;
                    if (begin == position) return false;
                }
                while (position < text.size() && text[position] == ' ') ++position;
                if (position == text.size()) return true;
                if (text[position++] != ',') return false;
            }
        }
        bool bounded(const Defines& values, std::size_t& bytes)
        {
            if (values.size() > 256) return false;
            for (const auto& [name,value] : values)
            {
                if (name.size() > 1024 || value.size() > 4096 || !numericParameters(value)) return false;
                bytes += name.size() + value.size() + 128;
                if (bytes > MaximumRecordBytes) return false;
            }
            return true;
        }
        std::string recipe(const Defines& values)
        {
            std::string result;
            for (const auto& [name,value] : values) { result += name; result += '\0'; result += value; result += '\0'; }
            return digest(result);
        }
        struct SourceData
        {
            std::shared_ptr<Charge> charge;
            std::shared_ptr<Epoch> epoch;
            std::uint64_t version = 0;
            unsigned families = 0;
            std::string name, sourceDigest, rawDigest, expandedDigest, defineDigest;
            // Shared once per Shader, never copied into a leaf snapshot. The
            // stock projection Source is immutable storage only: asset family
            // authorization still comes exclusively from our raw recipe.
            osg::ref_ptr<const IndirectProjection::Source> borrowedSource;
            std::string source;
            Defines effective;
            osg::Shader::Type type = osg::Shader::UNDEFINED;
            osg::Shader::ShaderDefinesMode mode{};
            osg::ShaderDefines defines, requirements;
            Refusal current(const osg::Shader& shader, const std::shared_ptr<Epoch>& owner,
                std::uint64_t expectedVersion) const
            {
                if (!epoch || epoch != owner || version != expectedVersion) return Refusal::Epoch;
                if (shader.getType() != type || (type != osg::Shader::VERTEX && type != osg::Shader::FRAGMENT)) return Refusal::Stage;
                if (shader.getShaderBinary() || !shader.getCodeInjectionMap().empty()
                    || shader.getShaderSource().size() > 2u * 1024u * 1024u
                    || (borrowedSource ? !borrowedSource->matches(shader)
                                       : shader.getShaderSource() != source)) return Refusal::Source;
                if (shader.getShaderDefinesMode() != mode || shader.getShaderDefines() != defines
                    || shader.getShaderRequirements() != requirements) return Refusal::Defines;
                return Refusal::None;
            }
            Refusal current(const osg::Shader& shader) const
            { return current(shader, epoch, epoch ? epoch->value.load(std::memory_order_acquire) : 0); }
        };
        class Source final : public osg::Object
        {
        public:
            Source() = default;
            Source(const Source& other, const osg::CopyOp& op) : osg::Object(other,op), data(other.data) {}
            explicit Source(std::shared_ptr<const SourceData> value) : data(std::move(value)) {setName(SourceKey);}
            META_Object(Shader, Source)
            std::shared_ptr<const SourceData> data;
        };
        struct ProgramData
        {
            struct Producer { osg::ref_ptr<const osg::Shader> shader; std::shared_ptr<const SourceData> source; };
            std::shared_ptr<Charge> charge;
            std::vector<Producer> producers;
            osg::Program::AttribBindingList attributes;
            osg::Program::FragDataBindingList fragments;
            osg::Program::UniformBlockBindingList blocks;
            osg::ShaderDefines defines;
            Family family = Family::Stock;
            Samplers samplers;
            std::string identity;
            std::shared_ptr<const SourceData> objectSource;
            GLenum feedbackMode = GL_SEPARATE_ATTRIBS;
            std::array<GLint,3> geometryParameters{};
            Refusal linkingCurrent(const osg::Program& program) const
            {
                if (!ProgramLinkIdentity::boundedCaptureSize(program)) return Refusal::Capacity;
                if ((program.getProgramBinary() && program.getProgramBinary()->getSize())
                    || program.getNumTransformFeedBackVaryings()
                    || program.getTransformFeedBackMode() != feedbackMode
                    || geometryParameters != std::array<GLint,3>{
                        program.getParameter(GL_GEOMETRY_VERTICES_OUT_EXT),
                        program.getParameter(GL_GEOMETRY_INPUT_TYPE_EXT),
                        program.getParameter(GL_GEOMETRY_OUTPUT_TYPE_EXT)}) return Refusal::Stage;
                if (attributes != program.getAttribBindingList() || fragments != program.getFragDataBindingList()
                    || blocks != program.getUniformBlockBindingList()) return Refusal::Bindings;
                if (defines != program.getShaderDefines()) return Refusal::Defines;
                const auto* samplerProducer = dynamic_cast<const SamplerProducer*>(&program);
                if (!samplerProducer || !samplerProducer->assetSamplersMatch(samplers)) return Refusal::Bindings;
                return Refusal::None;
            }
            Refusal current(const osg::Program& program, const std::shared_ptr<Epoch>* owner = nullptr,
                std::uint64_t expectedVersion = 0) const
            {
                if (const auto refusal = linkingCurrent(program); refusal != Refusal::None) return refusal;
                if (program.getNumShaders() != producers.size()) return Refusal::Stage;
                for (unsigned i = 0; i < producers.size(); ++i)
                {
                    const auto& producer = producers[i];
                    if (program.getShader(i) != producer.shader.get()) return Refusal::Source;
                    const auto refusal = owner ? producer.source->current(*producer.shader, *owner, expectedVersion)
                                               : producer.source->current(*producer.shader);
                    if (refusal != Refusal::None) return refusal;
                }
                return Refusal::None;
            }
        };
        class Program final : public osg::Object
        {
        public:
            Program() = default;
            Program(const Program& other, const osg::CopyOp& op) : osg::Object(other,op), data(other.data) {}
            explicit Program(std::shared_ptr<const ProgramData> value) : data(std::move(value)) {setName(ProgramKey);}
            META_Object(Shader, Program)
            std::shared_ptr<const ProgramData> data;
        };
        const Program* certificate(const osg::Program& program)
        {
            const auto* data = program.getUserDataContainer();
            return data ? dynamic_cast<const Program*>(data->getUserObject(ProgramKey)) : nullptr;
        }
        void publish(osg::Object& owner, const char* key, osg::Object* value)
        {
            osg::ref_ptr<osg::Object> owned = value;
            auto* user = owner.getOrCreateUserDataContainer();
            const auto index = user->getUserObjectIndex(key);
            if (index < user->getNumUserObjects()) user->setUserObject(index, owned);
            else user->addUserObject(owned);
        }
        bool safeImports(const ProgramDefineSnapshot& values)
        {
            if (!values.bounded) return false;
            for (const auto& [name,value] : values.map.currentDefines)
                if ((value.second & osg::StateAttribute::ON) && !numericParameters(value.first)) return false;
            // DISTORTION changes displaced fragment sampling. Opaque atlases
            // cannot qualify this effect even when its exact source is known.
            const auto found = values.map.currentDefines.find("DISTORTION");
            return found == values.map.currentDefines.end() || !(found->second.second & osg::StateAttribute::ON)
                || found->second.first == "0";
        }
        std::optional<std::size_t> pathScratch(const osg::Program& program,
            const std::vector<const osg::StateSet*>& path)
        {
            const auto text = ProgramDefineSnapshot::boundedPathMetadata(program,path);
            if (!text || *text > MaximumRecordBytes) return {};
            std::size_t entries = 0;
            for (const auto* state : path) if (state) entries += state->getDefineList().size();
            // ResolveStateSets makes temporary StateSets and two map/stack
            // representations, not just define text. Charge all that overlap.
            return 32768 + path.size() * 2048 + entries * 512 + *text * 4;
        }
        std::size_t importedBytes(const ProgramDefineSnapshot& values)
        {
            std::size_t bytes = sizeof(Snapshot) + 4096;
            for (const auto& [name,value] : values.map.currentDefines)
                bytes += 512 + 4 * (name.size() + value.first.size());
            return bytes;
        }
    }
    ProducerReadScope::ProducerReadScope() : mLock(producerMutex, std::defer_lock)
    {
        if (!producerReaders && !producerWriters) mLock.lock();
        ++producerReaders;
    }
    ProducerReadScope::~ProducerReadScope() { --producerReaders; }
    ProducerMutationScope::ProducerMutationScope() : mLock(producerMutex, std::defer_lock)
    {
        if (!producerWriters)
        {
            if (producerReaders) throw std::logic_error("producer lease cannot upgrade a reader to a writer");
            mLock.lock();
        }
        ++producerWriters;
    }
    ProducerMutationScope::~ProducerMutationScope() { --producerWriters; }
    bool enabled()
    {
        static const bool temporalProducer = [] {
            const auto* value = std::getenv("OPENMW_P9_DYNAMIC_MOTION");
            return value && std::string_view(value) == "1";
        }();
        return Resource::AssetPolicy::enabled(Resource::AssetPolicy::VisualLod)
            || Resource::AssetPolicy::enabled(Resource::AssetPolicy::Hlod) || temporalProducer;
    }
    std::uint64_t liveMetadataBytes() { return metadata.load(std::memory_order_relaxed); }
    const char* refusalName(Refusal value)
    {
        constexpr const char* names[]{"none","missing","epoch","source","stage","bindings","defines","features","capacity"};
        const auto index = static_cast<unsigned>(value);
        return index < std::size(names) ? names[index] : "invalid";
    }
    const char* familyName(Family value) { return value == Family::Pbr ? "pbr" : "stock"; }
    RawProof::RawProof()
    {
        if (!enabled()) {families = 0; return;}
        try {admission = reserve(MaximumRecordBytes + 1024);}
        catch (const std::bad_alloc&) {families = 0;}
    }
    void RawProof::observe(std::string_view path, std::string_view raw)
    {
        try
        {
        if (!families) return;
        if (++files > 128 || raw.size() > 2u * 1024u * 1024u - bytes || path.size() > 1024)
        { families = 0; return; }
        bytes += raw.size();
        Resource::DerivedAssets::Sha256 hash;
        for (const char c : raw)
            if (c != '\r') { const auto byte = static_cast<std::uint8_t>(c); hash.append({&byte,1}); }
        const auto actual = hash.finish();
        unsigned accepted = 0;
        for (const auto& entry : Baseline::entries)
            if (entry.path == path)
            {
                if (!entry.stock.empty() && entry.stock == actual) accepted |= Stock;
                if (!entry.pbr.empty() && entry.pbr == actual) accepted |= Pbr;
                break;
            }
        families &= accepted;
        if (recipe.size() + path.size() + actual.size() + 2 > MaximumRecordBytes) {families = 0; return;}
        recipe += path; recipe += '\0'; recipe += actual; recipe += '\0';
        }
        catch (const std::bad_alloc&) {families = 0;}
    }
    TemplateProof finish(const RawProof& raw, std::string_view expanded)
    {
        if (!raw.families || !raw.files || expanded.size() > 2u * 1024u * 1024u) return {};
        try { return {raw.families,digest(raw.recipe),digest(expanded),reserve(512)}; }
        catch (const std::bad_alloc&) { return {}; }
    }
    void attachSource(osg::Shader& shader, std::string_view name, const TemplateProof& proof,
        const Defines& local, const Defines& global, const std::shared_ptr<Epoch>& epoch)
    {
        ProducerMutationScope producer;
        if (!enabled() || !proof.families || !proof.admission || !epoch || name.size() > 1024
            || shader.getShaderSource().size() > 2u * 1024u * 1024u || shader.getShaderBinary()
            || !shader.getCodeInjectionMap().empty()) return;
        std::size_t bytes = sizeof(SourceData) + name.size() + 1024;
        if (!bounded(local,bytes) || !bounded(global,bytes)) return;
        for (const auto* values : {&shader.getShaderDefines(),&shader.getShaderRequirements()})
        {
            if (values->size() > 256) return;
            for (const auto& value : *values)
            {if (value.size() > 1024) return; bytes += value.size() + 128;}
        }
        if (bytes > MaximumRecordBytes) return;
        try
        {
            // Include logical ownership even when immutable stock storage is
            // borrowed from the separately capped projection Source. PBR owns
            // one copy, charged before allocation. Existing 4 MiB cap remains.
            auto charge = reserve(bytes + shader.getShaderSource().capacity() + 64);
            auto data = std::make_shared<SourceData>(); data->charge = std::move(charge);
            data->epoch = epoch; data->version = epoch->value.load(std::memory_order_acquire);
            data->name = name; data->families = proof.families;
            data->sourceDigest = digest(shader.getShaderSource()); data->rawDigest = proof.rawDigest;
            data->expandedDigest = proof.expandedDigest;
            const auto* user = shader.getUserDataContainer();
            const auto* borrowed = user ? dynamic_cast<const IndirectProjection::Source*>(
                user->getUserObject(IndirectProjection::SourceKey)) : nullptr;
            if (borrowed && borrowed->matches(shader)) data->borrowedSource = borrowed;
            else data->source = shader.getShaderSource();
            data->effective = global;
            for (const auto& [key,value] : local) data->effective[key] = value;
            data->defineDigest = recipe(data->effective);
            data->type = shader.getType(); data->mode = shader.getShaderDefinesMode();
            data->defines = shader.getShaderDefines(); data->requirements = shader.getShaderRequirements();
            publish(shader, SourceKey, new Source(std::move(data)));
        }
        catch (const std::bad_alloc&) {} // Optional proof cannot affect normal shader construction.
    }
    SourceStatus sourceStatus(const osg::Shader& shader)
    {
        ProducerReadScope producer;
        const auto* user = shader.getUserDataContainer();
        const auto* proof = user ? dynamic_cast<const Source*>(user->getUserObject(SourceKey)) : nullptr;
        if (!proof || !proof->data) return {};
        return {proof->data->current(shader),proof->data->families,proof->data->name,proof->data};
    }
    bool canRefreshSource(const osg::Shader& shader, const std::shared_ptr<Epoch>& epoch,
        std::uint64_t previousVersion)
    {
        ProducerReadScope producer;
        if (!regenerationEpoch(epoch, previousVersion)) return false;
        const auto* user = shader.getUserDataContainer();
        const auto* proof = user ? dynamic_cast<const Source*>(user->getUserObject(SourceKey)) : nullptr;
        return proof && proof->data && proof->data->current(shader, epoch, previousVersion) == Refusal::None;
    }
    bool canRefreshProgram(const osg::Program& program, const std::shared_ptr<Epoch>& epoch,
        std::uint64_t previousVersion)
    {
        ProducerReadScope producer;
        try
        {
            if (!regenerationEpoch(epoch, previousVersion)) return false;
            const auto* proof = certificate(program);
            return proof && proof->data
                && proof->data->current(program, &epoch, previousVersion) == Refusal::None;
        }
        catch (const std::bad_alloc&) { return false; }
    }
    Refusal refreshProgram(osg::Program& program, const std::shared_ptr<Epoch>& epoch,
        std::uint64_t previousVersion)
    {
        ProducerMutationScope producer;
        try
        {
            if (!regenerationEpoch(epoch, previousVersion)) return Refusal::Epoch;
            const auto* proof = certificate(program);
            if (!proof || !proof->data) return Refusal::Missing;
            // Only sources replaced by this manager's immediately preceding
            // invalidation can authorize refreshing the cached Program slot.
            for (const auto& producer : proof->data->producers)
                if (producer.source->epoch != epoch || producer.source->version != previousVersion)
                    return Refusal::Epoch;
            if (const auto refusal = proof->data->linkingCurrent(program); refusal != Refusal::None) return refusal;
            return certify(program, proof->data->samplers);
        }
        catch (const std::bad_alloc&) { return Refusal::Capacity; }
    }
    Refusal certify(osg::Program& program, const Samplers& samplers)
    {
        ProducerMutationScope producerLease;
        if (!enabled() || !ProgramLinkIdentity::boundedCaptureSize(program) || program.getNumShaders() > 16
            || samplers.size() > 16) return Refusal::Capacity;
        const auto* producer = dynamic_cast<const SamplerProducer*>(&program);
        if (!producer || !producer->assetSamplersMatch(samplers)) return Refusal::Bindings;
        std::size_t bytes = sizeof(ProgramData) + 2048 + program.getNumShaders() * 512;
        for (const auto& [name,unit] : samplers)
        {if (name.size() > 128 || unit < 0 || unit > 31) return Refusal::Capacity; bytes += name.size() + 128;}
        const auto bindings = [&](const auto& values) {
            for (const auto& [name,value] : values) {(void)value; bytes += name.size() + 128;}
        };
        bindings(program.getAttribBindingList()); bindings(program.getFragDataBindingList()); bindings(program.getUniformBlockBindingList());
        if (bytes > MaximumRecordBytes) return Refusal::Capacity;
        try
        {
            auto charge = reserve(bytes);
            auto data = std::make_shared<ProgramData>(); data->charge = std::move(charge);
            unsigned families = AllFamilies; bool objectVertex = false, objectFragment = false, coreVertex = false;
            std::string identity;
            for (unsigned i = 0; i < program.getNumShaders(); ++i)
            {
                const auto* shader = program.getShader(i);
                const auto* user = shader ? shader->getUserDataContainer() : nullptr;
                const auto* source = user ? dynamic_cast<const Source*>(user->getUserObject(SourceKey)) : nullptr;
                if (!source || !source->data) return Refusal::Missing;
                if (const auto refusal = source->data->current(*shader); refusal != Refusal::None) return refusal;
                const auto& record = *source->data;
                families &= record.families;
                objectVertex |= record.type == osg::Shader::VERTEX && record.name == "compatibility/objects.vert";
                objectFragment |= record.type == osg::Shader::FRAGMENT && record.name == "compatibility/objects.frag";
                coreVertex |= record.type == osg::Shader::VERTEX && record.name == "lib/core/vertex.glsl";
                if (record.name == "compatibility/objects.vert") data->objectSource = source->data;
                if (data->objectSource && (data->objectSource->effective != record.effective
                    || data->objectSource->epoch != record.epoch || data->objectSource->version != record.version)) return Refusal::Defines;
                identity += record.name; identity += '\0'; identity += record.rawDigest; identity += record.expandedDigest;
                identity += record.sourceDigest; identity += record.defineDigest;
                data->producers.push_back({shader,source->data});
            }
            if (!families || !objectVertex || !objectFragment || !coreVertex) return Refusal::Source;
            data->family = (families & Pbr) ? Family::Pbr : Family::Stock;
            data->samplers = samplers;
            data->attributes = program.getAttribBindingList(); data->fragments = program.getFragDataBindingList();
            data->blocks = program.getUniformBlockBindingList(); data->defines = program.getShaderDefines();
            data->feedbackMode = program.getTransformFeedBackMode();
            // OSG's ordinary vertex/fragment Program has nonzero geometry
            // parameter defaults. No geometry producer is accepted above;
            // retain these actual linking fields and detect later mutation.
            data->geometryParameters = {program.getParameter(GL_GEOMETRY_VERTICES_OUT_EXT),
                program.getParameter(GL_GEOMETRY_INPUT_TYPE_EXT),program.getParameter(GL_GEOMETRY_OUTPUT_TYPE_EXT)};
            const auto appendBindings = [&](const auto& values) {
                for (const auto& [name,value] : values)
                {identity += name; identity += '\0'; identity += std::to_string(value); identity += '\0';}
            };
            appendBindings(data->attributes); appendBindings(data->fragments); appendBindings(data->blocks); appendBindings(samplers);
            for (const auto& define : data->defines) {identity += define; identity += '\0';}
            identity += std::to_string(data->feedbackMode); identity += '\0';
            for (const auto value : data->geometryParameters) {identity += std::to_string(value); identity += '\0';}
            identity += familyName(data->family); data->identity = digest(identity);
            if (const auto refusal = data->current(program); refusal != Refusal::None) return refusal;
            publish(program, ProgramKey, new Program(std::move(data)));
            return Refusal::None;
        }
        catch (const std::bad_alloc&) {return Refusal::Capacity;}
    }
    Refusal validate(const osg::Program& program)
    {
        ProducerReadScope producer;
        try {const auto* proof = certificate(program); return proof && proof->data ? proof->data->current(program) : Refusal::Missing;}
        catch (const std::bad_alloc&) {return Refusal::Capacity;}
    }
    bool matches(const osg::Program& program) { return validate(program) == Refusal::None; }
    struct SnapshotData
    {
        std::shared_ptr<Charge> charge;
        std::shared_ptr<const ProgramData> proof;
        ProgramDefineSnapshot imported;
        std::size_t bytes = 0;
    };
    Family Snapshot::family() const { return mData ? mData->proof->family : Family::Stock; }
    std::string_view Snapshot::recipe() const { return mData ? std::string_view(mData->proof->identity) : std::string_view{}; }
    std::string_view Snapshot::define(std::string_view name) const
    {
        if (!mData) return {};
        const auto& features = mData->proof->objectSource->effective;
        const auto found = features.find(std::string(name));
        return found == features.end() ? std::string_view{} : std::string_view(found->second);
    }
    const Samplers& Snapshot::samplers() const {static const Samplers empty; return mData ? mData->proof->samplers : empty;}
    std::size_t Snapshot::metadataBytes() const {return mData ? mData->bytes : 0;}
    std::optional<Snapshot> capture(const osg::Program& program, const std::vector<const osg::StateSet*>& path,
        Refusal* refusal)
    {
        ProducerReadScope producer;
        if (refusal) *refusal = Refusal::None;
        const auto fail = [refusal](Refusal reason)->std::optional<Snapshot>
        { if (refusal) *refusal = reason; return {}; };
        try
        {
            const auto* proof = certificate(program);
            if (!proof || !proof->data) return fail(Refusal::Missing);
            if (const auto reason = proof->data->current(program); reason != Refusal::None) return fail(reason);
            const auto size = pathScratch(program,path); if (!size) return fail(Refusal::Capacity);
            const auto scratch = reserve(*size);
            auto imported = ProgramDefineSnapshot::fromStateSets(program,path);
            if (!safeImports(imported)) return fail(Refusal::Defines);
            const auto bytes = importedBytes(imported) + sizeof(SnapshotData);
            if (bytes > MaximumRecordBytes) return fail(Refusal::Capacity);
            auto charge = reserve(bytes);
            auto data = std::make_shared<SnapshotData>(); data->bytes = bytes;
            data->charge = std::move(charge); data->proof = proof->data; data->imported = std::move(imported);
            return Snapshot(std::move(data));
        }
        catch (const std::bad_alloc&) {return fail(Refusal::Capacity);}
    }
    Refusal current(const Snapshot& snapshot, const osg::Program& program, osg::State* state)
    {
        ProducerReadScope producer;
        try
        {
            if (!snapshot.mData) return Refusal::Missing;
            const auto refusal = snapshot.mData->proof->current(program);
            if (refusal != Refusal::None || !state) return refusal;
            const auto& map = state->getDefineMap();
            if (map.map.size() > 1024 || map.currentDefines.size() > 1024) return Refusal::Capacity;
            const auto scratch = reserve(32768 + program.getShaderDefines().size() * (512 + 4 * 4096));
            const auto actual = ProgramDefineSnapshot::capture(*state,program);
            return safeImports(actual) && actual.string == snapshot.mData->imported.string ? Refusal::None : Refusal::Defines;
        }
        catch (const std::bad_alloc&) {return Refusal::Capacity;}
    }
    Refusal current(const Snapshot& snapshot, const osg::Program& program, const std::vector<const osg::StateSet*>& path)
    {
        ProducerReadScope producer;
        try
        {
            if (!snapshot.mData) return Refusal::Missing;
            const auto refusal = snapshot.mData->proof->current(program);
            if (refusal != Refusal::None) return refusal;
            const auto size = pathScratch(program,path); if (!size) return Refusal::Capacity;
            const auto scratch = reserve(*size);
            const auto actual = ProgramDefineSnapshot::fromStateSets(program,path);
            return safeImports(actual) && actual.string == snapshot.mData->imported.string ? Refusal::None : Refusal::Defines;
        }
        catch (const std::bad_alloc&) {return Refusal::Capacity;}
    }
}
