#define OPENMW_COMPACT_COLLISION_ADAPTER_TEST_HOOKS 1
#include "compact-static-collision-adapter.hpp"

#include <BulletCollision/BroadphaseCollision/btDbvtBroadphase.h>
#include <BulletCollision/CollisionDispatch/btCollisionWorld.h>
#include <BulletCollision/CollisionDispatch/btDefaultCollisionConfiguration.h>
#include <BulletCollision/CollisionShapes/btCompoundShape.h>
#include <BulletCollision/CollisionShapes/btSphereShape.h>
#include <BulletCollision/CollisionShapes/btTriangleMesh.h>
#include <LinearMath/btAlignedAllocator.h>

#include <malloc.h>
#include <array>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace Adapter = Resource::CompactStaticCollisionAdapter;
namespace Compact = Resource::CompactStaticCollisionPrototype;
static_assert(sizeof(btScalar) == 8);
static_assert(sizeof(osg::Vec3f) == 12);

namespace
{
    unsigned checks = 0, boundsOracles = 0, rayOracles = 0, contactOracles = 0;
    unsigned rayHits = 0, contactsWithPoints = 0;
    void require(bool condition, const char* message)
    { if (!condition) throw std::runtime_error(message); ++checks; }
    bool near(btScalar a, btScalar b) { return btFabs(a - b) <= btScalar(1e-9); }
    bool near(const btVector3& a, const btVector3& b)
    { return near(a.x(), b.x()) && near(a.y(), b.y()) && near(a.z(), b.z()); }
    void failAllocation() { throw std::bad_alloc(); }
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
    Adapter::Certificate certificate()
    {
        Adapter::Certificate result;
        result.generation = 17; result.sourceIdentity[0] = 0x72; result.geometryRecord = 31;
        result.exactStaticTriangleList = result.immutableSnapshot = true;
        return result; // Closed native source, not a production content digest receipt.
    }
    std::uint64_t bytes(const Input& input)
    { return input.positions.size() * sizeof(osg::Vec3f) + input.indices.size() * sizeof(std::uint16_t); }
    Resource::CollisionShapePtr ordinary(const Input& input)
    {
        auto mesh = std::make_unique<btTriangleMesh>();
        mesh->preallocateVertices(static_cast<int>(input.positions.size()));
        for (const auto& v : input.positions) mesh->findOrAddVertex(btVector3(v.x(), v.y(), v.z()), false);
        mesh->preallocateIndices(static_cast<int>(input.indices.size()));
        for (std::size_t i = 0; i < input.indices.size(); i += 3)
            mesh->addTriangleIndices(input.indices[i], input.indices[i + 1], input.indices[i + 2]);
        auto shape = std::make_unique<Resource::TriangleMeshShape>(mesh.get(), true);
        mesh.release();
        return Resource::CollisionShapePtr(shape.release());
    }

    // Real Bullet allocation witness. Bounded bookkeeping does not allocate,
    // call Bullet, run assertions, print, or invoke user observers in callbacks.
    struct AllocationSlot
    {
        const void* pointer = nullptr; bool live = false; unsigned frees = 0;
        bool ownerObservationActiveAtFree = false, streamsAliveAtFree = false;
        unsigned ownerCleanupCountAtFree = 0;
    };
    void recordFreeContext(AllocationSlot& slot) noexcept;
    std::array<AllocationSlot, 8192> allocations{};
    unsigned allocationCount = 0;
    bool allocatorOverflow = false, unobservedFree = false, invalidAlignment = false;
    void* trackedAllocate(std::size_t size, int alignment)
    {
        if (alignment <= 0 || (alignment & (alignment - 1)))
        { invalidAlignment = true; alignment = 16; }
        if (!size) size = 1;
        void* pointer = _aligned_malloc(size, static_cast<std::size_t>(alignment));
        if (!pointer) return nullptr;
        for (unsigned i = 0; i < allocationCount; ++i)
            if (allocations[i].pointer == pointer)
            { allocations[i] = {pointer,true,0}; return pointer; }
        if (allocationCount == allocations.size()) allocatorOverflow = true;
        else allocations[allocationCount++] = {pointer, true, 0};
        return pointer;
    }
    void trackedFree(void* pointer)
    {
        if (!pointer) return;
        bool found = false;
        for (unsigned i = 0; i < allocationCount; ++i)
            if (allocations[i].pointer == pointer)
            {
                found = true; allocations[i].live = false; ++allocations[i].frees;
                recordFreeContext(allocations[i]); break;
            }
        if (!found) unobservedFree = true;
        _aligned_free(pointer);
    }
    bool wasFreedBeforeOwner(const void* pointer, unsigned previousOwners) noexcept
    {
        for (unsigned i = 0; i < allocationCount; ++i)
            if (allocations[i].pointer == pointer)
                return !allocations[i].live && allocations[i].frees == 1
                    && allocations[i].ownerObservationActiveAtFree && allocations[i].streamsAliveAtFree
                    && allocations[i].ownerCleanupCountAtFree == previousOwners;
        return false;
    }
    bool isTrackedLive(const void* pointer) noexcept
    {
        for (unsigned i = 0; i < allocationCount; ++i)
            if (allocations[i].pointer == pointer) return allocations[i].live && allocations[i].frees == 0;
        return false;
    }
    struct AllocatorGuard
    {
        AllocatorGuard() { btAlignedAllocSetCustomAligned(&trackedAllocate, &trackedFree); }
        ~AllocatorGuard() { btAlignedAllocSetCustomAligned(nullptr, nullptr); }
    };
    struct RetirementObservation
    {
        unsigned ownerBegins = 0, completedBvhs = 0, exactBvhFrees = 0;
        unsigned interfacesAlive = 0, afterInterfaces = 0, afterOwners = 0;
        bool bufferReadValid = true;
        bool finalWeakExpired = false;
        std::uint64_t finalBudgetTotal = UINT64_MAX;
    } observation;
    std::weak_ptr<const Compact::Streams> observedStreams;
    std::shared_ptr<Adapter::StreamBudget> observedBudget;
    void recordFreeContext(AllocationSlot& slot) noexcept
    {
        slot.ownerObservationActiveAtFree=Adapter::TestHooks::beforeInterfaceDelete!=nullptr;
        slot.streamsAliveAtFree=!observedStreams.expired();
        slot.ownerCleanupCountAtFree=observation.ownerBegins;
    }
    void beforeInterfaceDelete(const btOptimizedBvh* bvh, const Compact::MeshInterface* interface,
        const Compact::Streams* streams) noexcept
    {
        if (bvh)
        {
            ++observation.completedBvhs;
            if (wasFreedBeforeOwner(bvh,observation.ownerBegins)) ++observation.exactBvhFrees;
        }
        ++observation.ownerBegins;
        if (interface && streams) ++observation.interfacesAlive;
        observation.bufferReadValid &= streams && streams->positions && streams->indices
            && streams->vertexCount && streams->indexCount && streams->certificate.generation == 17;
    }
    void afterInterfaceDelete(const Compact::Streams* streams) noexcept
    {
        ++observation.afterInterfaces;
        observation.bufferReadValid &= streams && streams->positions && streams->indices;
    }
    void afterBackingRelease() noexcept
    {
        ++observation.afterOwners;
        observation.finalWeakExpired = observedStreams.expired();
        if (observedBudget) observation.finalBudgetTotal = observedBudget->stats().total();
    }
    void startObservation(const Adapter::Snapshot& snapshot, std::shared_ptr<Adapter::StreamBudget> budget)
    {
        observation = {}; observedStreams = snapshot.weakData(); observedBudget = std::move(budget);
        Adapter::TestHooks::beforeInterfaceDelete = &beforeInterfaceDelete;
        Adapter::TestHooks::afterInterfaceDelete = &afterInterfaceDelete;
        Adapter::TestHooks::afterBackingRelease = &afterBackingRelease;
    }
    void stopObservation()
    {
        Adapter::TestHooks::beforeInterfaceDelete = nullptr;
        Adapter::TestHooks::afterInterfaceDelete = nullptr;
        Adapter::TestHooks::afterBackingRelease = nullptr;
        observedStreams.reset(); observedBudget.reset();
    }
    void checkObservedDestruction(unsigned expected, bool lastBacking)
    {
        require(observation.ownerBegins == expected && observation.completedBvhs == expected
            && observation.exactBvhFrees == expected,
            "real native BVH aligned free did not precede immutable owner destruction");
        require(observation.interfacesAlive == expected && observation.afterInterfaces == expected
            && observation.afterOwners == expected && observation.bufferReadValid,
            "interface/buffers were not alive through actual BVH destruction");
        if (lastBacking)
            require(observation.finalWeakExpired && observation.finalBudgetTotal == 0,
                "final backing did not retire after interface destruction, or debt outlived payload");
    }

    struct Contacts final : btCollisionWorld::ContactResultCallback
    {
        unsigned count = 0; btScalar deepest = BT_LARGE_FLOAT;
        btScalar addSingleResult(btManifoldPoint& point, const btCollisionObjectWrapper*, int, int,
            const btCollisionObjectWrapper*, int, int) override
        { ++count; deepest = btMin(deepest, point.getDistance()); return 0; }
    };
    struct World
    {
        btDefaultCollisionConfiguration configuration;
        btCollisionDispatcher dispatcher{&configuration};
        btDbvtBroadphase broadphase;
        btCollisionWorld world{&dispatcher, &broadphase, &configuration};
        btCollisionObject object;
        World(btCollisionShape& shape, const btTransform& transform)
        {
            object.setCollisionShape(&shape); object.setWorldTransform(transform); world.addCollisionObject(&object);
        }
        ~World() { world.removeCollisionObject(&object); }
    };
    void compare(btCollisionShape& control, btCollisionShape& candidate, const btTransform& transform)
    {
        btVector3 aMin,aMax,bMin,bMax;
        control.getAabb(transform,aMin,aMax); candidate.getAabb(transform,bMin,bMax);
        require(near(aMin,bMin) && near(aMax,bMax), "production adapter/wrapper transformed bounds differ");
        ++boundsOracles;
        World reference(control,transform), test(candidate,transform);
        for (int axis = 0; axis < 3; ++axis)
            for (btScalar offset : {btScalar(-0.7),btScalar(0),btScalar(0.7),btScalar(4)})
            {
                btVector3 from(0,0,0),to(0,0,0); from[axis] = -5; to[axis] = 5;
                from[(axis+1)%3] = to[(axis+1)%3] = offset;
                from = transform*from; to = transform*to;
                btCollisionWorld::ClosestRayResultCallback a(from,to),b(from,to);
                reference.world.rayTest(from,to,a); test.world.rayTest(from,to,b);
                require(a.hasHit() == b.hasHit(), "production wrapper ray hit/miss differs");
                if (a.hasHit())
                {
                    ++rayHits;
                    require(near(a.m_closestHitFraction,b.m_closestHitFraction)
                        && near(a.m_hitPointWorld,b.m_hitPointWorld) && near(a.m_hitNormalWorld,b.m_hitNormalWorld),
                        "production wrapper ray position/normal/fraction differs");
                }
                ++rayOracles;
            }
        btSphereShape sphere(btScalar(0.35));
        for (btScalar x : {btScalar(0),btScalar(0.85),btScalar(1.2),btScalar(4)})
        {
            btCollisionObject probe; probe.setCollisionShape(&sphere);
            btTransform at = transform; at.setOrigin(transform*btVector3(x,0,0)); probe.setWorldTransform(at);
            Contacts a,b; reference.world.contactPairTest(&reference.object,&probe,a);
            test.world.contactPairTest(&test.object,&probe,b);
            require(a.count == b.count, "production wrapper narrowphase contact count differs");
            if (a.count) { ++contactsWithPoints; require(near(a.deepest,b.deepest), "production wrapper contact depth differs"); }
            ++contactOracles;
        }
    }
    Resource::CollisionShapePtr scaled(Resource::CollisionShapePtr child, const btVector3& scale)
    {
        auto* mesh = dynamic_cast<btBvhTriangleMeshShape*>(child.get());
        require(mesh != nullptr, "test child is not actual BVH");
        auto wrapper = std::make_unique<Resource::ScaledTriangleMeshShape>(mesh,scale);
        child.release();
        return Resource::CollisionShapePtr(wrapper.release());
    }
    Resource::CollisionShapePtr compound(Resource::CollisionShapePtr child, const btTransform& local)
    {
        auto result = std::make_unique<btCompoundShape>();
        result->addChildShape(local,child.get()); child.release();
        return Resource::CollisionShapePtr(result.release());
    }
    Adapter::Snapshot prepared(const Input& input, const std::shared_ptr<Adapter::StreamBudget>& budget)
    {
        auto result = Adapter::prepare(input.positions,input.indices,certificate(),budget);
        require(result.refusal == Adapter::Refusal::None && bool(result.snapshot), "adapter source preparation failed");
        return std::move(result.snapshot);
    }

    void directAndWrapped(const Input& input)
    {
        const auto charge = bytes(input);
        auto budget = std::make_shared<Adapter::StreamBudget>(charge);
        auto snapshot = prepared(input,budget);
        auto result = snapshot.instantiate(certificate());
        require(result.refusal == Adapter::Refusal::None && result.shape, "adapter real polymorphic BVH construction failed");
        auto* shape = dynamic_cast<Adapter::Shape*>(result.shape.get());
        require(shape && shape->getShapeType() == TRIANGLE_MESH_SHAPE_PROXYTYPE && shape->getOwnsBvh()
            && shape->getOptimizedBvh() && isTrackedLive(shape->getOptimizedBvh()),
            "adapter does not preserve actual BVH shape interface and ownership");
        const unsigned char *v=nullptr,*i=nullptr; int vc=0,vs=0,tc=0,is=0;
        PHY_ScalarType vt=PHY_DOUBLE,it=PHY_INTEGER;
        shape->getMeshInterface()->getLockedReadOnlyVertexIndexBase(&v,vc,vt,vs,&i,is,tc,it);
        require(vt==PHY_FLOAT && vs==12 && it==PHY_SHORT && is==6 && vc==8 && tc==12,
            "real production-compatible shape lost compact immutable strides");
        require(v==reinterpret_cast<const unsigned char*>(snapshot.data()->positions.get())
            && i==reinterpret_cast<const unsigned char*>(snapshot.data()->indices.get()),
            "production-compatible BVH uses another buffer allocation");
        shape->getMeshInterface()->unLockReadOnlyVertexBase(0);
        bool mutableRefused=false; unsigned char *mv=nullptr,*mi=nullptr;
        try { shape->getMeshInterface()->getLockedVertexIndexBase(&mv,vc,vt,vs,&mi,is,tc,it); }
        catch (Adapter::Refusal refusal) { mutableRefused=refusal==Adapter::Refusal::Unsupported; }
        require(mutableRefused,"actual adapter grants mutable vertex/index access");
        auto control=ordinary(input);
        const btTransform identity=btTransform::getIdentity();
        compare(*control,*result.shape,identity);
        btTransform rotated(btQuaternion(btVector3(0,0,1),btScalar(0.41)),btVector3(3,-2,4));
        compare(*control,*result.shape,rotated);
        startObservation(snapshot,budget); snapshot={};
        result.shape.reset(); // Actual DeleteCollisionShape, through adjusted btCollisionShape pointer.
        checkObservedDestruction(1,true); stopObservation();

        snapshot=prepared(input,budget); result=snapshot.instantiate(certificate());
        startObservation(snapshot,budget);
        auto wrapped=scaled(std::move(result.shape),btVector3(1.25,0.75,1.5));
        auto ordinaryWrapped=scaled(ordinary(input),btVector3(1.25,0.75,1.5));
        compare(*ordinaryWrapped,*wrapped,identity); compare(*ordinaryWrapped,*wrapped,rotated);
        snapshot={}; wrapped.reset(); // Actual owning ScaledTriangleMeshShape destructor.
        checkObservedDestruction(1,true); stopObservation();

        snapshot=prepared(input,budget); result=snapshot.instantiate(certificate());
        startObservation(snapshot,budget);
        btTransform local(btQuaternion(btVector3(0,1,0),btScalar(0.21)),btVector3(0.2,0,0));
        auto nested=compound(compound(scaled(std::move(result.shape),btVector3(1,0.8,1.1)),local),identity);
        auto ordinaryNested=compound(compound(scaled(ordinary(input),btVector3(1,0.8,1.1)),local),identity);
        compare(*ordinaryNested,*nested,identity); compare(*ordinaryNested,*nested,rotated);
        snapshot={}; nested.reset(); // Actual recursive DeleteCollisionShape visits both compound levels.
        checkObservedDestruction(1,true); stopObservation();
        require(budget->stats().peak==charge,"wrapping created a second charged stream backing");
    }

    void sharedInstances(const Input& input)
    {
        const auto charge=bytes(input);
        auto budget=std::make_shared<Adapter::StreamBudget>(2*charge);
        auto body=prepared(input,budget),avoid=prepared(input,budget);
        auto child=body.instantiate(certificate()),avoidChild=avoid.instantiate(certificate());
        require(child.shape && avoidChild.shape,"actual source/avoid shapes missing");
        osg::ref_ptr<Resource::BulletShape> source=new Resource::BulletShape;
        btTransform local=btTransform::getIdentity(); local.setOrigin(btVector3(0.1,0,0));
        source->mCollisionShape=compound(scaled(std::move(child.shape),btVector3(1,1.2,0.9)),local);
        source->mAvoidCollisionShape=compound(std::move(avoidChild.shape),local);
        auto reference=osg::ref_ptr<Resource::BulletShape>(new Resource::BulletShape);
        reference->mCollisionShape=compound(scaled(ordinary(input),btVector3(1,1.2,0.9)),local);
        reference->mAvoidCollisionShape=compound(ordinary(input),local);
        auto instance=Resource::makeInstance(source),second=Resource::makeInstance(source);
        auto referenceInstance=Resource::makeInstance(reference);
        require(instance->getSource().get()==source.get() && second->getSource().get()==source.get(),
            "real BulletShapeInstance does not hold the exact immutable source");
        require(budget->stats().current==2*charge && budget->stats().total()==2*charge,
            "actual instance wrappers duplicate or release unique backing charges");
        require(!body.retire() && !avoid.retire(),"live source compound/avoid consumers can retire");
        auto* instanceCompound=dynamic_cast<btCompoundShape*>(instance->mCollisionShape.get());
        auto* clonedWrapper=instanceCompound ? dynamic_cast<btScaledBvhTriangleMeshShape*>(instanceCompound->getChildShape(0)) : nullptr;
        auto* sourceCompound=dynamic_cast<btCompoundShape*>(source->mCollisionShape.get());
        auto* owningWrapper=sourceCompound ? dynamic_cast<Resource::ScaledTriangleMeshShape*>(sourceCompound->getChildShape(0)) : nullptr;
        require(clonedWrapper && owningWrapper && clonedWrapper->getChildShape()==owningWrapper->getChildShape(),
            "actual instance duplication does not borrow the exact source BVH");
        auto bodyWeak=body.weakData(),avoidWeak=avoid.weakData();
        startObservation(body,budget); body={}; avoid={}; source=nullptr;
        require(!bodyWeak.expired() && !avoidWeak.expired() && observation.ownerBegins==0,
            "dropping cache/source alias retired streams still owned by real instances");
        const btVector3 scaling(0.8,1.1,1.25);
        instance->setLocalScaling(scaling); referenceInstance->setLocalScaling(scaling);
        compare(*referenceInstance->mCollisionShape,*instance->mCollisionShape,btTransform::getIdentity());
        compare(*referenceInstance->mAvoidCollisionShape,*instance->mAvoidCollisionShape,btTransform::getIdentity());
        instance=nullptr;
        require(!bodyWeak.expired() && !avoidWeak.expired() && budget->stats().current==2*charge,
            "first actual instance release retired shared source streams");
        second=nullptr;
        checkObservedDestruction(2,true);
        require(bodyWeak.expired() && avoidWeak.expired() && budget->stats().total()==0,
            "last actual instance/source/avoid release retained backing or prematurely credited it");
        stopObservation();
    }

    void directInstancesBothRetirementOrders(const Input& input)
    {
        for (bool sourceFirst : {false,true})
        {
            const auto charge=bytes(input);
            auto budget=std::make_shared<Adapter::StreamBudget>(charge);
            auto snapshot=prepared(input,budget);
            auto child=snapshot.instantiate(certificate());
            auto* root=dynamic_cast<Adapter::Shape*>(child.shape.get());
            require(root && root->getOwnsBvh() && isTrackedLive(root->getOptimizedBvh()),
                "direct instance source does not own the tracked native BVH");
            osg::ref_ptr<Resource::BulletShape> source=new Resource::BulletShape;
            source->mCollisionShape=std::move(child.shape);
            auto reference=osg::ref_ptr<Resource::BulletShape>(new Resource::BulletShape);
            reference->mCollisionShape=ordinary(input);
            auto instance=Resource::makeInstance(source),second=Resource::makeInstance(source);
            auto referenceInstance=Resource::makeInstance(reference);
            auto* cloned=dynamic_cast<btScaledBvhTriangleMeshShape*>(instance->mCollisionShape.get());
            require(cloned && cloned->getChildShape()==root
                && dynamic_cast<Resource::ScaledTriangleMeshShape*>(cloned)==nullptr,
                "direct source clone is not the ordinary nonowning scaled wrapper");
            auto weak=snapshot.weakData();
            startObservation(snapshot,budget); snapshot={};
            if (sourceFirst) source=nullptr;
            compare(*referenceInstance->mCollisionShape,*instance->mCollisionShape,btTransform::getIdentity());
            instance=nullptr;
            require(!weak.expired() && observation.ownerBegins==0 && budget->stats().current==charge,
                "first direct instance release retired still-owned immutable source");
            second=nullptr;
            if (!sourceFirst)
            {
                require(!weak.expired() && observation.ownerBegins==0 && budget->stats().current==charge,
                    "instances released a BVH still owned by an external source");
                source=nullptr;
            }
            checkObservedDestruction(1,true); stopObservation();
        }
    }

    void constructionCountBound(const Input& input)
    {
        const auto charge=bytes(input);
        auto budget=std::make_shared<Adapter::StreamBudget>(charge);
        auto snapshot=prepared(input,budget);
        std::array<Resource::CollisionShapePtr,4> shapes;
        for(auto& shape:shapes)
        {
            auto candidate=snapshot.instantiate(certificate());
            require(candidate.shape && candidate.refusal==Adapter::Refusal::None,
                "bounded immutable source refused an available real BVH slot");
            shape=std::move(candidate.shape);
        }
        auto refused=snapshot.instantiate(certificate());
        require(!refused.shape && refused.refusal==Adapter::Refusal::Bounds,
            "fifth actual BVH representation exceeded the shared source construction bound");
        require(budget->stats().current==charge && budget->stats().total()==charge,
            "multiple real BVHs doublecharged unique stream backing");
        startObservation(snapshot,budget); snapshot={};
        for(auto& shape:shapes) shape.reset();
        checkObservedDestruction(4,true); stopObservation();
    }

    void guardedSourceAndFallback(const Input& input)
    {
        const auto charge=bytes(input);
        auto budget=std::make_shared<Adapter::StreamBudget>(charge);
        auto snapshot=prepared(input,budget);
        auto changed=certificate(); ++changed.generation;
        require(snapshot.instantiate(changed).refusal==Adapter::Refusal::Generation,"stale source generation admitted");
        changed=certificate(); changed.sourceIdentity[5]=0x12;
        require(snapshot.instantiate(changed).refusal==Adapter::Refusal::Generation,"changed exact source hash admitted");
        changed=certificate(); ++changed.geometryRecord;
        require(snapshot.instantiate(changed).refusal==Adapter::Refusal::Generation,"different geometry record in same file admitted");
        changed=certificate(); changed.geometryRecord=UINT32_MAX;
        require(snapshot.instantiate(changed).refusal==Adapter::Refusal::Generation,"unknown current geometry record admitted");
        changed=certificate(); changed.controllers=true;
        require(snapshot.instantiate(changed).refusal==Adapter::Refusal::Unsupported,"mutable/controller source admitted after preparation");
        changed=certificate(); changed.skinned=true;
        require(snapshot.instantiate(changed).refusal==Adapter::Refusal::Unsupported,"skinned source admitted after preparation");
        changed=certificate(); changed.generation=0;
        require(snapshot.instantiate(changed).refusal==Adapter::Refusal::Generation,"unknown current source admitted");
        require(budget->stats().pending==charge && budget->stats().current==0,"guard refusal changes pending backing debt");
        snapshot={};

        unsigned ordinaryCalls=0;
        auto fallback=[&] {
            require(budget->stats().total()==0,"failed candidate retained payload while ordinary fallback allocates");
            ++ordinaryCalls; return ordinary(input);
        };
        auto control=Adapter::select(false,input.positions,input.indices,certificate(),budget,fallback);
        require(!control.compactSelected && ordinaryCalls==1 && budget->stats().total()==0,"default-off path is not ordinary control");
        control.shape.reset();
        std::array<Adapter::Certificate,7> unsupported{};
        unsupported.fill(certificate());
        unsupported[0].animated=true; unsupported[1].skinned=true; unsupported[2].morphed=true;
        unsupported[3].controllers=true; unsupported[4].customOrUnknownSemantics=true;
        unsupported[5].immutableSnapshot=false; unsupported[6].exactStaticTriangleList=false;
        for (const auto& cert:unsupported)
        {
            auto result=Adapter::select(true,input.positions,input.indices,cert,budget,fallback);
            require(!result.compactSelected && result.shape && result.refusal==Adapter::Refusal::Unsupported,
                "unsupported resolved source did not preserve actual ordinary BVH fallback");
        }
        auto deniedBudget=std::make_shared<Adapter::StreamBudget>(charge-1);
        auto denied=Adapter::select(true,input.positions,input.indices,certificate(),deniedBudget,fallback);
        require(!denied.compactSelected && denied.shape && denied.refusal==Adapter::Refusal::Budget,
            "shared backing admission failure did not preserve ordinary fallback");
        Adapter::TestHooks::afterReservation=&failAllocation;
        auto failedCopy=Adapter::select(true,input.positions,input.indices,certificate(),budget,fallback);
        Adapter::TestHooks::afterReservation=nullptr;
        require(!failedCopy.compactSelected && failedCopy.shape && failedCopy.refusal==Adapter::Refusal::Allocation,
            "failed reserved copy did not roll back before fallback");
        Adapter::TestHooks::beforeBvh=&failAllocation;
        auto failedBeforeBvh=Adapter::select(true,input.positions,input.indices,certificate(),budget,fallback);
        Adapter::TestHooks::beforeBvh=nullptr;
        require(!failedBeforeBvh.compactSelected && failedBeforeBvh.shape
            && failedBeforeBvh.refusal==Adapter::Refusal::Construction,"pre-BVH construction failure did not fall back");
        snapshot=prepared(input,budget); startObservation(snapshot,budget);
        Adapter::TestHooks::afterBvhBeforePublication=&failAllocation;
        auto failedAfterBvh=snapshot.instantiate(certificate());
        Adapter::TestHooks::afterBvhBeforePublication=nullptr;
        require(!failedAfterBvh.shape && failedAfterBvh.refusal==Adapter::Refusal::Construction,
            "actual constructed BVH exception was not contained");
        checkObservedDestruction(1,false);
        require(budget->stats().pending==charge && budget->stats().current==0,
            "failed publication leaves an active consumer or loses pending debt");
        snapshot={}; require(budget->stats().total()==0,"failed-publication backing did not retire");
        stopObservation();
        Adapter::TestHooks::afterBvhBeforePublication=&failAllocation;
        auto failedPublished=Adapter::select(true,input.positions,input.indices,certificate(),budget,fallback);
        Adapter::TestHooks::afterBvhBeforePublication=nullptr;
        require(!failedPublished.compactSelected && failedPublished.shape
            && failedPublished.refusal==Adapter::Refusal::Construction,"post-BVH candidate failure did not release before fallback");

        snapshot=prepared(input,budget); auto alias=snapshot;
        auto candidate=snapshot.instantiate(certificate());
        require(candidate.shape && !snapshot.retire(),"live adapter can retire shared backing");
        candidate.shape.reset(); require(snapshot.retire(),"idle adapter backing cannot enter retiring phase");
        require(snapshot.instantiate(certificate()).refusal==Adapter::Refusal::Retiring,"retiring snapshot builds a new adapter");
        snapshot={}; require(budget->stats().retiring==charge,"alias was credited before its backing died");
        alias={}; require(budget->stats().total()==0,"last retiring alias did not retire debt");
        require(ordinaryCalls==12,"ordinary fallback engagement count differs from tested refusal paths");
    }
}

int main() try
{
    // This guard MUST outlive every Bullet allocation made under its callbacks.
    // All shapes/worlds are inside called scopes and destroyed before reset.
    AllocatorGuard allocator;
    const auto input=cube();
    directAndWrapped(input); sharedInstances(input); directInstancesBothRetirementOrders(input);
    constructionCountBound(input); guardedSourceAndFallback(input);
    require(rayHits>0 && contactsWithPoints>0,"actual collision oracle never exercised a positive hit/contact");
    require(!allocatorOverflow && !unobservedFree && !invalidAlignment,
        "native allocation witness overflow, invalid alignment or unmatched Bullet free");
    require(std::none_of(allocations.begin(),allocations.begin()+allocationCount,
        [](const auto& slot){return slot.live;}),"fixture leaves actual native Bullet allocations alive when resetting allocator");
    std::cout << "COMPACT_COLLISION_ADAPTER_NATIVE_PASS checks=" << checks
        << " bounds=" << boundsOracles << " rays=" << rayOracles << " ray_hits=" << rayHits
        << " contacts=" << contactOracles << " contact_pairs_with_points=" << contactsWithPoints
        << " distinct_aligned_addresses=" << allocationCount
        << " source_owner=actual_BulletShapeInstance compound_scaled_avoid=actual_DeleteCollisionShape"
        << " stream_payload_only=1 process_RAM_claim=0 engine_integration=0\n";
    return 0;
}
catch(const std::exception& error)
{
    std::cerr << "COMPACT_COLLISION_ADAPTER_NATIVE_FAIL " << error.what() << '\n'; return 1;
}
