// Real VSG view initialization and record-time light collection. No GPU, game,
// asset/configuration access, or hand-sized replacement light buffer is used.
#include <components/render/backend/vsg/openmwviewdependentstate.hpp>

#include <vsg/all.h>

#include <cstdlib>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
    void require(bool condition, const std::string& message)
    {
        if (!condition)
            throw std::runtime_error(message);
    }

    struct Fixture
    {
        vsg::ref_ptr<vsg::View> view;
        vsg::ref_ptr<RenderVsg::OpenMwViewDependentState> state;
        vsg::ref_ptr<vsg::RecordTraversal> record = vsg::RecordTraversal::create();
        vsg::ResourceRequirements requirements;

        explicit Fixture(vsg::ViewFeatures features, unsigned int cascades = 0)
            : view(vsg::View::create(features))
            , state(RenderVsg::OpenMwViewDependentState::create(view.get()))
        {
            view->viewDependentState = state;
            requirements.numShadowMapsRange = { cascades, cascades };
            state->init(requirements);
            record->viewDependentState = state;
        }

        void collect()
        {
            state->clear();
            view->traverse(*record);
        }

        void update()
        {
            collect();
            state->traverse(*record);
        }
    };

    void addAmbientAndSun(vsg::View& view)
    {
        auto ambient = vsg::AmbientLight::create();
        ambient->color = { .2f, .3f, .4f };
        auto sun = vsg::DirectionalLight::create();
        sun->color = { .6f, .5f, .4f };
        sun->direction = { 0.0, 0.0, -1.0 };
        view.addChild(ambient);
        view.addChild(sun);
    }

    std::vector<vsg::vec4> values(const vsg::vec4Array& data)
    {
        return { data.begin(), data.end() };
    }
}

int main()
{
    if (std::getenv("OPENMW_V4_LEGACY_UNSHADOWED_LIGHTS_CONTROL"))
    {
        std::cerr << "FAIL: unset the legacy-lighting control for this regression test\n";
        return 1;
    }
    unsigned passed = 0;
    unsigned total = 0;
    const auto test = [&](const char* name, const std::function<void()>& run) {
        ++total;
        try
        {
            run();
            ++passed;
            std::cout << "PASS " << name << '\n';
        }
        catch (const std::exception& error)
        {
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    };

    for (const auto features : { vsg::ViewFeatures(0), vsg::INHERIT_VIEWPOINT })
    {
        test(features == 0 ? "no-feature depth view skips incidental lights"
                           : "inherited depth view skips incidental lights", [=] {
            Fixture f(features);
            require(f.state->lightData->size() == 1, "depth-only light allocation unexpectedly changed");
            addAmbientAndSun(*f.view);
            const auto before = values(*f.state->lightData);
            f.collect();
            require(f.state->ambientLights.size() == 1 && f.state->directionalLights.size() == 1,
                "real VSG record traversal did not encounter inherited lights");
            f.state->traverse(*f.record);
            require(values(*f.state->lightData) == before, "depth-only light data was overwritten");
            require(f.state->shadowMaps.empty(), "depth view allocated nested shadow maps");
        });
    }

    test("all three VSG-generated shadow cameras preserve their depth-only contract", [] {
        Fixture parent(vsg::RECORD_ALL, 3);
        addAmbientAndSun(*parent.view);
        require(parent.state->shadowMaps.size() == 3, "actual VSG shadow cameras were not created");
        for (auto& entry : parent.state->shadowMaps)
        {
            auto view = entry.view;
            require(view->features == vsg::INHERIT_VIEWPOINT, "pinned shadow-camera flags changed");
            auto state = view->viewDependentState.cast<RenderVsg::OpenMwViewDependentState>();
            require(bool(state), "runtime did not extend the generated shadow view");
            vsg::ResourceRequirements requirements;
            state->init(requirements);
            require(state->lightData->size() == 1, "generated shadow view should reserve only its count header");
            auto record = vsg::RecordTraversal::create();
            record->viewDependentState = state;
            // This follows the actual generated TraverseChildrenOfNode back to
            // the parent's children, rather than manually seeding light arrays.
            view->traverse(*record);
            require(state->ambientLights.size() == view->children.size()
                    && state->directionalLights.size() == view->children.size(),
                "generated shadow traversal counts ambient=" + std::to_string(state->ambientLights.size())
                + " directional=" + std::to_string(state->directionalLights.size()));
            const auto before = values(*state->lightData);
            state->traverse(*record);
            require(values(*state->lightData) == before && state->shadowMaps.empty(),
                "shadow depth data changed or nested shadows were allocated");
        }
    });

    for (const auto features : { vsg::RECORD_LIGHTS,
             vsg::ViewFeatures(vsg::RECORD_LIGHTS | vsg::INHERIT_VIEWPOINT) })
    {
        test(features == vsg::RECORD_LIGHTS ? "lighting-only preview still packs ambient and directional data"
                                          : "inherited lighting-only view still packs ambient and directional data", [=] {
            Fixture f(features, 3);
            require(f.requirements.numShadowMapsRange == vsg::uivec2(3, 3), "shadow hints leaked to other views");
            require(f.state->shadowMaps.empty(), "lighting-only view allocated unused shadow passes");
            addAmbientAndSun(*f.view);
            f.update();
            require((*f.state->lightData)[0] == vsg::vec4(1, 1, 0, 0), "lighting counts missing");
            require((*f.state->lightData)[1] == vsg::vec4(.2f, .3f, .4f, 1), "ambient values missing");
            require((*f.state->lightData)[2] == vsg::vec4(.6f, .5f, .4f, 1), "directional values missing");
            require((*f.state->lightData)[3] == vsg::vec4(0, 0, -1, 0), "direction was not preserved");
            require((*f.state->lightData)[4] == vsg::vec4(0, 0, 0, 0), "unshadowed light gained shadow payload");
        });
    }

    test("mixed ambient directional point and spot light slot counts match the compiled layout", [] {
        Fixture f(vsg::RECORD_LIGHTS);
        addAmbientAndSun(*f.view);
        f.view->addChild(vsg::PointLight::create());
        f.view->addChild(vsg::SpotLight::create());
        f.update();
        require((*f.state->lightData)[0] == vsg::vec4(1, 1, 1, 1), "mixed light counts disagree");
        require(f.state->lightData->size() == 33, "default lighting reservation changed");
        require(f.state->shadowMaps.empty(), "mixed lights enabled shadow recording");
    });

    test("exact compiled capacity works without growing the descriptor", [] {
        Fixture f(vsg::RECORD_LIGHTS);
        const auto data = f.state->lightData;
        const auto descriptor = f.state->descriptorSet;
        for (unsigned int i = 0; i < 8; ++i)
            f.view->addChild(vsg::SpotLight::create());
        f.update();
        require((*data)[0] == vsg::vec4(0, 0, 0, 8), "exact-capacity light counts disagree");
        require(data->size() == 33 && f.state->lightData == data && f.state->descriptorSet == descriptor,
            "recording grew/replaced a compiled light buffer or descriptor");
    });

    test("genuine lit-view overflow still fails before any writes", [] {
        Fixture f(vsg::RECORD_LIGHTS);
        for (unsigned int i = 0; i < 9; ++i)
            f.view->addChild(vsg::SpotLight::create());
        const auto before = values(*f.state->lightData);
        std::string diagnostic;
        try { f.update(); }
        catch (const std::runtime_error& error) { diagnostic = error.what(); }
        require(diagnostic.find("exceeds its compiled buffer capacity") != std::string::npos,
            "genuine overflow was silently accepted");
        require(values(*f.state->lightData) == before, "overflow partially modified the light buffer");
    });

    test("genuine overflow reports view flags required slots capacity and light counts", [] {
        Fixture f(vsg::RECORD_LIGHTS);
        for (unsigned int i = 0; i < 9; ++i)
            f.view->addChild(vsg::SpotLight::create());
        std::string diagnostic;
        try { f.update(); }
        catch (const std::runtime_error& error) { diagnostic = error.what(); }
        for (const auto* field : { "view=", "features=2", "required_vec4=37", "capacity_vec4=33",
                 "ambient=0", "directional=0", "point=0", "spot=9" })
            require(diagnostic.find(field) != std::string::npos, std::string("overflow diagnostic omits ") + field);
    });

    test("light changes and removal update data without reallocating the compiled buffer", [] {
        Fixture f(vsg::RECORD_LIGHTS);
        const auto data = f.state->lightData;
        auto ambient = vsg::AmbientLight::create();
        f.view->addChild(ambient);
        f.update();
        vsg::ModifiedCount modified;
        data->getModifiedCount(modified);
        f.update();
        require(!data->differentModifiedCount(modified), "unchanged light values dirtied their upload");
        ambient->color = { .1f, .2f, .3f };
        f.update();
        require(data->differentModifiedCount(modified), "light color change did not dirty the upload");
        require((*data)[1] == vsg::vec4(.1f, .2f, .3f, 1), "changed ambient color lost");
        f.view->children.clear();
        f.update();
        require((*data)[0] == vsg::vec4(0, 0, 0, 0), "removed lights remain active");
        require(data == f.state->lightData, "light update replaced the compiled buffer");
    });

    test("reinitializing a lit view does not duplicate OpenMW descriptor bindings", [] {
        Fixture f(vsg::RECORD_LIGHTS);
        const auto data = f.state->lightData;
        const auto bindings = f.state->descriptorSetLayout->bindings.size();
        f.state->init(f.requirements);
        require(data == f.state->lightData && bindings == f.state->descriptorSetLayout->bindings.size(),
            "reinitialization changed the compiled descriptor contract");
    });

    std::cout << passed << '/' << total << " view-feature tests passed\n";
    return passed == total ? 0 : 1;
}
