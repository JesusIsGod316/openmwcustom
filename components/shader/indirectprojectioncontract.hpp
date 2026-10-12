#ifndef OPENMW_SHADER_INDIRECTPROJECTIONCONTRACT_H
#define OPENMW_SHADER_INDIRECTPROJECTIONCONTRACT_H

#include <osg/Program>
#include <osg/Shader>
#include <osg/State>
#include <osg/UserDataContainer>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <new>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <components/resource/assetpolicy.hpp>

namespace Shader::IndirectProjection
{
    inline bool enabled()
    {
        static const bool value = [] {
            const char* p = std::getenv("OPENMW_P9_GPU_VISIBILITY");
            const char* mdi = std::getenv("OPENMW_P9_MULTI_DRAW_INDIRECT");
            return (p && std::strcmp(p, "1") == 0) || (mdi && std::strcmp(mdi, "1") == 0)
                || Resource::AssetPolicy::enabled(Resource::AssetPolicy::VisualLod)
                || Resource::AssetPolicy::enabled(Resource::AssetPolicy::Hlod);
        }();
        return value;
    }
    inline constexpr const char* SourceKey = "OptimizedMW.BaselineObjectProjectionSource";
    inline constexpr const char* ProgramKey = "OptimizedMW.BaselineObjectProjectionProgram";

    inline std::shared_mutex producerMutex;
    inline thread_local unsigned producerReaders = 0, producerWriters = 0;
    // Independent synchronization for this proof domain. Draws never wait for
    // a manager mutation: a failed try-read chooses original primitive draws.
    // Readers may nest under a same-thread writer; never call ShaderManager
    // from a read lease or upgrade one into a writer.
    class ProducerReadScope
    {
    public:
        ProducerReadScope() : mLock(producerMutex, std::defer_lock)
        {
            if (!producerReaders && !producerWriters && !mLock.try_lock()) return;
            ++producerReaders; mHeld = true;
        }
        ~ProducerReadScope() { if (mHeld) --producerReaders; }
        explicit operator bool() const { return mHeld; }
    private:
        std::shared_lock<std::shared_mutex> mLock;
        bool mHeld = false;
    };
    class ProducerMutationScope
    {
    public:
        explicit ProducerMutationScope(bool requested = true) : mLock(producerMutex, std::defer_lock)
        {
            if (!requested) return;
            if (!producerWriters)
            {
                if (producerReaders) throw std::logic_error("indirect producer reader cannot become a writer");
                mLock.lock();
            }
            ++producerWriters; mHeld = true;
        }
        ~ProducerMutationScope() { if (mHeld) --producerWriters; }
    private:
        std::unique_lock<std::shared_mutex> mLock;
        bool mHeld = false;
    };

    inline bool numericParameter(std::string_view text)
    {
        // A pinned template must not acquire GLSL statements through @ values.
        // Numeric values and the comma-separated shadow texture list suffice
        // for the reviewed stock/PBR producers; an empty list disables it.
        if (text.empty()) return true;
        std::size_t position = 0;
        const auto digit = [](char value) { return value >= '0' && value <= '9'; };
        for (;;)
        {
            while (position < text.size() && text[position] == ' ') ++position;
            if (position < text.size() && (text[position] == '+' || text[position] == '-')) ++position;
            bool digits = false;
            while (position < text.size() && digit(text[position])) { ++position; digits = true; }
            if (position < text.size() && text[position] == '.')
            {
                ++position;
                while (position < text.size() && digit(text[position])) { ++position; digits = true; }
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

    template<class Defines> inline bool numericRecipe(const Defines& values)
    {
        if (values.size() > 256) return false;
        for (const auto& [name, value] : values)
            if (name.size() > 1024 || value.size() > 4096 || !numericParameter(value)) return false;
        return true;
    }

    inline bool safeImports(const osg::Program& program, const osg::State& state)
    {
        // OSG may prepend an inherited import without changing Shader source.
        // Read its resolved stack/cache directly: no temporary StateSets, GL
        // queries, or per-draw allocations. If not yet resolved, the top stack
        // entry already incorporates OVERRIDE/PROTECTED/OFF precedence.
        const auto& names = program.getShaderDefines();
        if (names.size() > 256) return false;
        const auto& map = state.getDefineMap();
        for (const auto& name : names)
        {
            if (name.size() > 1024) return false;
            const osg::StateSet::DefinePair* value = nullptr;
            if (map.changed)
            {
                const auto found = map.map.find(name);
                if (found != map.map.end() && !found->second.defineVec.empty())
                    value = &found->second.defineVec.back();
            }
            else if (const auto found = map.currentDefines.find(name); found != map.currentDefines.end())
                value = &found->second;
            if (value && (value->second & osg::StateAttribute::ON)
                && (value->first.size() > 4096 || !numericParameter(value->first))) return false;
        }
        return true;
    }

    class Source final : public osg::Object
    {
    public:
        Source() = default;
        Source(const Source& copy, const osg::CopyOp& op) : osg::Object(copy, op) {}
        META_Object(Shader, Source)
        Source(const osg::Shader& shader, std::string templateName)
            : mCode(shader.getShaderSource()), mTemplate(std::move(templateName)), mType(shader.getType())
            , mMode(shader.getShaderDefinesMode()), mDefines(shader.getShaderDefines())
            , mRequirements(shader.getShaderRequirements())
        {
            setName(SourceKey);
            std::uint64_t bytes = mCode.capacity() + mTemplate.capacity() + sizeof(Source);
            for (const auto* names : { &mDefines, &mRequirements })
            {
                if (names->size() > 256) throw std::bad_alloc{};
                for (const auto& name : *names)
                {
                    if (name.size() > 1024) throw std::bad_alloc{};
                    bytes += name.capacity() + 128;
                }
            }
            auto used = liveBytes.load(std::memory_order_relaxed);
            do
            {
                if (bytes > MaximumBytes || used > MaximumBytes - bytes) throw std::bad_alloc{};
            } while (!liveBytes.compare_exchange_weak(used, used + bytes, std::memory_order_relaxed));
            mCharge = bytes;
        }
        inline static std::atomic_uint64_t liveBytes{ 0 };
        static constexpr std::uint64_t MaximumBytes = 8ull * 1024 * 1024;
        bool matches(const osg::Shader& shader) const
        {
            ProducerReadScope producer;
            return producer && shader.getType() == mType && shader.getShaderSource() == mCode && !mCode.empty()
                && shader.getShaderDefinesMode() == mMode && shader.getShaderDefines() == mDefines
                && shader.getShaderRequirements() == mRequirements;
        }
        const std::string& sourceTemplate() const { return mTemplate; }
    private:
        ~Source() override { liveBytes.fetch_sub(mCharge, std::memory_order_relaxed); }
        const std::string mCode;
        const std::string mTemplate;
        osg::Shader::Type mType = osg::Shader::UNDEFINED;
        osg::Shader::ShaderDefinesMode mMode = osg::Shader::USE_SHADER_PRAGMA;
        const osg::ShaderDefines mDefines, mRequirements;
        std::uint64_t mCharge = 0;
    };

    class Program final : public osg::Object
    {
    public:
        Program() = default;
        Program(const Program& copy, const osg::CopyOp& op) : osg::Object(copy, op) {}
        META_Object(Shader, Program)
        struct Producer { osg::ref_ptr<const osg::Shader> shader; osg::ref_ptr<const Source> source; };
        Program(const osg::Program& program, std::vector<Producer>&& producers)
            : mProducers(std::move(producers)), mDefines(program.getShaderDefines())
        { setName(ProgramKey); }
        bool matches(const osg::Program& program) const
        {
            ProducerReadScope producer;
            if (!producer) return false;
            if (mProducers.empty() || program.getNumShaders() != mProducers.size()) return false;
            if (program.getShaderDefines() != mDefines || program.getNumTransformFeedBackVaryings()) return false;
            for (unsigned i = 0; i < program.getNumShaders(); ++i)
                if (program.getShader(i) != mProducers[i].shader.get()
                    || !mProducers[i].source->matches(*program.getShader(i))) return false;
            return true;
        }
    private:
        const std::vector<Producer> mProducers;
        const osg::ShaderDefines mDefines;
    };

    inline void certify(osg::Program& program)
    {
        ProducerMutationScope producer;
        std::vector<Program::Producer> producers;
        bool object = false, core = false;
        for (unsigned i = 0; i < program.getNumShaders(); ++i)
        {
            const auto* shader = program.getShader(i);
            if (!shader || (shader->getType() != osg::Shader::VERTEX && shader->getType() != osg::Shader::FRAGMENT)) return;
            const auto* data = shader->getUserDataContainer();
            const auto* source = data ? dynamic_cast<const Source*>(data->getUserObject(SourceKey)) : nullptr;
            if (!source || !source->matches(*shader)) return;
            object |= shader->getType() == osg::Shader::VERTEX && source->sourceTemplate() == "compatibility/objects.vert";
            core |= shader->getType() == osg::Shader::VERTEX && source->sourceTemplate() == "lib/core/vertex.glsl";
            producers.push_back({ shader, source });
        }
        if (object && core && program.getShaderDefines().size() <= 256 && !program.getNumTransformFeedBackVaryings())
        {
            auto* data = program.getOrCreateUserDataContainer();
            const auto index = data->getUserObjectIndex(ProgramKey);
            osg::ref_ptr<Program> proof = new Program(program, std::move(producers));
            if (index < data->getNumUserObjects()) data->setUserObject(index, proof);
            else data->addUserObject(proof);
        }
    }

    inline bool matches(const osg::Program& program)
    {
        ProducerReadScope producer;
        if (!producer) return false;
        const auto* data = program.getUserDataContainer();
        const auto* certificate = data ? dynamic_cast<const Program*>(data->getUserObject(ProgramKey)) : nullptr;
        return certificate && certificate->matches(program);
    }
}
#endif
