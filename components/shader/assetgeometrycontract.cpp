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
        std::atomic_uint64_t metadata{RegistryMetadataBytes};
        std::atomic_uint64_t metadataPeak{RegistryMetadataBytes};
        constexpr unsigned RefusalCount = static_cast<unsigned>(Refusal::Count);
        std::array<std::atomic_uint64_t, RefusalCount> initialSource{}, regeneratedSource{}, refreshedProgram{};
        class MetadataAdmissionFailed final : public std::bad_alloc {};
        class RegistryCapacityFailed final : public std::bad_alloc {};
        struct Charge
        {
            std::uint64_t bytes;
            explicit Charge(std::uint64_t value) : bytes(value)
            {
                auto used = metadata.load(std::memory_order_relaxed);
                do
                {
                    if (value > MaximumMetadataBytes || used > MaximumMetadataBytes - value) throw MetadataAdmissionFailed{};
                } while (!metadata.compare_exchange_weak(used, used + value, std::memory_order_relaxed));
                const auto total = used + value;
                auto peak = metadataPeak.load(std::memory_order_relaxed);
                while (total > peak && !metadataPeak.compare_exchange_weak(peak, total, std::memory_order_relaxed)) {}
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
        Refusal bounded(const Defines& values, std::size_t& bytes)
        {
            if (values.size() > 256) return Refusal::Capacity;
            for (const auto& [name,value] : values)
            {
                if (name.size() > 1024 || value.size() > 4096) return Refusal::Capacity;
                if (!numericParameters(value)) return Refusal::Defines;
                bytes += name.size() + value.size() + 128;
                if (bytes > MaximumRecordBytes) return Refusal::Capacity;
            }
            return Refusal::None;
        }
        std::string recipe(const Defines& values)
        {
            std::string result;
            for (const auto& [name,value] : values) { result += name; result += '\0'; result += value; result += '\0'; }
            return digest(result);
        }
        struct SourceChunk
        {
            std::shared_ptr<Charge> charge;
            std::string digest, bytes;
        };
        struct SourceText
        {
            std::shared_ptr<Charge> charge;
            std::string digest;
            std::vector<std::shared_ptr<const SourceChunk>> chunks;
            std::size_t bytes = 0;
            bool matches(const osg::Shader& shader) const
            {
                // Exact comparison stays on the existing validation path.
                // Hashes only select an interning candidate during attachment.
                const auto& value = shader.getShaderSource();
                if (bytes != value.size()) return false;
                std::size_t offset = 0;
                for (const auto& chunk : chunks)
                {
                    if (!chunk || chunk->bytes.empty() || chunk->bytes.size() > value.size() - offset
                        || value.compare(offset,chunk->bytes.size(),chunk->bytes) != 0) return false;
                    offset += chunk->bytes.size();
                }
                return offset == value.size(); // Never accept a matching prefix.
            }
        };
        struct EffectiveRecipe
        {
            std::shared_ptr<Charge> charge;
            std::string digest;
            Defines values;
        };
        std::array<std::weak_ptr<const SourceText>, SourceTextSlots> sourceTexts;
        std::array<std::weak_ptr<const EffectiveRecipe>, EffectiveRecipeSlots> effectiveRecipes;
        std::array<std::weak_ptr<const SourceChunk>, SourceChunkSlots> sourceChunks;
        std::shared_ptr<Charge> admitMetadata(std::size_t bytes, SourceAttachReceipt& receipt)
        {
            receipt.step = SourceAttachStep::MetadataAdmission;
            receipt.requestedMetadataBytes = bytes + sizeof(Charge) + 64;
            auto result = reserve(bytes);
            receipt.step = SourceAttachStep::Allocation;
            return result;
        }
        std::size_t sourceChunkEnd(std::string_view source, std::size_t offset)
        {
            const auto end = offset + std::min(SourceChunkBytes, source.size() - offset);
            if (end == source.size()) return end;
            const auto newline = source.substr(offset,end - offset).rfind('\n');
            // Preserve CRLF and every byte exactly. A line longer than the
            // bounded target is cut deterministically without normalization.
            return newline != std::string_view::npos ? offset + newline + 1 : end;
        }
        std::shared_ptr<const SourceChunk> internSourceChunk(std::string_view bytes, SourceAttachReceipt& receipt)
        {
            const auto hash = digest(bytes);
            auto slot = sourceChunks.end();
            for (auto it = sourceChunks.begin(); it != sourceChunks.end(); ++it)
            {
                if (auto existing = it->lock())
                {
                    if (existing->digest == hash && existing->bytes == bytes) return existing;
                }
                else
                {
                    it->reset();
                    if (slot == sourceChunks.end()) slot = it;
                }
            }
            if (slot == sourceChunks.end())
            {
                receipt.step = SourceAttachStep::RegistryCapacity;
                receipt.requestedMetadataBytes = 0;
                throw RegistryCapacityFailed{};
            }
            auto charge = admitMetadata(sizeof(SourceChunk) + bytes.size() + 256, receipt);
            // Keep object and shared control block separate: a surviving weak
            // registry reference may retain only its charged fixed overhead.
            auto result = std::shared_ptr<SourceChunk>(new SourceChunk);
            result->charge = std::move(charge); result->digest = hash; result->bytes = bytes;
            *slot = result;
            return result;
        }
        std::shared_ptr<const SourceText> internSourceText(const osg::Shader& shader, const std::string& hash,
            SourceAttachReceipt& receipt)
        {
            // All mutation of these fixed weak registries happens under the
            // producer writer lease. Neither a digest nor a filename proves
            // body equality; an exact full comparison is mandatory on reuse.
            auto slot = sourceTexts.end();
            for (auto it = sourceTexts.begin(); it != sourceTexts.end(); ++it)
            {
                if (auto existing = it->lock())
                {
                    if (existing->digest == hash && existing->matches(shader)) return existing;
                }
                else
                {
                    it->reset(); // release a dead weak control block before replacement
                    if (slot == sourceTexts.end()) slot = it;
                }
            }
            if (slot == sourceTexts.end())
            {
                receipt.step = SourceAttachStep::RegistryCapacity;
                receipt.requestedMetadataBytes = 0;
                throw RegistryCapacityFailed{};
            }
            const std::string_view source = shader.getShaderSource();
            std::size_t count = 0;
            for (std::size_t offset = 0; offset < source.size(); offset = sourceChunkEnd(source,offset))
                if (++count > MaximumSourceChunks)
                {
                    receipt.step = SourceAttachStep::SourceShape;
                    receipt.requestedMetadataBytes = 0;
                    throw RegistryCapacityFailed{};
                }
            // A body owns only its bounded ordered references. Its chunks own
            // immutable exact bytes, shared even when numeric substitutions
            // make complete stock/PBR bodies differ. No projection family or
            // filename authorizes either storage object.
            auto charge = admitMetadata(sizeof(SourceText) + count * sizeof(std::shared_ptr<const SourceChunk>) + 512, receipt);
            auto result = std::shared_ptr<SourceText>(new SourceText);
            result->charge = std::move(charge); result->digest = hash; result->bytes = source.size();
            result->chunks.reserve(count);
            for (std::size_t offset = 0; offset < source.size();)
            {
                const auto end = sourceChunkEnd(source,offset);
                result->chunks.push_back(internSourceChunk(source.substr(offset,end - offset),receipt));
                offset = end;
            }
            *slot = result;
            return result;
        }
        std::shared_ptr<const EffectiveRecipe> internEffectiveRecipe(const Defines& local, const Defines& global,
            std::size_t parameterBytes, SourceAttachReceipt& receipt)
        {
            // Charge both the temporary map and its serialized digest input
            // before copying. New retained ownership is admitted separately;
            // a reused recipe releases all of this scratch before returning.
            auto scratch = admitMetadata(parameterBytes * 2 + 512, receipt);
            Defines values = global;
            for (const auto& [key,value] : local) values[key] = value;
            auto hash = recipe(values);
            auto slot = effectiveRecipes.end();
            for (auto it = effectiveRecipes.begin(); it != effectiveRecipes.end(); ++it)
            {
                if (auto existing = it->lock())
                {
                    if (existing->digest == hash && existing->values == values) return existing;
                }
                else
                {
                    it->reset();
                    if (slot == effectiveRecipes.end()) slot = it;
                }
            }
            if (slot == effectiveRecipes.end())
            {
                receipt.step = SourceAttachStep::RegistryCapacity;
                receipt.requestedMetadataBytes = 0;
                throw RegistryCapacityFailed{};
            }
            auto charge = admitMetadata(sizeof(EffectiveRecipe) + parameterBytes + 256, receipt);
            auto result = std::shared_ptr<EffectiveRecipe>(new EffectiveRecipe);
            result->charge = std::move(charge); result->digest = std::move(hash); result->values = std::move(values);
            *slot = result;
            return result;
        }
        struct SourceData
        {
            std::shared_ptr<Charge> charge;
            std::shared_ptr<Epoch> epoch;
            std::uint64_t version = 0;
            unsigned families = 0;
            std::string name, sourceDigest, rawDigest, expandedDigest, defineDigest;
            // Epoch/provenance remain unique immutable producer records.
            // Only exact source bytes and effective numeric input recipes are
            // shared; neither storage object confers a family certificate.
            std::shared_ptr<const SourceText> text;
            std::shared_ptr<const EffectiveRecipe> effective;
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
                    || !text || !text->matches(shader)) return Refusal::Source;
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
    const char* sourceAttachStepName(SourceAttachStep value)
    {
        constexpr const char* names[]{"none","raw_proof","source_shape","parameter_recipe","record_limit",
            "metadata_admission","allocation","publication","registry_capacity"};
        return static_cast<unsigned>(value) < std::size(names) ? names[static_cast<unsigned>(value)] : "unknown";
    }
    ProducerDiagnostics producerDiagnostics()
    {
        ProducerDiagnostics result;
        for (unsigned i = 0; i < RefusalCount; ++i)
        {
            result.initialSource[i] = initialSource[i].load(std::memory_order_relaxed);
            result.regeneratedSource[i] = regeneratedSource[i].load(std::memory_order_relaxed);
            result.refreshedProgram[i] = refreshedProgram[i].load(std::memory_order_relaxed);
        }
        result.liveMetadataBytes = metadata.load(std::memory_order_relaxed);
        result.peakMetadataBytes = metadataPeak.load(std::memory_order_relaxed);
        return result;
    }
    void recordSourceAttachment(bool regeneration, Refusal refusal) noexcept
    {
        const auto slot = static_cast<unsigned>(refusal);
        if (slot < RefusalCount) ++(regeneration ? regeneratedSource : initialSource)[slot];
    }
    void recordProgramRefresh(Refusal refusal) noexcept
    {
        const auto slot = static_cast<unsigned>(refusal);
        if (slot < RefusalCount) ++refreshedProgram[slot];
    }
    Refusal attachSource(osg::Shader& shader, std::string_view name, const TemplateProof& proof,
        const Defines& local, const Defines& global, const std::shared_ptr<Epoch>& epoch,
        SourceAttachReceipt* receipt)
    {
        ProducerMutationScope producer;
        SourceAttachReceipt localReceipt;
        if (!receipt) receipt = &localReceipt;
        *receipt = {};
        receipt->step = SourceAttachStep::RawProof;
        if (!enabled() || !proof.families || !proof.admission || !epoch) return Refusal::Missing;
        receipt->step = SourceAttachStep::SourceShape;
        if (name.size() > 1024 || shader.getShaderSource().size() > 2u * 1024u * 1024u) return Refusal::Capacity;
        if (shader.getShaderBinary() || !shader.getCodeInjectionMap().empty()) return Refusal::Source;
        std::size_t bytes = sizeof(SourceData) + name.size() + 1024;
        const auto recordBaseBytes = bytes;
        receipt->step = SourceAttachStep::ParameterRecipe;
        for (const auto* values : {&local,&global})
            if (const auto refusal = bounded(*values,bytes); refusal != Refusal::None) return refusal;
        const auto parameterBytes = bytes - recordBaseBytes;
        receipt->step = SourceAttachStep::RecordLimit;
        for (const auto* values : {&shader.getShaderDefines(),&shader.getShaderRequirements()})
        {
            if (values->size() > 256) return Refusal::Capacity;
            for (const auto& value : *values)
            {if (value.size() > 1024) return Refusal::Capacity; bytes += value.size() + 128;}
        }
        if (bytes > MaximumRecordBytes) return Refusal::Capacity;
        try
        {
            // The record owns its provenance and stage/link fields. Shared
            // exact body/recipe ownership has separate quota admissions below.
            auto charge = admitMetadata(bytes - parameterBytes + 64, *receipt);
            auto data = std::make_shared<SourceData>(); data->charge = std::move(charge);
            data->epoch = epoch; data->version = epoch->value.load(std::memory_order_acquire);
            data->name = name; data->families = proof.families;
            data->sourceDigest = digest(shader.getShaderSource()); data->rawDigest = proof.rawDigest;
            data->expandedDigest = proof.expandedDigest;
            data->text = internSourceText(shader, data->sourceDigest, *receipt);
            data->effective = internEffectiveRecipe(local, global, parameterBytes, *receipt);
            data->defineDigest = data->effective->digest;
            data->type = shader.getType(); data->mode = shader.getShaderDefinesMode();
            data->defines = shader.getShaderDefines(); data->requirements = shader.getShaderRequirements();
            receipt->step = SourceAttachStep::Publication;
            publish(shader, SourceKey, new Source(std::move(data)));
            receipt->step = SourceAttachStep::None;
            return Refusal::None;
        }
        catch (const MetadataAdmissionFailed&) {return Refusal::Capacity;}
        catch (const RegistryCapacityFailed&) {return Refusal::Capacity;}
        catch (const std::bad_alloc&)
        {
            // Failure allocating the Charge itself is not evidence that its
            // logical cap rejected admission; preserve that distinction.
            if (receipt->step == SourceAttachStep::MetadataAdmission) receipt->step = SourceAttachStep::Allocation;
            return Refusal::Capacity;
        } // Optional proof cannot affect normal shader construction.
    }
    SourceStatus sourceStatus(const osg::Shader& shader)
    {
        ProducerReadScope producer;
        const auto* user = shader.getUserDataContainer();
        const auto* proof = user ? dynamic_cast<const Source*>(user->getUserObject(SourceKey)) : nullptr;
        if (!proof || !proof->data) return {};
        return {proof->data->current(shader),proof->data->families,proof->data->name,proof->data,
            proof->data->version,proof->data->epoch ? proof->data->epoch->value.load(std::memory_order_acquire) : 0};
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
                if (data->objectSource && (!record.effective || !data->objectSource->effective
                    || data->objectSource->effective->values != record.effective->values
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
        const auto& features = mData->proof->objectSource->effective->values;
        const auto found = features.find(std::string(name));
        return found == features.end() ? std::string_view{} : std::string_view(found->second);
    }
    const Samplers& Snapshot::samplers() const {static const Samplers empty; return mData ? mData->proof->samplers : empty;}
    std::size_t Snapshot::metadataBytes() const {return mData ? mData->bytes : 0;}
    const Shader::ProgramDefineSnapshot* Snapshot::importedDefines() const noexcept
    {return mData ? &mData->imported : nullptr;}
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
