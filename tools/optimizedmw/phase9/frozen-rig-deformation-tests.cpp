#include <components/sceneutil/frozenrigdeformation.hpp>
#include <components/sceneutil/riggeometry.hpp>
#include <components/sceneutil/skeleton.hpp>
#include <osg/MatrixTransform>
#include <iostream>
#include <stdexcept>

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
std::uint64_t hash(const osg::Geometry& geometry)
{
    std::uint64_t value = 1469598103934665603ull;
    for (const auto* array : { geometry.getVertexArray(), geometry.getNormalArray(), geometry.getTexCoordArray(7) })
        if (array)
        {
            const auto* bytes = static_cast<const unsigned char*>(array->getDataPointer());
            for (unsigned i = 0; i < array->getTotalDataSize(); ++i) value = (value ^ bytes[i]) * 1099511628211ull;
        }
    return value;
}
int main() try
{
    constexpr unsigned Count = 32768;
    osg::ref_ptr<osg::Geometry> source = new osg::Geometry;
    osg::ref_ptr<osg::Vec3Array> positions = new osg::Vec3Array, normals = new osg::Vec3Array;
    osg::ref_ptr<osg::Vec4Array> tangents = new osg::Vec4Array;
    for (unsigned i = 0; i < Count; ++i)
    {
        positions->push_back(osg::Vec3(.00007f * i, .00003f * i, -.000011f * i));
        normals->push_back(osg::Vec3(.25f, .5f, .75f));
        tangents->push_back(osg::Vec4(.75f, -.5f, .25f, i % 2 ? -1.f : 1.f));
    }
    source->setVertexArray(positions); source->setNormalArray(normals, osg::Array::BIND_PER_VERTEX);
    source->setTexCoordArray(7, tangents); source->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES, 0, 3));
    const auto sourceHash = hash(*source);
    osg::ref_ptr<SceneUtil::Skeleton> skeleton = new SceneUtil::Skeleton;
    osg::ref_ptr<osg::MatrixTransform> first = new osg::MatrixTransform, second = new osg::MatrixTransform;
    first->setName("one"); second->setName("two"); skeleton->addChild(first); skeleton->addChild(second);
    osg::ref_ptr<SceneUtil::RigGeometry> rig = new SceneUtil::RigGeometry;
    rig->setSourceGeometry(source);
    SceneUtil::RigGeometry::BoneInfo one, two;
    one.mName = "one"; two.mName = "two";
    one.mBoundSphere.set(osg::Vec3(0, 0, 0), 8); two.mBoundSphere = one.mBoundSphere;
    one.mInvBindMatrix = osg::Matrixf::rotate(.15f, osg::Vec3(0, 0, 1));
    two.mInvBindMatrix = osg::Matrixf::translate(.05f, -.025f, 0);
    rig->setBoneInfo({ one, two });
    std::vector<SceneUtil::RigGeometry::BoneWeights> influences(Count);
    for (unsigned i = 0; i < Count; ++i)
        influences[i] = i % 3 == 0 ? SceneUtil::RigGeometry::BoneWeights{ { 0, 1.f } }
            : i % 3 == 1 ? SceneUtil::RigGeometry::BoneWeights{ { 1, 1.f } }
                        : SceneUtil::RigGeometry::BoneWeights{ { 0, .35f }, { 1, .65f } };
    rig->setInfluences(influences); rig->setTransform(osg::Matrixf::scale(1.1f, .9f, 1.2f));
    skeleton->addChild(rig);
    osg::NodePath path{ skeleton, rig };
    osg::Geometry* previous = nullptr;
    std::uint64_t previousHash = 0;
    for (unsigned frame = 1; frame <= 16; ++frame)
    {
        first->setMatrix(osg::Matrix::rotate(.05 * frame, osg::Vec3(0, 1, 0))
            * osg::Matrix::translate(.01 * frame, -.02 * frame, .005 * frame));
        second->setMatrix(osg::Matrix::rotate(-.07 * frame, osg::Vec3(1, 0, 0)));
        osg::Geometry* current = rig->evaluateGeometry(frame, path);
        require(current != nullptr, "actual production rig evaluation failed");
        require(rig->evaluateGeometry(frame, path) == current, "secondary view rebuilt current pose");
        if (previous) require(hash(*previous) == previousHash, "next pose mutated still-owned previous CPU slot");
        const auto currentHash = hash(*current);
        std::cout << "frame " << frame << ' ' << currentHash << '\n';
        previous = current; previousHash = currentHash;
    }
    require(hash(*source) == sourceHash, "frozen workers modified shared source arrays");
    const auto stats = SceneUtil::FrameCriticalJobGroup::instance().stats();
    const auto publications = SceneUtil::FrozenRigDeformation::Counters::publications.load();
    if (SceneUtil::FrozenRigDeformation::enabled())
    {
        require(publications == 16 && stats.groups >= 16, "production frozen rig path never activated");
        require(stats.workerItems > 0 && stats.callerItems > 0, "real reserved helpers or caller participation absent");
    }
    else require(publications == 0 && stats.peakWorkers == 0, "disabled control started deformation workers");
    // Malformed overlapping input refuses before any worker writes.
    std::array<unsigned short, 2> repeated{ 0, 0 };
    std::array<SceneUtil::FrozenRigDeformation::Range, 1> invalid{
        SceneUtil::FrozenRigDeformation::Range{ osg::Matrixf::identity(), repeated } };
    SceneUtil::FrozenRigDeformation::Output output{ *positions, normals, tangents, *positions, normals, tangents };
    require(!SceneUtil::FrozenRigDeformation::run(invalid, output, true), "overlapping vertex publication admitted");
    require(hash(*source) == sourceHash, "refused malformed work changed source arrays");
    std::cout << "publication " << publications << " helper_items " << stats.workerItems
              << " caller_items " << stats.callerItems << " peak_workers " << stats.peakWorkers << '\n';
    return 0;
}
catch (const std::exception& error)
{
    std::cerr << error.what() << '\n';
    return 1;
}
