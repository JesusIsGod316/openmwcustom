#define OPENMW_COMPACT_COLLISION_TEST_HOOKS 1
#include "compact-static-collision-prototype.hpp"

#include <BulletCollision/BroadphaseCollision/btDbvtBroadphase.h>
#include <BulletCollision/CollisionDispatch/btCollisionWorld.h>
#include <BulletCollision/CollisionDispatch/btDefaultCollisionConfiguration.h>
#include <BulletCollision/CollisionShapes/btSphereShape.h>
#include <BulletCollision/CollisionShapes/btTriangleMesh.h>

#include <atomic>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

namespace Compact = Resource::CompactStaticCollisionPrototype;
static_assert(sizeof(btScalar) == 8, "Preserve the qualified float64 Bullet ABI");
static_assert(sizeof(osg::Vec3f) == 12, "The closed prototype requires packed float3");
static_assert(std::is_same_v<decltype(std::declval<const Compact::Streams>().positions.get()), const osg::Vec3f*>);

namespace
{
    unsigned checks = 0, rayOracles = 0, contactOracles = 0, boundsOracles = 0;
    unsigned rayHits = 0, contactPairsWithPoints = 0;
    void require(bool condition, const char* message)
    { if (!condition) throw std::runtime_error(message); ++checks; }
    bool near(btScalar a, btScalar b) { return btFabs(a - b) <= btScalar(1e-9); }
    bool near(const btVector3& a, const btVector3& b)
    { return near(a.x(), b.x()) && near(a.y(), b.y()) && near(a.z(), b.z()); }
    void failAllocation() { throw std::bad_alloc(); }
    Compact::Certificate certificate()
    {
        Compact::Certificate value;
        value.generation = 1; value.sourceIdentity[0] = 0x53;
        value.exactStaticTriangleList = value.immutableSnapshot = true;
        return value; // Closed native input identity; not a game source digest receipt.
    }
    struct Input
    {
        std::vector<osg::Vec3f> positions;
        std::vector<std::uint16_t> indices;
    };
    Input cube()
    {
        return {{{-1,-1,-1},{1,-1,-1},{1,1,-1},{-1,1,-1},
                 {-1,-1,1},{1,-1,1},{1,1,1},{-1,1,1}},
                {0,2,1,0,3,2,4,5,6,4,6,7,0,1,5,0,5,4,
                 3,7,6,3,6,2,0,4,7,0,7,3,1,2,6,1,6,5}};
    }
    struct Control
    {
        // Actual existing indexed btTriangleMesh path; shape dies before mesh.
        std::unique_ptr<btTriangleMesh> mesh = std::make_unique<btTriangleMesh>();
        std::unique_ptr<btBvhTriangleMeshShape> shape;
        explicit Control(const Input& input)
        {
            mesh->preallocateVertices(static_cast<int>(input.positions.size()));
            for (const auto& value : input.positions)
                mesh->findOrAddVertex(btVector3(value.x(), value.y(), value.z()), false);
            mesh->preallocateIndices(static_cast<int>(input.indices.size()));
            for (std::size_t i = 0; i < input.indices.size(); i += 3)
                mesh->addTriangleIndices(input.indices[i], input.indices[i + 1], input.indices[i + 2]);
            shape = std::make_unique<btBvhTriangleMeshShape>(mesh.get(), true);
        }
    };
    struct Contacts final : btCollisionWorld::ContactResultCallback
    {
        unsigned count = 0;
        btScalar deepest = BT_LARGE_FLOAT;
        btScalar addSingleResult(btManifoldPoint& point, const btCollisionObjectWrapper*, int, int,
            const btCollisionObjectWrapper*, int, int) override
        { ++count; deepest = btMin(deepest, point.getDistance()); return 0; }
    };
    struct World
    {
        btDefaultCollisionConfiguration configuration;
        btCollisionDispatcher dispatcher{ &configuration };
        btDbvtBroadphase broadphase;
        btCollisionWorld world{ &dispatcher, &broadphase, &configuration };
        btCollisionObject object;
        World(btCollisionShape& shape, const btTransform& transform)
        {
            object.setCollisionShape(&shape); object.setWorldTransform(transform);
            world.addCollisionObject(&object);
        }
        ~World() { world.removeCollisionObject(&object); }
    };
    void compare(btBvhTriangleMeshShape& control, btBvhTriangleMeshShape& candidate,
        const btTransform& transform, const btVector3& scaling)
    {
        control.setLocalScaling(scaling); candidate.setLocalScaling(scaling);
        btVector3 controlMin, controlMax, candidateMin, candidateMax;
        control.getAabb(transform, controlMin, controlMax);
        candidate.getAabb(transform, candidateMin, candidateMax);
        require(near(controlMin, candidateMin) && near(controlMax, candidateMax), "actual BVH transformed bounds differ");
        ++boundsOracles;
        World reference(control, transform), test(candidate, transform);
        for (int axis = 0; axis < 3; ++axis)
            for (btScalar offset : {btScalar(-0.7), btScalar(0), btScalar(0.7), btScalar(4)})
            {
                btVector3 from(0,0,0), to(0,0,0);
                from[axis] = -5; to[axis] = 5;
                from[(axis + 1) % 3] = to[(axis + 1) % 3] = offset;
                from = transform * from; to = transform * to;
                btCollisionWorld::ClosestRayResultCallback a(from, to), b(from, to);
                reference.world.rayTest(from, to, a); test.world.rayTest(from, to, b);
                require(a.hasHit() == b.hasHit(), "real ray hit/miss differs from ordinary collision");
                if (a.hasHit())
                {
                    ++rayHits;
                    require(near(a.m_closestHitFraction, b.m_closestHitFraction)
                        && near(a.m_hitPointWorld, b.m_hitPointWorld) && near(a.m_hitNormalWorld, b.m_hitNormalWorld),
                        "real ray point/normal/fraction differs from ordinary collision");
                }
                ++rayOracles;
            }
        btSphereShape sphere(btScalar(0.35));
        for (btScalar position : {btScalar(0), btScalar(0.85), btScalar(1.2), btScalar(4)})
        {
            btCollisionObject probe; probe.setCollisionShape(&sphere);
            btTransform location = transform;
            location.setOrigin(transform * btVector3(position, 0, 0));
            probe.setWorldTransform(location);
            Contacts a, b;
            reference.world.contactPairTest(&reference.object, &probe, a);
            test.world.contactPairTest(&test.object, &probe, b);
            require(a.count == b.count, "real narrowphase contact count differs from ordinary collision");
            if (a.count)
            {
                ++contactPairsWithPoints;
                require(near(a.deepest, b.deepest), "real narrowphase contact depth differs");
            }
            ++contactOracles;
        }
    }
}

int main() try
{
    const auto input = cube();
    const auto streams = input.positions.size() * sizeof(osg::Vec3f) + input.indices.size() * sizeof(std::uint16_t);
    auto budget = std::make_shared<Compact::StreamBudget>(streams);
    auto result = Compact::prepare(input.positions, input.indices, certificate(), budget);
    require(result.refusal == Compact::Refusal::None && bool(result.snapshot), "supported source failed preparation");
    require(budget->stats().pending == streams && budget->stats().current == 0,
        "pending immutable backing is not charged exactly once");
    auto candidate = result.snapshot.instantiate();
    require(candidate.refusal == Compact::Refusal::None && candidate.representation, "actual BVH construction failed");
    require(budget->stats().current == streams && budget->stats().pending == 0,
        "actual representation publication did not transfer its unique charge");
    const auto& mesh = candidate.representation->interface();
    const unsigned char *vertices = nullptr, *indices = nullptr;
    int vertexCount = 0, vertexStride = 0, triangleCount = 0, indexStride = 0;
    PHY_ScalarType vertexType = PHY_DOUBLE, indexType = PHY_INTEGER;
    mesh.getLockedReadOnlyVertexIndexBase(&vertices, vertexCount, vertexType, vertexStride,
        &indices, indexStride, triangleCount, indexType);
    require(vertexType == PHY_FLOAT && vertexStride == 12 && vertexCount == 8
        && indexType == PHY_SHORT && indexStride == 6 && triangleCount == 12,
        "real locked mixed-precision mesh layout differs");
    require(vertices == reinterpret_cast<const unsigned char*>(result.snapshot.data()->positions.get())
        && indices == reinterpret_cast<const unsigned char*>(result.snapshot.data()->indices.get()),
        "actual mesh interface does not lease the exact owned immutable buffers");
    mesh.unLockReadOnlyVertexBase(0);
    unsigned char *mutableVertices = nullptr, *mutableIndices = nullptr;
    bool mutableLockRefused = false;
    try
    {
        candidate.representation->shape().getMeshInterface()->getLockedVertexIndexBase(&mutableVertices,
            vertexCount, vertexType, vertexStride, &mutableIndices, indexStride, triangleCount, indexType);
    }
    catch (Compact::Refusal refusal) { mutableLockRefused = refusal == Compact::Refusal::Unsupported; }
    require(mutableLockRefused, "immutable collision interface granted a mutable stream lock");
    Control control(input);
    const unsigned char *controlVertices = nullptr, *controlIndices = nullptr;
    int controlVertexCount = 0, controlVertexStride = 0, controlTriangles = 0, controlIndexStride = 0;
    PHY_ScalarType controlVertexType = PHY_FLOAT, controlIndexType = PHY_SHORT;
    control.mesh->getLockedReadOnlyVertexIndexBase(&controlVertices, controlVertexCount, controlVertexType,
        controlVertexStride, &controlIndices, controlIndexStride, controlTriangles, controlIndexType);
    require(controlVertexType == PHY_DOUBLE && controlVertexStride == 32 && controlIndexType == PHY_INTEGER
        && controlIndexStride == 12, "ordinary float64 btTriangleMesh control layout was not preserved");
    control.mesh->unLockReadOnlyVertexBase(0);
    for (int placement = 0; placement < 3; ++placement)
    {
        btTransform transform = btTransform::getIdentity();
        if (placement) transform.setOrigin(btVector3(-14, 2, 8));
        if (placement == 2) transform.setRotation(btQuaternion(btVector3(0,0,1), btScalar(0.4)));
        compare(*control.shape, candidate.representation->shape(), transform, btVector3(1,1,1));
        compare(*control.shape, candidate.representation->shape(), transform, btVector3(2,0.5,1.5));
    }

    auto mutableInput = input;
    auto independentBudget = std::make_shared<Compact::StreamBudget>(4096);
    auto privateCopy = Compact::prepare(mutableInput.positions, mutableInput.indices, certificate(), independentBudget);
    auto privateShape = privateCopy.snapshot.instantiate();
    mutableInput.positions[0].set(900,800,700); mutableInput.indices[0] = 7;
    require(privateCopy.snapshot.data()->positions[0] == input.positions[0]
        && privateCopy.snapshot.data()->indices[0] == input.indices[0], "snapshot borrowed mutable caller backing");
    btTransform identity = btTransform::getIdentity();
    compare(*control.shape, privateShape.representation->shape(), identity, btVector3(1,1,1));

    auto alias = result.snapshot;
    auto second = alias.instantiate();
    require(second.representation && &second.representation->streams() == &candidate.representation->streams()
        && budget->stats().total() == streams, "two representations double-charge or copy immutable streams");
    require(!alias.retire(), "live collision consumers allowed premature retirement");
    auto third = alias.instantiate(), fourth = alias.instantiate(), fifth = alias.instantiate();
    require(third.representation && fourth.representation && !fifth.representation
        && fifth.refusal == Compact::Refusal::Bounds, "concurrent BVH owner count exceeded its explicit bound");
    third.representation.reset(); fourth.representation.reset();
    candidate.representation.reset();
    require(!alias.retire(), "one remaining collision consumer was ignored");
    second.representation.reset();
    require(alias.retire() && budget->stats().retiring == streams && budget->stats().current == 0,
        "retirement was credited while immutable backing is still held");
    require(!Compact::prepare(input.positions, input.indices, certificate(), budget).snapshot,
        "retiring debt admitted an overlapping backing without headroom");
    require(alias.instantiate().refusal == Compact::Refusal::Retiring, "retired generation recreated a new shape");
    result.snapshot = {}; require(budget->stats().retiring == streams, "dropping one alias freed shared retirement debt");
    alias = {}; require(budget->stats().total() == 0, "last immutable owner did not release its exact debt");

    for (int unsupported = 0; unsupported < 9; ++unsupported)
    {
        auto cert = certificate();
        if (unsupported == 0) cert.exactStaticTriangleList = false;
        if (unsupported == 1) cert.immutableSnapshot = false;
        if (unsupported == 2) cert.animated = true;
        if (unsupported == 3) cert.skinned = true;
        if (unsupported == 4) cert.morphed = true;
        if (unsupported == 5) cert.controllers = true;
        if (unsupported == 6) cert.customOrUnknownSemantics = true;
        if (unsupported == 7) cert.generation = 0;
        if (unsupported == 8) cert.sourceIdentity = {};
        auto denied = Compact::prepare(input.positions, input.indices, cert, budget);
        require(!denied.snapshot && budget->stats().total() == 0, "unsupported source allocated a compact backing");
        Control fallback(input);
        require(fallback.shape->getOptimizedBvh(), "ordinary unsupported-source fallback lost its actual BVH");
    }
    auto invalid = input;
    invalid.positions[0].x() = std::numeric_limits<float>::quiet_NaN();
    require(Compact::prepare(invalid.positions, invalid.indices, certificate(), budget).refusal == Compact::Refusal::NonFinite,
        "nonfinite coordinates entered actual native construction");
    invalid = input; invalid.indices[0] = 60000;
    require(Compact::prepare(invalid.positions, invalid.indices, certificate(), budget).refusal == Compact::Refusal::Index,
        "out-of-range indices entered native construction");
    invalid = input; invalid.indices.pop_back();
    require(Compact::prepare(invalid.positions, invalid.indices, certificate(), budget).refusal == Compact::Refusal::Bounds,
        "non-triangle topology entered compact storage");
    require(Compact::prepare(input.positions, input.indices, certificate(), budget, {7,12}).refusal == Compact::Refusal::Bounds,
        "vertex count exceeded its source bound");
    require(Compact::prepare(input.positions, input.indices, certificate(), budget, {8,11}).refusal == Compact::Refusal::Bounds,
        "triangle count exceeded its source bound");

    std::array<Compact::Result, 2> competing;
    std::thread first([&] { competing[0] = Compact::prepare(input.positions, input.indices, certificate(), budget); });
    std::thread other([&] { competing[1] = Compact::prepare(input.positions, input.indices, certificate(), budget); });
    first.join(); other.join();
    require(bool(competing[0].snapshot) != bool(competing[1].snapshot) && budget->stats().pending == streams,
        "concurrent shared budget admitted two unique pending backings");
    competing = {}; require(budget->stats().total() == 0, "concurrent admission leaked pending debt");
    Compact::TestHooks::afterReservation = &failAllocation;
    auto failedCopy = Compact::prepare(input.positions, input.indices, certificate(), budget);
    Compact::TestHooks::afterReservation = nullptr;
    require(failedCopy.refusal == Compact::Refusal::Allocation && budget->stats().total() == 0,
        "source-copy failure leaked reserved stream debt");
    auto failureSource = Compact::prepare(input.positions, input.indices, certificate(), budget);
    Compact::TestHooks::beforeBvh = &failAllocation;
    const auto failedShape = failureSource.snapshot.instantiate();
    Compact::TestHooks::beforeBvh = nullptr;
    require(failedShape.refusal == Compact::Refusal::Construction && budget->stats().pending == streams,
        "native construction failure leaked an active consumer or lost source debt");
    require(failureSource.snapshot.retire(), "failed native construction left a phantom consumer");
    failureSource.snapshot = {}; require(budget->stats().total() == 0, "failed source owner leaked retirement debt");

    // PHY_SHORT must preserve unsigned16 values above32767 in actual Bullet.
    Input high;
    high.positions.resize(65536);
    high.positions[65533].set(-1,0,-1); high.positions[65534].set(1,0,-1); high.positions[65535].set(0,0,1);
    high.indices = {65533,65535,65534};
    auto highBudget = std::make_shared<Compact::StreamBudget>(1024 * 1024);
    auto highSnapshot = Compact::prepare(high.positions, high.indices, certificate(), highBudget);
    auto highShape = highSnapshot.snapshot.instantiate();
    require(bool(highShape.representation), "maximum uint16 topology refused supported native construction");
    Control highControl(high);
    compare(*highControl.shape, highShape.representation->shape(), identity, btVector3(1,1,1));
    require(rayHits >= 12 && contactPairsWithPoints >= 2,
        "oracle harness did not engage actual positive rays and narrowphase contacts");

    const std::uint64_t ordinaryNominal = input.positions.size() * controlVertexStride
        + (input.indices.size() / 3) * controlIndexStride;
    std::cout << "PASS: compact static collision native checks=" << checks << " ray_oracles=" << rayOracles
        << " contact_oracles=" << contactOracles << " bounds_oracles=" << boundsOracles
        << " positive_rays=" << rayHits << " positive_contact_pairs=" << contactPairsWithPoints
        << " ordinary_stream_bytes=" << ordinaryNominal << " compact_stream_bytes=" << streams
        << " stream_scope=owned_payload_only_bvh_heap_unmeasured\n";
    return 0;
}
catch (const std::exception& error)
{ std::cerr << "Compact collision fixture failed: " << error.what() << '\n'; return 1; }
catch (...)
{ std::cerr << "Compact collision fixture failed with an unexpected native exception\n"; return 1; }
