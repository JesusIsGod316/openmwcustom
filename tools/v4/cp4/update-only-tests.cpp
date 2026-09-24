#include <apps/openmw/mwrender/v4updateonlyviewer.hpp>
#include <components/sceneutil/skeleton.hpp>
#include <components/sceneutil/riggeometry.hpp>
#include <components/sceneutil/morphgeometry.hpp>
#include <components/sceneutil/deformationintersectionvisitor.hpp>
#include <osg/MatrixTransform>
#include <osg/ShapeDrawable>
#include <osgUtil/IntersectionVisitor>
#include <osgUtil/LineSegmentIntersector>
#include <iostream>
#include <stdexcept>

namespace
{
    void require(bool value, const char* message)
    {
        if (!value) throw std::runtime_error(message);
    }

    struct MoveBone : osg::NodeCallback
    {
        unsigned calls = 0;
        void operator()(osg::Node* node, osg::NodeVisitor* nv) override
        {
            ++calls;
            static_cast<osg::MatrixTransform*>(node)->setMatrix(
                osg::Matrix::translate(nv->getFrameStamp()->getSimulationTime(), 0, 0));
            traverse(node, nv);
        }
    };

    struct MoveCamera : osg::NodeCallback
    {
        unsigned calls = 0;
        void operator()(osg::Node* node, osg::NodeVisitor* nv) override
        {
            ++calls;
            const double x = nv->getFrameStamp()->getFrameNumber() % 2 ? 0 : 10;
            static_cast<osg::Camera*>(node)->setViewMatrix(osg::Matrix::translate(-x, 0, 0));
            traverse(node, nv);
        }
    };

    struct CountEvents : osgGA::GUIEventHandler
    {
        unsigned frames = 0, keys = 0;
        bool handle(const osgGA::GUIEventAdapter& event, osgGA::GUIActionAdapter&) override
        {
            if (event.getEventType() == osgGA::GUIEventAdapter::FRAME) ++frames;
            if (event.getEventType() == osgGA::GUIEventAdapter::KEYUP) ++keys;
            return false;
        }
    };

    osg::ref_ptr<osg::Geode> target(const char* name, float x)
    {
        osg::ref_ptr<osg::Geode> node = new osg::Geode;
        node->setName(name);
        node->addDrawable(new osg::ShapeDrawable(new osg::Box(osg::Vec3(x, 0, -5), 2)));
        return node;
    }
}

int main()
{
    int failures = 0;
    const auto test = [&](const char* name, auto run) {
        std::cout << "RUN " << name << std::endl;
        try { run(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& e) { ++failures; std::cerr << "FAIL " << name << ": " << e.what() << '\n'; }
    };
    test("headless picking follows skinned and morphed triangles without cull", [] {
        auto source = osg::ref_ptr<osg::Geometry>(new osg::Geometry);
        auto vertices = osg::ref_ptr<osg::Vec3Array>(new osg::Vec3Array);
        vertices->push_back({-1,-1,-5}); vertices->push_back({1,-1,-5}); vertices->push_back({0,1,-5});
        source->setVertexArray(vertices);
        source->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES,0,3));
        auto rig = osg::ref_ptr<SceneUtil::RigGeometry>(new SceneUtil::RigGeometry);
        rig->setName("Geometry");
        rig->setSourceGeometry(source);
        rig->setBoneInfo({{"Bone", osg::BoundingSpheref(osg::Vec3f(0,0,-5),2.f), osg::Matrixf::identity()}});
        rig->setInfluences({{{0,1.f}},{{0,1.f}},{{0,1.f}}});
        rig->setTransform(osg::Matrixf::identity());
        auto bone = osg::ref_ptr<osg::MatrixTransform>(new osg::MatrixTransform);
        bone->setName("Bone");
        auto skeleton = osg::ref_ptr<SceneUtil::Skeleton>(new SceneUtil::Skeleton);
        skeleton->setName("Skeleton");
        skeleton->addChild(bone); skeleton->addChild(rig);
        auto morph = osg::ref_ptr<SceneUtil::MorphGeometry>(new SceneUtil::MorphGeometry);
        morph->setSourceGeometry(source);
        morph->addMorphTarget(vertices);
        auto offsets = osg::ref_ptr<osg::Vec3Array>(new osg::Vec3Array(3));
        for (auto& offset : *offsets) offset.set(6,0,0);
        morph->addMorphTarget(offsets,0);
        auto root = osg::ref_ptr<osg::Group>(new osg::Group);
        root->addChild(skeleton); root->addChild(morph);
        skeleton->setNodeMask(1); morph->setNodeMask(2);
        auto stamp = osg::ref_ptr<osg::FrameStamp>(new osg::FrameStamp);
        SceneUtil::UpdateOnlyVisitor update;
        update.setFrameStamp(stamp);
        const auto hit = [&](double x, unsigned frame, unsigned mask, bool refresh) {
            auto ray = osg::ref_ptr<osgUtil::LineSegmentIntersector>(
                new osgUtil::LineSegmentIntersector(osg::Vec3d(x,0,0),osg::Vec3d(x,0,-10)));
            SceneUtil::DeformationIntersectionVisitor visitor;
            visitor.setIntersector(ray); visitor.setTraversalNumber(frame);
            visitor.setTraversalMask(mask); visitor.evaluateDeformation = refresh;
            root->accept(visitor);
            return ray->containsIntersections();
        };
        for (unsigned frame=1; frame<=6; ++frame)
        {
            const double x = frame % 2 ? 6 : 3;
            bone->setMatrix(osg::Matrix::translate(x,0,0));
            morph->getMorphTarget(1).setWeight(static_cast<float>(x/6)); morph->dirty();
            stamp->setFrameNumber(frame); update.setTraversalNumber(frame); root->accept(update);
            // Bounds advance during update; primitive intersection historically still reads the stale buffer.
            require(!hit(x,frame,1,false), "rig control no longer reproduces stale picking");
            require(!hit(x,frame,2,false), "morph control no longer reproduces stale picking");
            require(hit(x,frame,1,true), "fresh skinned triangle is not pickable");
            require(hit(x,frame,2,true), "fresh morphed triangle is not pickable");
            require(!hit(x,frame,4,true), "picking bypassed traversal masks");
            require(!hit(0,frame,3,true), "bind-pose triangle remains pickable");
        }
        require((*vertices)[0].x() == -1, "picking mutated shared source geometry");
    });
    test("reproduce ordinary Viewer no-window shutdown", [] {
        osg::ref_ptr<osgViewer::Viewer> viewer = new osgViewer::Viewer;
        viewer->setSceneData(new osg::Group);
        viewer->advance(0.1);
        viewer->eventTraversal();
        require(viewer->done(), "baseline no longer reproduces; re-audit OSG lifecycle");
        const auto frame = viewer->getFrameStamp()->getFrameNumber();
        viewer->advance(0.2);
        require(viewer->getFrameStamp()->getFrameNumber() == frame, "baseline done frame advanced");
    });
    test("headless frames animate semi-active bones and refresh actual picking", [] {
        osg::ref_ptr<MWRender::V4UpdateOnlyViewer> viewer = new MWRender::V4UpdateOnlyViewer;
        osg::ref_ptr<osg::Group> root = new osg::Group;
        osg::ref_ptr<SceneUtil::Skeleton> skeleton = new SceneUtil::Skeleton;
        osg::ref_ptr<osg::MatrixTransform> bone = new osg::MatrixTransform;
        osg::ref_ptr<MoveBone> motion = new MoveBone;
        bone->setName("test bone");
        bone->setUpdateCallback(motion);
        skeleton->addChild(bone);
        skeleton->setActive(SceneUtil::Skeleton::SemiActive);
        root->addChild(skeleton);
        root->addChild(target("left", 0));
        root->addChild(target("right", 10));
        auto* evaluated = skeleton->getBone("test bone");
        require(evaluated != nullptr, "bone fixture absent");
        viewer->setSceneData(root);
        viewer->getCamera()->setProjectionMatrixAsPerspective(60, 1, 0.1, 100);
        osg::ref_ptr<MoveCamera> camera = new MoveCamera;
        viewer->getCamera()->setUpdateCallback(camera);
        osg::ref_ptr<CountEvents> handler = new CountEvents;
        osg::ref_ptr<CountEvents> sceneEvents = new CountEvents;
        osg::ref_ptr<CountEvents> cameraEvents = new CountEvents;
        root->setEventCallback(sceneEvents);
        viewer->getCamera()->setEventCallback(cameraEvents);
        viewer->addEventHandler(handler);
        viewer->getEventQueue()->keyRelease(osgGA::GUIEventAdapter::KEY_Escape, 0.0);
        for (unsigned i = 1; i <= 12; ++i)
        {
            viewer->advance(i * 0.25);
            viewer->eventTraversal();
            viewer->updateTraversal();
            require(!viewer->done(), "headless event or Escape shut down viewer");
            require(viewer->getFrameStamp()->getFrameNumber() == i, "frame stamp stopped");
            skeleton->updateBoneMatrices(i);
            require(evaluated->mMatrixInSkeletonSpace.getTrans().x() == i * 0.25,
                "canonical evaluated skeleton pose did not advance");
            osg::ref_ptr<osgUtil::LineSegmentIntersector> ray = new osgUtil::LineSegmentIntersector(
                osgUtil::Intersector::PROJECTION, 0, 0);
            osgUtil::IntersectionVisitor visitor(ray);
            viewer->getCamera()->accept(visitor);
            require(ray->containsIntersections(), "live camera ray missed targets");
            const auto& path = ray->getFirstIntersection().nodePath;
            const std::string expected = i % 2 ? "left" : "right";
            bool hitExpected = false;
            for (const auto* node : path)
                hitExpected = hitExpected || node->getName() == expected;
            if (!hitExpected)
            {
                std::cerr << "frame=" << i << " cameraX=" << viewer->getCamera()->getViewMatrix().getTrans().x()
                          << " expected=" << expected << " path=";
                for (const auto* node : path) std::cerr << node->className() << ':' << node->getName() << '/';
                std::cerr << '\n';
            }
            require(hitExpected, "picking used stale view matrix");
        }
        require(motion->calls == 12 && camera->calls == 12, "duplicate or missing update callbacks");
        require(handler->frames == 12 && handler->keys == 1, "registered event delivery failed");
        require(sceneEvents->frames == 12 && sceneEvents->keys == 1, "scene events duplicated or missing");
        require(cameraEvents->frames == 12 && cameraEvents->keys == 1, "camera events duplicated or missing");
        require(viewer->getCamera()->getGraphicsContext() == nullptr, "repair created graphics context");
        // Loading-screen order repeats update before advance with frozen simulation time.
        viewer->eventTraversal();
        viewer->updateTraversal();
        viewer->advance(3.0);
        require(viewer->getFrameStamp()->getFrameNumber() == 13, "loading frame did not advance");
        viewer->setDone(true);
        viewer->advance(4);
        viewer->eventTraversal();
        viewer->updateTraversal();
        require(viewer->getFrameStamp()->getFrameNumber() == 13 && motion->calls == 13,
            "explicit shutdown was ignored");
    });
    test("inactive policy and legacy offscreen control remain intact", [] {
        osg::ref_ptr<SceneUtil::Skeleton> skeleton = new SceneUtil::Skeleton;
        osg::ref_ptr<osg::MatrixTransform> bone = new osg::MatrixTransform;
        osg::ref_ptr<MoveBone> motion = new MoveBone;
        bone->setUpdateCallback(motion);
        skeleton->addChild(bone);
        skeleton->updateBoneMatrices(1);
        osg::ref_ptr<osg::FrameStamp> stamp = new osg::FrameStamp;
        osgUtil::UpdateVisitor legacy;
        legacy.setFrameStamp(stamp);
        legacy.setTraversalNumber(10);
        SceneUtil::UpdateOnlyVisitor cpu;
        cpu.setFrameStamp(stamp);
        cpu.setTraversalNumber(10);
        skeleton->setActive(SceneUtil::Skeleton::SemiActive);
        skeleton->accept(legacy);
        require(motion->calls == 0, "OpenGL offscreen policy changed");
        skeleton->accept(cpu);
        require(motion->calls == 1, "CPU semiactive skeleton was cull gated");
        skeleton->setActive(SceneUtil::Skeleton::Inactive);
        skeleton->accept(cpu);
        require(motion->calls == 1, "inactive range policy bypassed");
        skeleton->setActive(SceneUtil::Skeleton::Active);
        skeleton->accept(legacy);
        require(motion->calls == 2, "OpenGL active animation stopped");
    });
    test("CPU particle updates advance without GL draws and preserve explicit freeze and legacy cull policy", [] {
        auto particles = osg::ref_ptr<osgParticle::ParticleSystem>(new osgParticle::ParticleSystem);
        particles->setFreezeOnCull(true);
        auto* particle = particles->createParticle(nullptr);
        particle->setLifeTime(100);
        particle->setVelocity(osg::Vec3(2, 0, 0));
        auto updater = osg::ref_ptr<osgParticle::ParticleSystemUpdater>(new osgParticle::ParticleSystemUpdater);
        updater->addParticleSystem(particles);
        auto stamp = osg::ref_ptr<osg::FrameStamp>(new osg::FrameStamp);
        SceneUtil::ParticlePlaybackVisitor cpu;
        cpu.setFrameStamp(stamp);
        for (unsigned i = 10; i <= 14; ++i)
        {
            stamp->setFrameNumber(i);
            stamp->setSimulationTime((i - 10) * 0.1);
            cpu.setTraversalNumber(i);
            updater->accept(cpu);
            require(particles->getFreezeOnCull(), "CPU traversal leaked its cull policy");
        }
        require(particle->getPosition().x() > 0.5f, "particles still depend on an OpenGL draw");
        const float before = particle->getPosition().x();
        particles->setFrozen(true);
        stamp->setFrameNumber(15); stamp->setSimulationTime(0.5); cpu.setTraversalNumber(15);
        updater->accept(cpu);
        require(particle->getPosition().x() == before && particles->isFrozen(), "controller freeze bypassed");
        particles->setFrozen(false);
        auto legacy = osg::ref_ptr<osgUtil::CullVisitor>(new osgUtil::CullVisitor);
        legacy->setFrameStamp(stamp);
        stamp->setFrameNumber(100); stamp->setSimulationTime(0.6); legacy->setTraversalNumber(100);
        updater->traverse(*legacy);
        require(particle->getPosition().x() == before, "legacy cull gate changed");
    });
    test("accidental OpenGL rendering fails explicitly", [] {
        osg::ref_ptr<MWRender::V4UpdateOnlyViewer> viewer = new MWRender::V4UpdateOnlyViewer;
        bool realizeRejected = false, renderRejected = false;
        try { viewer->realize(); } catch (const std::logic_error&) { realizeRejected = true; }
        try { viewer->renderingTraversals(); } catch (const std::logic_error&) { renderRejected = true; }
        require(realizeRejected && renderRejected, "OpenGL presentation was silently permitted");
    });
    return failures == 0 ? 0 : 1;
}
