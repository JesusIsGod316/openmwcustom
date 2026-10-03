#ifndef OPENMW_SHADER_INDIRECTPROJECTIONCONTRACT_H
#define OPENMW_SHADER_INDIRECTPROJECTIONCONTRACT_H

#include <osg/Program>
#include <osg/Shader>
#include <osg/UserDataContainer>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <new>
#include <string>
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

    class Source final : public osg::Object
    {
    public:
        Source() = default;
        Source(const Source& copy, const osg::CopyOp& op) : osg::Object(copy, op) {}
        META_Object(Shader, Source)
        Source(const osg::Shader& shader, std::string templateName)
            : mCode(shader.getShaderSource()), mTemplate(std::move(templateName)), mType(shader.getType())
        {
            setName(SourceKey);
            const auto bytes = mCode.capacity() + mTemplate.capacity() + sizeof(Source);
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
        { return shader.getType() == mType && shader.getShaderSource() == mCode && !mCode.empty(); }
        const std::string& sourceTemplate() const { return mTemplate; }
    private:
        ~Source() override { liveBytes.fetch_sub(mCharge, std::memory_order_relaxed); }
        const std::string mCode;
        const std::string mTemplate;
        osg::Shader::Type mType = osg::Shader::UNDEFINED;
        std::uint64_t mCharge = 0;
    };

    class Program final : public osg::Object
    {
    public:
        Program() = default;
        Program(const Program& copy, const osg::CopyOp& op) : osg::Object(copy, op) {}
        META_Object(Shader, Program)
        struct Producer { osg::ref_ptr<const osg::Shader> shader; osg::ref_ptr<const Source> source; };
        explicit Program(std::vector<Producer>&& producers) : mProducers(std::move(producers))
        { setName(ProgramKey); }
        bool matches(const osg::Program& program) const
        {
            if (mProducers.empty() || program.getNumShaders() != mProducers.size()) return false;
            for (unsigned i = 0; i < program.getNumShaders(); ++i)
                if (program.getShader(i) != mProducers[i].shader.get()
                    || !mProducers[i].source->matches(*program.getShader(i))) return false;
            return true;
        }
    private:
        const std::vector<Producer> mProducers;
    };

    inline void certify(osg::Program& program)
    {
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
        if (object && core) program.getOrCreateUserDataContainer()->addUserObject(new Program(std::move(producers)));
    }

    inline bool matches(const osg::Program& program)
    {
        const auto* data = program.getUserDataContainer();
        const auto* certificate = data ? dynamic_cast<const Program*>(data->getUserObject(ProgramKey)) : nullptr;
        return certificate && certificate->matches(program);
    }
}
#endif
