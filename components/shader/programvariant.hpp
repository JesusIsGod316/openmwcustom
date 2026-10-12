#ifndef OPENMW_COMPONENTS_SHADER_PROGRAMVARIANT_H
#define OPENMW_COMPONENTS_SHADER_PROGRAMVARIANT_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <osg/GraphicsContext>
#include <osg/Program>
#include <osg/State>
#include <osg/StateSet>

namespace Shader
{
    // Resolved once per process. OFF leaves producer queues and Program::apply unchanged.
    bool exactProgramVariantsEnabled();

    struct ProgramVariantStats
    {
        std::atomic<std::uint64_t> requests{ 0 }, prepared{ 0 }, reused{ 0 }, failed{ 0 }, stale{ 0 };
        std::atomic<std::uint64_t> firstUses{ 0 }, reuseUses{ 0 }, fallbackUses{ 0 }, unsupported{ 0 };
    };
    ProgramVariantStats& programVariantStats();

    // Exact CPU-side linking inputs, not a GPU byte estimate or serialized GL object.
    struct ProgramLinkIdentity
    {
        struct Source
        {
            osg::ref_ptr<const osg::Shader> shader;
            std::string text;
            osg::ShaderDefines defines, requirements;
            osg::Shader::Type type;
            osg::Shader::ShaderDefinesMode mode;
        };
        std::vector<Source> sources;
        osg::Program::AttribBindingList attributes;
        osg::Program::FragDataBindingList fragments;
        osg::Program::UniformBlockBindingList blocks;
        osg::ShaderDefines defines;
        std::vector<std::string> feedback;
        GLenum feedbackMode = GL_SEPARATE_ATTRIBS;
        GLint geometryVertices = 0, geometryInput = 0, geometryOutput = 0;
        std::uint64_t generation = 0;
        std::size_t sourceBytes = 0;

        struct CaptureSize { std::size_t sources = 0, metadata = 0; };
        static constexpr std::size_t sMaxShaders = 32, sMaxBindings = 256, sMaxDefines = 256;
        static constexpr std::size_t sMaxNameBytes = 1024, sMaxValueBytes = 4096;
        static constexpr std::size_t sMaxMetadataBytes = 64u * 1024u;
        // Checks cardinality and text payload before any candidate copy.
        static std::optional<CaptureSize> boundedCaptureSize(const osg::Program& program);

        static std::optional<ProgramLinkIdentity> capture(const osg::Program& program);
        bool matches(const osg::Program& program) const;
    };

    // Only copies define values. No StateSet/cull visitor/texture/FBO survives capture.
    // OSG itself resolves OFF/OVERRIDE/PROTECTED through this CPU-only State.
    struct ProgramDefineSnapshot
    {
        osg::State::DefineMap map;
        std::string string;
        bool bounded = true;
        static std::optional<std::size_t> boundedPathMetadata(
            const osg::Program& program, const std::vector<const osg::StateSet*>& path);
        static ProgramDefineSnapshot capture(
            osg::State& state, const osg::Program& program);
        static ProgramDefineSnapshot fromStateSets(const osg::Program& program,
            const std::vector<const osg::StateSet*>& path);
    };

    class ProgramDefineScope
    {
    public:
        ProgramDefineScope(osg::State& state, const ProgramDefineSnapshot& snapshot);
        ~ProgramDefineScope();
        ProgramDefineScope(const ProgramDefineScope&) = delete;
        ProgramDefineScope& operator=(const ProgramDefineScope&) = delete;
    private:
        osg::State& mState;
        osg::State::DefineMap mPrevious;
    };

    struct PreparedVariantUse
    {
        bool ready = false;
        std::uint64_t receipt = 0;
        std::uint64_t linkEpoch = 0;
        bool reused = false;
    };

    // The canonical Program retains its PCP in OSG. These bounded receipts prove
    // the selected PCP was prepared; they never substitute another Program/PCP.
    class VariantProgram : public osg::Program
    {
    public:
        VariantProgram();
        VariantProgram(const osg::Program& other, const osg::CopyOp& copyop);
        VariantProgram(const VariantProgram& other, const osg::CopyOp& copyop);
        ~VariantProgram() override;
        std::uint64_t variantGeneration() const;
        void invalidateVariantPreparations() const;
        void releaseGLObjects(osg::State* state = nullptr) const override;
        void notePrepared(osg::State& state, std::uint64_t receipt, bool relinked = true) const;
        // A producer may relink the same PCP handle, resetting its sampler
        // uniforms. This per-PCP epoch distinguishes that from simple reuse.
        std::uint64_t preparedLinkEpoch(osg::State& state,
            const osg::Program::PerContextProgram& pcp) const;
        // Called only when SamplerProgram will initialize a PCP, not every draw.
        PreparedVariantUse takePreparedFirstUse(osg::State& state,
            const osg::Program::PerContextProgram& pcp) const;
    private:
        struct Prepared;
        mutable std::unique_ptr<Prepared> mPrepared;
        mutable std::atomic<std::uint64_t> mVariantGeneration{ 1 };
    };

    // Explicit producer seam on the actual draw/ICO context. No texture, FBO,
    // arbitrary graph cull, glUseProgram, or sampler initialization is performed.
    bool compileExactProgramVariant(osg::Program& program, osg::State& state,
        std::uint64_t receipt = 0);
}

#endif
