#include <apps/openmw/mwrender/v4updateonlyviewer.hpp>
#include <components/sceneutil/skeleton.hpp>
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
        try { run(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& e) { ++failures; std::cerr << "FAIL " << name << ": " << e.what() << '\n'; }
    };
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
