#include <apps/openmw/mwrender/v4effectuv.hpp>

#include <osg/Matrix>
#include <osg/TexGen>

#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <string>

#ifdef OPENMW_V4_TEST_LEGACY_UV
#include "legacy-effect-uv.hpp"
#endif

namespace
{
    void require(bool condition, const std::string& message)
    {
        if (!condition)
            throw std::runtime_error(message);
    }

    osg::ref_ptr<osg::Geometry> geometry()
    {
        auto result = osg::ref_ptr<osg::Geometry>(new osg::Geometry);
        result->setName("evaluated-uv-regression");
        auto positions = osg::ref_ptr<osg::Vec3Array>(new osg::Vec3Array);
        positions->push_back({0.0f, 0.0f, 0.0f});
        positions->push_back({1.0f, 0.0f, 0.0f});
        positions->push_back({0.0f, 1.0f, 0.0f});
        result->setVertexArray(positions);
        return result;
    }

    osg::ref_ptr<osg::Vec2Array> uv(float offset = 0.0f)
    {
        auto result = osg::ref_ptr<osg::Vec2Array>(new osg::Vec2Array);
        result->push_back({offset, 0.0f});
        result->push_back({offset + 1.0f, 0.0f});
        result->push_back({offset, 1.0f});
        return result;
    }

    RenderCore::ImmediateEffectDraw draw(std::initializer_list<unsigned int> units)
    {
        RenderCore::ImmediateEffectDraw result;
        for (unsigned int unit : units)
        {
            RenderCore::EffectTextureSnapshot texture;
            texture.texture.sourceIdentity = "textures/uv-fixture-" + std::to_string(unit) + ".dds";
            texture.binding.transform.uvSet = unit;
            result.textures.push_back(std::move(texture));
        }
        return result;
    }

    bool capture(const osg::Geometry& input, const osg::StateSet& state,
        RenderCore::ImmediateEffectDraw& output, std::string& diagnostic)
    {
#ifdef OPENMW_V4_TEST_LEGACY_UV
        return MWRender::v4_effect_detail::captureLegacyEffectTextureCoordinates(input, state, output, diagnostic);
#else
        return MWRender::v4_effect_detail::captureEffectTextureCoordinates(input, state, output, diagnostic);
#endif
    }
}

int main()
{
    int failures = 0;
    int tests = 0;
    auto test = [&](const char* name, auto&& run) {
        ++tests;
        try { run(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& error)
        {
            ++failures;
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    };
    auto state = osg::ref_ptr<osg::StateSet>(new osg::StateSet);
    test("untextured drawable does not require UVs", [&] {
        auto input = geometry(); auto output = draw({}); std::string diagnostic;
        require(capture(*input, *state, output, diagnostic), diagnostic);
        require(output.mesh.texCoordSets.empty(), "untextured geometry acquired coordinates");
    });
    test("unit-zero explicit UVs are preserved", [&] {
        auto input = geometry(); input->setTexCoordArray(0, uv());
        auto output = draw({0}); std::string diagnostic;
        require(capture(*input, *state, output, diagnostic), diagnostic);
        require(output.mesh.texCoordSets[0][1] == glm::vec2(1.0f, 0.0f), "explicit UV changed");
    });
    test("sparse bound unit does not require unbound slots", [&] {
        auto input = geometry(); input->setTexCoordArray(3, uv(3.0f));
        auto output = draw({3}); std::string diagnostic;
        require(capture(*input, *state, output, diagnostic), diagnostic);
        require(output.mesh.texCoordSets.size() == 1, "sparse slots were materialized");
        require(output.textures[0].binding.transform.uvSet == 0, "binding was not compacted");
        require(output.mesh.texCoordSets[0][0].x == 3.0f, "wrong source UV set");
    });
    test("added explicit stage inherits native unit-zero array", [&] {
        auto input = geometry(); input->setTexCoordArray(0, uv());
        auto output = draw({0, 4}); std::string diagnostic;
        require(capture(*input, *state, output, diagnostic), diagnostic);
        require(output.mesh.texCoordSets.size() == 1, "one source array was duplicated for a second stage");
        require(output.textures[0].binding.transform.uvSet == 0
            && output.textures[1].binding.transform.uvSet == 0, "shared source bindings diverged");
        require(!input->getTexCoordArray(4), "capture mutated authoritative source geometry");
    });
    test("native fallback uses first non-null array when zero is absent", [&] {
        auto input = geometry(); input->setTexCoordArray(2, uv(2.0f));
        auto output = draw({5}); std::string diagnostic;
        require(capture(*input, *state, output, diagnostic), diagnostic);
        require(output.mesh.texCoordSets[0][0].x == 2.0f, "first source array was not retained");
        require(!input->getTexCoordArray(0), "capture modified source unit zero");
    });
    test("explicit requested array takes precedence over fallback", [&] {
        auto input = geometry(); input->setTexCoordArray(0, uv()); input->setTexCoordArray(3, uv(3.0f));
        auto output = draw({3}); std::string diagnostic;
        require(capture(*input, *state, output, diagnostic), diagnostic);
        require(output.mesh.texCoordSets[0][0].x == 3.0f, "explicit UVs were replaced");
    });
    test("original texture unit owns TexMat after compaction", [&] {
        auto input = geometry(); input->setTexCoordArray(0, uv());
        auto local = osg::ref_ptr<osg::StateSet>(new osg::StateSet);
        local->setTextureAttribute(0, new osg::TexMat(osg::Matrix::translate(1.0, 0.0, 0.0)));
        local->setTextureAttribute(4, new osg::TexMat(osg::Matrix::translate(4.0, 0.0, 0.0)));
        auto output = draw({0, 4}); std::string diagnostic;
        require(capture(*input, *local, output, diagnostic), diagnostic);
        require(output.mesh.texCoordSets[0][0].x == 1.0f, "unit-zero matrix lost");
        require(output.mesh.texCoordSets[1][0].x == 4.0f, "fallback used source unit's matrix");
        require(output.textures[1].binding.transform.uvSet == 1, "secondary binding not remapped");
    });
    test("eight bound stages sharing one authored stream remain one stream", [&] {
        auto input = geometry(); auto shared = uv(2.0f);
        for (unsigned int unit = 0; unit < 8; ++unit) input->setTexCoordArray(unit, shared);
        auto output = draw({0, 1, 2, 3, 4, 5, 6, 7}); std::string diagnostic;
        require(capture(*input, *state, output, diagnostic), diagnostic);
        require(output.mesh.texCoordSets.size() == 1, "aliased texture stages consumed extra vertex inputs");
        require(output.textures.size() == 8, "a material texture stage was removed");
        for (const auto& texture : output.textures)
            require(texture.binding.transform.uvSet == 0, "shared stage was not remapped");
    });
    test("eight native fallback stages fit the four-stream shader contract", [&] {
        auto input = geometry(); input->setTexCoordArray(0, uv());
        auto output = draw({0, 1, 2, 3, 4, 5, 6, 7}); std::string diagnostic;
        require(capture(*input, *state, output, diagnostic), diagnostic);
        require(output.mesh.texCoordSets.size() == 1, "fallback duplicated coordinates past backend capacity");
        require(output.textures.size() == 8, "textures were truncated to fit the shader");
        require(!input->getTexCoordArray(7), "capture modified the source");
    });
    test("five stages using four source streams do not require a fifth vertex input", [&] {
        auto input = geometry();
        for (unsigned int unit = 0; unit < 4; ++unit) input->setTexCoordArray(unit, uv(float(unit)));
        input->setTexCoordArray(4, input->getTexCoordArray(1));
        auto output = draw({0, 1, 2, 3, 4}); std::string diagnostic;
        require(capture(*input, *state, output, diagnostic), diagnostic);
        require(output.mesh.texCoordSets.size() == 4, "stage count was confused with source-stream count");
        require(output.textures[4].binding.transform.uvSet == 1, "aliased fifth stage lost its source");
    });
    test("equal TexMat values share a source despite distinct TexMat objects", [&] {
        auto input = geometry(); input->setTexCoordArray(0, uv());
        auto local = osg::ref_ptr<osg::StateSet>(new osg::StateSet);
        local->setTextureAttribute(0, new osg::TexMat(osg::Matrix::translate(2.0, 0.0, 0.0)));
        local->setTextureAttribute(5, new osg::TexMat(osg::Matrix::translate(2.0, 0.0, 0.0)));
        auto output = draw({0, 5}); std::string diagnostic;
        require(capture(*input, *local, output, diagnostic), diagnostic);
        require(output.mesh.texCoordSets.size() == 1, "identical evaluated transforms were duplicated");
        require(output.mesh.texCoordSets[0][0].x == 2.0f, "transform was lost");
    });
    test("missing TexMat and explicit identity share the same source", [&] {
        auto input = geometry(); input->setTexCoordArray(0, uv());
        auto local = osg::ref_ptr<osg::StateSet>(new osg::StateSet);
        local->setTextureAttribute(4, new osg::TexMat(osg::Matrix::identity()));
        auto output = draw({0, 4}); std::string diagnostic;
        require(capture(*input, *local, output, diagnostic), diagnostic);
        require(output.mesh.texCoordSets.size() == 1, "identity TexMat created a redundant stream");
    });
    test("independent authored arrays remain independent even when values coincide", [&] {
        auto input = geometry(); input->setTexCoordArray(0, uv()); input->setTexCoordArray(1, uv());
        auto output = draw({0, 1}); std::string diagnostic;
        require(capture(*input, *state, output, diagnostic), diagnostic);
        require(output.mesh.texCoordSets.size() == 2, "independently animated sources were conflated");
    });
    test("animated TexMat divergence separates formerly shared streams", [&] {
        auto input = geometry(); input->setTexCoordArray(0, uv());
        auto local = osg::ref_ptr<osg::StateSet>(new osg::StateSet);
        auto matrix = osg::ref_ptr<osg::TexMat>(new osg::TexMat(osg::Matrix::identity()));
        local->setTextureAttribute(4, matrix);
        auto first = draw({0, 4}); std::string diagnostic;
        require(capture(*input, *local, first, diagnostic), diagnostic);
        require(first.mesh.texCoordSets.size() == 1, "identity stage did not share source");
        matrix->setMatrix(osg::Matrix::translate(4.0, 0.0, 0.0));
        auto second = draw({0, 4});
        require(capture(*input, *local, second, diagnostic), diagnostic);
        require(second.mesh.texCoordSets.size() == 2, "a stale alias hid the animated transform");
        require(second.mesh.texCoordSets[1][0].x == 4.0f, "new transform was not evaluated");
        require(first.mesh.texCoordSets[0][0].x == 0.0f, "previous immutable capture was modified");
    });
    test("five genuinely distinct sources are preserved, never silently truncated", [&] {
        auto input = geometry();
        for (unsigned int unit = 0; unit < 5; ++unit) input->setTexCoordArray(unit, uv(float(unit)));
        auto output = draw({0, 1, 2, 3, 4}); std::string diagnostic;
        require(capture(*input, *state, output, diagnostic), diagnostic);
        require(output.mesh.texCoordSets.size() == 5, "distinct authored UVs were discarded");
        require(output.textures[4].binding.transform.uvSet == 4, "unsupported distinct UVs were aliased");
    });
    test("missing all UV arrays remains a diagnostic failure", [&] {
        auto input = geometry(); auto output = draw({2}); std::string diagnostic;
        require(!capture(*input, *state, output, diagnostic), "coordinates were fabricated");
#ifndef OPENMW_V4_TEST_LEGACY_UV
        require(diagnostic.find("unit=2") != std::string::npos, "missing texture-unit provenance");
        require(diagnostic.find("textures/uv-fixture-2.dds") != std::string::npos, "missing texture identity");
        require(diagnostic.find("evaluated-uv-regression") != std::string::npos, "missing drawable identity");
#endif
    });
    test("wrong-length explicit UVs are not replaced with fallback", [&] {
        auto input = geometry(); input->setTexCoordArray(0, uv());
        auto bad = uv(); bad->pop_back(); input->setTexCoordArray(1, bad);
        auto output = draw({1}); std::string diagnostic;
        require(!capture(*input, *state, output, diagnostic), "malformed authored array was hidden");
    });
    test("wrong-type explicit UVs are not replaced with fallback", [&] {
        auto input = geometry(); input->setTexCoordArray(0, uv());
        input->setTexCoordArray(1, new osg::Vec3Array(3));
        auto output = draw({1}); std::string diagnostic;
        require(!capture(*input, *state, output, diagnostic), "unsupported array type was hidden");
    });
    test("generated coordinates are not fabricated from unrelated mesh UVs", [&] {
        auto input = geometry(); input->setTexCoordArray(0, uv());
        auto local = osg::ref_ptr<osg::StateSet>(new osg::StateSet);
        local->setTextureMode(2, GL_TEXTURE_GEN_S, osg::StateAttribute::ON);
        local->setTextureMode(2, GL_TEXTURE_GEN_T, osg::StateAttribute::ON);
        auto output = draw({2}); std::string diagnostic;
        require(!capture(*input, *local, output, diagnostic), "TexGen was silently treated as mesh coordinates");
#ifndef OPENMW_V4_TEST_LEGACY_UV
        require(diagnostic.find("texgen=enabled") != std::string::npos, "missing TexGen provenance");
#endif
    });
    test("failure does not partially remap earlier bindings", [&] {
        auto input = geometry(); input->setTexCoordArray(3, uv());
        auto bad = uv(); bad->pop_back(); input->setTexCoordArray(4, bad);
        auto output = draw({3, 4}); output.mesh.texCoordSets = {{glm::vec2(9.0f)}};
        std::string diagnostic;
        require(!capture(*input, *state, output, diagnostic), "bad later stage accepted");
#ifndef OPENMW_V4_TEST_LEGACY_UV
        require(output.textures[0].binding.transform.uvSet == 3, "earlier binding changed on failure");
        require(output.mesh.texCoordSets.size() == 1 && output.mesh.texCoordSets[0][0].x == 9.0f,
            "mesh coordinates changed on failure");
#endif
    });
    std::cout << (tests - failures) << '/' << tests << " effect UV tests passed\n";
    return failures ? 1 : 0;
}
