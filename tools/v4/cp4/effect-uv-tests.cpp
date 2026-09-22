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
        require(output.mesh.texCoordSets.size() == 2, "wrong compact stream count");
        require(output.mesh.texCoordSets[0] == output.mesh.texCoordSets[1], "native array fallback lost");
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
