#include <components/resource/programvariantcompile.hpp>
#include <components/shader/programvariant.hpp>
#include <components/shader/shadermanager.hpp>
#include <components/shader/indirectprojectioncontract.hpp>
#include <components/testing/util.hpp>

#include <fstream>
#include <regex>

#include <gtest/gtest.h>

#include <osg/GLExtensions>
#include <osgViewer/Viewer>

namespace
{
    osg::ref_ptr<Shader::VariantProgram> programWithDefines(const osg::ShaderDefines& names)
    {
        osg::ref_ptr<Shader::VariantProgram> program = new Shader::VariantProgram;
        program->addShader(new osg::Shader(osg::Shader::VERTEX, "void main() { gl_Position = gl_Vertex; }"));
        program->setShaderDefines(names);
        return program;
    }

    TEST(ProgramVariant, DefineSnapshotResolvesOverrideAndProtectedWithoutRetainingPass)
    {
        auto program = programWithDefines({ "A" });
        osg::ref_ptr<osg::StateSet> outer = new osg::StateSet;
        osg::ref_ptr<osg::StateSet> inner = new osg::StateSet;
        outer->setDefine("A", "1", osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
        inner->setDefine("A", "2", osg::StateAttribute::ON);
        const auto blocked = Shader::ProgramDefineSnapshot::fromStateSets(*program, { outer, inner });
        EXPECT_EQ(blocked.map.currentDefines.at("A").first, "1");
        EXPECT_NE(blocked.string.find("#define A 1"), std::string::npos);
        inner->setDefine("A", "2", osg::StateAttribute::ON | osg::StateAttribute::PROTECTED);
        const auto protectedValue = Shader::ProgramDefineSnapshot::fromStateSets(*program, { outer, inner });
        EXPECT_EQ(protectedValue.map.currentDefines.at("A").first, "2");
        EXPECT_NE(protectedValue.string.find("#define A 2"), std::string::npos);
        outer = nullptr;
        inner = nullptr;
        osg::ref_ptr<osg::State> state = new osg::State;
        Shader::ProgramDefineScope scope(*state, protectedValue);
        EXPECT_EQ(state->getDefineString(program->getShaderDefines()), protectedValue.string);
    }

    TEST(ProgramVariant, DefineScopeRemovesUnrelatedCurrentStateAndRestoresExactMap)
    {
        auto program = programWithDefines({ "A", "DISTORTION" });
        osg::ref_ptr<osg::State> state = new osg::State;
        osg::ref_ptr<osg::StateSet> original = new osg::StateSet;
        original->setDefine("A", "7", osg::StateAttribute::ON);
        original->setDefine("DISTORTION", "1", osg::StateAttribute::ON);
        state->pushStateSet(original);
        const auto previous = state->getDefineString(program->getShaderDefines());
        const auto absent = Shader::ProgramDefineSnapshot::fromStateSets(*program, {});
        {
            Shader::ProgramDefineScope scope(*state, absent);
            EXPECT_TRUE(state->getDefineString(program->getShaderDefines()).empty());
            EXPECT_EQ(state->getStateSetStackSize(), 1u);
        }
        EXPECT_EQ(state->getDefineString(program->getShaderDefines()), previous);
        state->popStateSet();
        EXPECT_TRUE(state->getDefineString(program->getShaderDefines()).empty());
    }

    TEST(ProgramVariant, CustomAbsentImportedDefineKeepsOriginalFallback)
    {
        auto program = programWithDefines({ "MOD_UNKNOWN" });
        const auto absent = Shader::ProgramDefineSnapshot::fromStateSets(*program, {});
        EXPECT_FALSE(Resource::knownProgramProducerDefines(*program, absent));
        osg::ref_ptr<osg::StateSet> exactPass = new osg::StateSet;
        exactPass->setDefine("MOD_UNKNOWN", "9", osg::StateAttribute::ON);
        EXPECT_TRUE(Resource::knownProgramProducerDefines(*program,
            Shader::ProgramDefineSnapshot::fromStateSets(*program, { exactPass })));
    }

    TEST(ProgramVariant, DefineScopeRestoresChangedStacksDuringExceptionUnwind)
    {
        auto program = programWithDefines({ "A" });
        osg::ref_ptr<osg::State> state = new osg::State;
        osg::ref_ptr<osg::StateSet> original = new osg::StateSet;
        original->setDefine("A", "7", osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
        state->pushStateSet(original);
        // Preserve the pending changed bit as well as values and override
        // stacks: no getDefineString has consumed the pending update yet.
        const auto before = state->getDefineMap();
        const auto replacement = Shader::ProgramDefineSnapshot::fromStateSets(*program, {});
        EXPECT_THROW(([&] {
            Shader::ProgramDefineScope scope(*state, replacement);
            EXPECT_TRUE(state->getDefineString(program->getShaderDefines()).empty());
            throw std::runtime_error("fixture compile failure");
        })(), std::runtime_error);
        const auto& restored = state->getDefineMap();
        EXPECT_EQ(restored.changed, before.changed);
        EXPECT_EQ(restored.currentDefines, before.currentDefines);
        ASSERT_EQ(restored.map.size(), before.map.size());
        EXPECT_EQ(restored.map.at("A").changed, before.map.at("A").changed);
        EXPECT_EQ(restored.map.at("A").defineVec, before.map.at("A").defineVec);
        EXPECT_NE(state->getDefineString(program->getShaderDefines()).find("#define A 7"), std::string::npos);
        state->popStateSet();
        EXPECT_TRUE(state->getDefineString(program->getShaderDefines()).empty());
    }

    TEST(ProgramVariant, FullSourceBindingsAndReleaseGenerationInvalidateCpuRequest)
    {
        auto program = programWithDefines({});
        auto identity = Shader::ProgramLinkIdentity::capture(*program);
        ASSERT_TRUE(identity);
        EXPECT_TRUE(identity->matches(*program));
        program->addBindAttribLocation("position", 3);
        EXPECT_FALSE(identity->matches(*program));
        identity = Shader::ProgramLinkIdentity::capture(*program);
        program->addBindFragDataLocation("output", 1);
        EXPECT_FALSE(identity->matches(*program));
        identity = Shader::ProgramLinkIdentity::capture(*program);
        program->addBindUniformBlock("view", 4);
        EXPECT_FALSE(identity->matches(*program));
        identity = Shader::ProgramLinkIdentity::capture(*program);
        program->getShader(0)->setShaderSource("void main() { gl_Position = vec4(1.0); }");
        EXPECT_FALSE(identity->matches(*program));
        identity = Shader::ProgramLinkIdentity::capture(*program);
        program->invalidateVariantPreparations();
        EXPECT_FALSE(identity->matches(*program));
    }

    TEST(ProgramVariant, PopulatedProgramBinaryIsNotAppliedAcrossDefineVariants)
    {
        auto program = programWithDefines({ "A" });
        osg::ref_ptr<osg::Program::ProgramBinary> binary = new osg::Program::ProgramBinary;
        binary->allocate(4);
        program->setProgramBinary(binary);
        EXPECT_FALSE(Shader::ProgramLinkIdentity::capture(*program));
    }

    TEST(ProgramVariant, OversizedShaderCaptureUsesOriginalFallback)
    {
        auto program = programWithDefines({});
        program->getShader(0)->setShaderSource(std::string(2u * 1024u * 1024u + 1u, 'x'));
        EXPECT_FALSE(Shader::ProgramLinkIdentity::capture(*program));
    }

    TEST(ProgramVariant, MetadataAndProducerPathBoundsRefuseBeforeSnapshotCopies)
    {
        auto program = programWithDefines({});
        for (unsigned i = 0; i <= Shader::ProgramLinkIdentity::sMaxBindings; ++i)
            program->addBindAttribLocation("attribute" + std::to_string(i), i);
        EXPECT_FALSE(Shader::ProgramLinkIdentity::capture(*program));
        program = programWithDefines({ "A" });
        osg::ref_ptr<osg::StateSet> huge = new osg::StateSet;
        huge->setDefine("A", std::string(Shader::ProgramLinkIdentity::sMaxValueBytes + 1, 'x'));
        const auto refused = Shader::ProgramDefineSnapshot::fromStateSets(*program, { huge });
        EXPECT_FALSE(refused.bounded);
        EXPECT_TRUE(refused.map.map.empty());
        EXPECT_FALSE(Resource::knownProgramProducerDefines(*program, refused));
        EXPECT_FALSE(Shader::ProgramDefineSnapshot::fromStateSets(*program,
            std::vector<const osg::StateSet*>(65, nullptr)).bounded);
    }

    TEST(ProgramVariant, FactorySeparatesActualTemplateBindingsAndSamplers)
    {
        if (!Shader::exactProgramVariantsEnabled())
            GTEST_SKIP() << "Candidate factory identity is independently gated; run with OPENMW_EXACT_PROGRAM_VARIANTS=1";
        Shader::ShaderManager manager;
        osg::ref_ptr<osg::Shader> vertex = new osg::Shader(osg::Shader::VERTEX, "void main() {}" );
        osg::ref_ptr<osg::Shader> fragment = new osg::Shader(osg::Shader::FRAGMENT, "void main() {}" );
        osg::ref_ptr<osg::Program> firstTemplate = new osg::Program;
        osg::ref_ptr<osg::Program> secondTemplate = new osg::Program;
        firstTemplate->addBindAttribLocation("position", 2);
        secondTemplate->addBindAttribLocation("position", 3);
        const auto first = manager.getProgram(vertex, fragment, firstTemplate, { { "diffuse", 0 } });
        const auto same = manager.getProgram(vertex, fragment, firstTemplate, { { "diffuse", 0 } });
        const auto differentBinding = manager.getProgram(vertex, fragment, secondTemplate, { { "diffuse", 0 } });
        const auto differentSampler = manager.getProgram(vertex, fragment, firstTemplate, { { "diffuse", 1 } });
        EXPECT_EQ(first, same);
        EXPECT_NE(first, differentBinding);
        EXPECT_NE(first, differentSampler);
        EXPECT_EQ(first->getAttribBindingList().at("position"), 2u);
        EXPECT_EQ(differentBinding->getAttribBindingList().at("position"), 3u);
    }

    TEST(ProgramVariant, ProducerRelinkRestoresActualSamplerUniformsOnTheSamePcp)
    {
        if (!Shader::exactProgramVariantsEnabled())
            GTEST_SKIP() << "Run with OPENMW_EXACT_PROGRAM_VARIANTS=1";
        osgViewer::Viewer anchor;
        osg::ref_ptr<osg::GraphicsContext::Traits> traits = new osg::GraphicsContext::Traits;
        traits->readDISPLAY();
        traits->setUndefinedScreenDetailsToDefaultScreen();
        traits->width = 16;
        traits->height = 16;
        traits->doubleBuffer = false;
        traits->windowDecoration = false;
        osg::ref_ptr<osg::GraphicsContext> context = osg::GraphicsContext::createGraphicsContext(traits);
        ASSERT_TRUE(context && context->realize());
        ASSERT_TRUE(context->makeCurrent());
        struct ReleaseCurrent
        {
            osg::GraphicsContext& context;
            ~ReleaseCurrent() { context.releaseContext(); }
        } release{ *context };
        auto& state = *context->getState();
        Shader::ShaderManager manager;
        osg::ref_ptr<osg::Shader> vertex = new osg::Shader(osg::Shader::VERTEX,
            "#version 120\nvoid main(){gl_Position=gl_Vertex;}\n");
        osg::ref_ptr<osg::Shader> fragment = new osg::Shader(osg::Shader::FRAGMENT,
            "#version 120\nuniform sampler2D diffuse;\n"
            "void main(){gl_FragColor=texture2D(diffuse,vec2(0.25));}\n");
        auto program = manager.getProgram(vertex, fragment, nullptr, { { "diffuse", 3 } });
        auto* variant = dynamic_cast<Shader::VariantProgram*>(program.get());
        ASSERT_TRUE(variant);
        const auto baselineSlots = Resource::ExactProgramVariantRequest::liveRequests.load();
        const auto baselineSources = Resource::ExactProgramVariantRequest::capturedSourceBytes.load();
        const auto baselineMetadata = Resource::ExactProgramVariantRequest::capturedMetadataBytes.load();
        std::vector<std::shared_ptr<Resource::ExactProgramVariantRequest>> queued;
        for (std::size_t i = baselineSlots; i < Resource::ExactProgramVariantRequest::sMaxLiveRequests; ++i)
        {
            auto request = Resource::ExactProgramVariantRequest::create(context, program, {});
            ASSERT_TRUE(request);
            queued.push_back(std::move(request));
        }
        EXPECT_FALSE(Resource::ExactProgramVariantRequest::create(context, program, {}));
        queued.clear();
        EXPECT_EQ(Resource::ExactProgramVariantRequest::liveRequests.load(), baselineSlots);
        EXPECT_EQ(Resource::ExactProgramVariantRequest::capturedSourceBytes.load(), baselineSources);
        EXPECT_EQ(Resource::ExactProgramVariantRequest::capturedMetadataBytes.load(), baselineMetadata);
        program->apply(state);
        auto* pcp = program->getPCP(state);
        ASSERT_TRUE(pcp && pcp->isLinked());
        const auto handle = pcp->getHandle();
        auto* extensions = state.get<osg::GLExtensions>();
        const auto samplerValue = [&] {
            GLint value = -1;
            const auto location = extensions->glGetUniformLocation(handle, "diffuse");
            EXPECT_GE(location, 0);
            extensions->glGetUniformiv(handle, location, &value);
            return value;
        };
        EXPECT_EQ(samplerValue(), 3);
        const auto firstUses = Shader::programVariantStats().firstUses.load();
        vertex->setShaderSource(vertex->getShaderSource() + "\n// actual producer relink\n");
        ASSERT_TRUE(Shader::compileExactProgramVariant(*program, state, 6001));
        ASSERT_EQ(program->getPCP(state), pcp);
        ASSERT_EQ(pcp->getHandle(), handle);
        EXPECT_EQ(samplerValue(), 0); // GL linking resets active uniform values.
        const auto epoch = variant->preparedLinkEpoch(state, *pcp);
        EXPECT_NE(epoch, 0u);
        program->apply(state);
        EXPECT_EQ(samplerValue(), 3);
        EXPECT_EQ(Shader::programVariantStats().firstUses.load(), firstUses + 1);
        const auto reuse = variant->takePreparedFirstUse(state, *pcp);
        EXPECT_FALSE(reuse.ready);
        EXPECT_TRUE(reuse.reused);
        EXPECT_EQ(reuse.linkEpoch, epoch);
        ASSERT_TRUE(Shader::compileExactProgramVariant(*program, state, 6002));
        EXPECT_EQ(variant->preparedLinkEpoch(state, *pcp), epoch);
        program->apply(state);
        EXPECT_EQ(samplerValue(), 3);
        EXPECT_EQ(Shader::programVariantStats().firstUses.load(), firstUses + 1);
        program->releaseGLObjects(&state);
        EXPECT_EQ(glGetError(), GL_NO_ERROR);
    }

    Shader::ShaderManager::DefineMap baselineTemplateDefines(const std::filesystem::path& root)
    {
        // This test exercises CPU template/link preparation only. Every shipped
        // parameter gets a zero value so the ordinary mono/core link is chosen.
        Shader::ShaderManager::DefineMap defines;
        const std::regex token(R"(@([A-Za-z_][A-Za-z_0-9]*))");
        for (const auto& entry : std::filesystem::recursive_directory_iterator(root))
        {
            if (!entry.is_regular_file())
                continue;
            std::ifstream input(entry.path());
            const std::string content{ std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
            for (auto it = std::sregex_iterator(content.begin(), content.end(), token); it != std::sregex_iterator(); ++it)
                defines.emplace((*it)[1].str(), "0");
        }
        return defines;
    }

    TEST(ProgramVariant, ActualShippedObjectProjectionCertificateRejectsConsumedSourceMutation)
    {
        if (!Shader::IndirectProjection::enabled())
            GTEST_SKIP() << "Run in a separate process with OPENMW_P9_GPU_VISIBILITY=1";
        const auto root = std::filesystem::path(OPENMW_PROJECT_SOURCE_DIR) / "files" / "shaders";
        Shader::ShaderManager manager;
        manager.setShaderPath(root);
        const auto program = manager.getProgram("compatibility/objects", baselineTemplateDefines(root));
        ASSERT_TRUE(program);
        EXPECT_TRUE(Shader::IndirectProjection::matches(*program));
        osg::Shader* object = nullptr;
        for (unsigned i = 0; i < program->getNumShaders(); ++i)
            if (program->getShader(i)->getShaderSource().find("gl_Position = modelToClip(gl_Vertex)") != std::string::npos)
                object = program->getShader(i);
        ASSERT_TRUE(object);
        object->setShaderSource(object->getShaderSource() + "\n// consumed source mutation invalidates exact certificate\n");
        EXPECT_FALSE(Shader::IndirectProjection::matches(*program));
    }

    TEST(ProgramVariant, ActualTemplateIncludeMutationCannotAcquireBaselineCertificate)
    {
        if (!Shader::IndirectProjection::enabled())
            GTEST_SKIP() << "Run in a separate process with OPENMW_P9_GPU_VISIBILITY=1";
        const auto root = std::filesystem::path(OPENMW_PROJECT_SOURCE_DIR) / "files" / "shaders";
        const auto copied = TestingOpenMW::outputDir() / "program-variant-custom-projection";
        std::filesystem::create_directories(copied);
        std::filesystem::copy(root, copied,
            std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing);
        const auto core = copied / "lib" / "core" / "vertex.glsl";
        std::ifstream input(core);
        std::string content{ std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
        input.close();
        const std::string baseline = "return projectionMatrix * modelToView(pos);";
        const auto position = content.find(baseline);
        ASSERT_NE(position, std::string::npos);
        content.replace(position, baseline.size(), "return projectionMatrix * modelToView(pos + vec4(1000.0,0.0,0.0,0.0));");
        {
            std::ofstream output(core);
            output << content;
        }
        Shader::ShaderManager manager;
        manager.setShaderPath(copied);
        const auto program = manager.getProgram("compatibility/objects", baselineTemplateDefines(copied));
        ASSERT_TRUE(program);
        EXPECT_FALSE(Shader::IndirectProjection::matches(*program));
    }
}
