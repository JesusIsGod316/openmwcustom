#ifndef OPTIMIZEDMW_COMPACT_STATIC_COLLISION_PROTOTYPE_HPP
#define OPTIMIZEDMW_COMPACT_STATIC_COLLISION_PROTOTYPE_HPP

#include <osg/Vec3f>
#include <BulletCollision/CollisionShapes/btBvhTriangleMeshShape.h>
#include <BulletCollision/CollisionShapes/btTriangleIndexVertexArray.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <utility>

#ifndef BT_USE_DOUBLE_PRECISION
#error This prototype must use the existing qualified float64 Bullet ABI
#endif

namespace Resource::CompactStaticCollisionPrototype
{
    // Native prototype ONLY. No NIF/engine publisher uses this representation.
    // A caller must certify source semantics, not derive immutability from the
    // absence of an animation callback or from a filename alone.
    struct Certificate
    {
        std::uint64_t generation = 0;
        std::array<std::uint8_t, 32> sourceIdentity{};
        bool exactStaticTriangleList = false;
        bool immutableSnapshot = false;
        bool animated = false, skinned = false, morphed = false, controllers = false;
        bool customOrUnknownSemantics = false;
    };
    enum class Refusal { None, Unsupported, Generation, Empty, Bounds, NonFinite, Index,
        Budget, Allocation, Construction, Retiring };
    enum class Phase : unsigned { Pending, Current, Retiring };
#ifdef OPENMW_COMPACT_COLLISION_TEST_HOOKS
    struct TestHooks
    {
        inline static void (*afterReservation)() = nullptr;
        inline static void (*beforeBvh)() = nullptr;
    };
#endif
    struct Limits
    {
        std::size_t vertices = 65536, triangles = 65536;
    };

    // One shared budget across every snapshot using this owner. Charges count
    // each unique immutable vertex/index backing once, in all lifetime phases.
    // Heap overhead and Bullet BVH/temporary native allocations are NOT covered.
    // Count bounds constrain topology; they are not a total-process RAM cap.
    class StreamBudget : public std::enable_shared_from_this<StreamBudget>
    {
    public:
        struct Stats
        {
            std::uint64_t limit = 0, pending = 0, current = 0, retiring = 0, peak = 0;
            std::uint64_t total() const noexcept { return pending + current + retiring; }
        };
        explicit StreamBudget(std::uint64_t limit) : mStats{ limit } {}
        class Charge
        {
        public:
            ~Charge()
            {
                if (!mArmed) return;
                const std::lock_guard lock(mOwner->mMutex);
                mOwner->value(mPhase) -= mBytes;
            }
            void beginConstruction()
            {
                const std::lock_guard lock(mOwner->mMutex);
                if (mPhase == Phase::Retiring) throw Refusal::Retiring;
                if (mConsumers + mConstructing >= 4) throw Refusal::Bounds;
                ++mConstructing;
            }
            void completeConstruction()
            {
                const std::lock_guard lock(mOwner->mMutex);
                --mConstructing; ++mConsumers;
                if (mPhase != Phase::Current)
                {
                    mOwner->value(mPhase) -= mBytes;
                    mPhase = Phase::Current;
                    mOwner->value(mPhase) += mBytes;
                }
            }
            void leaveCurrent() noexcept
            {
                const std::lock_guard lock(mOwner->mMutex);
                --mConsumers;
                if (!mConsumers && mPhase == Phase::Current)
                {
                    mOwner->value(mPhase) -= mBytes;
                    mPhase = Phase::Pending;
                    mOwner->value(mPhase) += mBytes;
                }
            }
            void abortConstruction() noexcept
            { const std::lock_guard lock(mOwner->mMutex); --mConstructing; }
            bool retire()
            {
                const std::lock_guard lock(mOwner->mMutex);
                if (mConsumers || mConstructing) return false;
                if (mPhase != Phase::Retiring)
                {
                    mOwner->value(mPhase) -= mBytes;
                    mPhase = Phase::Retiring;
                    mOwner->value(mPhase) += mBytes;
                }
                return true;
            }
            Phase phase() const
            { const std::lock_guard lock(mOwner->mMutex); return mPhase; }
        private:
            friend class StreamBudget;
            Charge(std::shared_ptr<StreamBudget> owner, std::uint64_t bytes)
                : mOwner(std::move(owner)), mBytes(bytes) {}
            std::shared_ptr<StreamBudget> mOwner;
            std::uint64_t mBytes, mConsumers = 0, mConstructing = 0;
            bool mArmed = false;
            Phase mPhase = Phase::Pending;
        };
        std::shared_ptr<Charge> reserve(std::uint64_t bytes)
        {
            if (!bytes) return {};
            // Metadata/control-block allocation must happen before locking:
            // shared_ptr construction failure deletes its raw Charge. That
            // unarmed destructor neither subtracts debt nor relocks this mutex.
            auto charge = std::shared_ptr<Charge>(new Charge(shared_from_this(), bytes));
            const std::lock_guard lock(mMutex);
            if (mStats.total() > mStats.limit || bytes > mStats.limit - mStats.total()) return {};
            mStats.pending += bytes;
            charge->mArmed = true;
            mStats.peak = std::max(mStats.peak, mStats.total());
            return charge;
        }
        Stats stats() const { const std::lock_guard lock(mMutex); return mStats; }
    private:
        std::uint64_t& value(Phase phase) noexcept
        {
            switch (phase)
            {
                case Phase::Pending: return mStats.pending;
                case Phase::Current: return mStats.current;
                case Phase::Retiring: return mStats.retiring;
            }
            return mStats.pending;
        }
        mutable std::mutex mMutex;
        Stats mStats;
    };

    struct Streams
    {
    private:
        friend class Representation;
        friend class Snapshot;
        // Charge is destroyed LAST: array backing dies before its debt is freed.
        std::shared_ptr<StreamBudget::Charge> charge;
    public:
        const Certificate certificate;
        const std::size_t vertexCount, indexCount;
        const std::unique_ptr<const osg::Vec3f[]> positions;
        const std::unique_ptr<const std::uint16_t[]> indices;
        Streams(Certificate cert, std::span<const osg::Vec3f> vertices, std::span<const std::uint16_t> sourceIndices,
            std::shared_ptr<StreamBudget::Charge> reservation)
            : charge(std::move(reservation)), certificate(cert), vertexCount(vertices.size()), indexCount(sourceIndices.size()),
              positions(copy(vertices)), indices(copy(sourceIndices)) {}
        std::uint64_t knownStreamBytes() const noexcept
        { return vertexCount * sizeof(osg::Vec3f) + indexCount * sizeof(std::uint16_t); }
    private:
        template <class Value>
        static std::unique_ptr<const Value[]> copy(std::span<const Value> input)
        {
            auto output = std::make_unique<Value[]>(input.size());
            std::copy(input.begin(), input.end(), output.get());
            return std::unique_ptr<const Value[]>(std::move(output));
        }
    };

    class MeshInterface final : public btTriangleIndexVertexArray
    {
    public:
        explicit MeshInterface(std::shared_ptr<const Streams> streams) : mStreams(std::move(streams))
        {
            btIndexedMesh mesh;
            mesh.m_numVertices = static_cast<int>(mStreams->vertexCount);
            mesh.m_vertexBase = reinterpret_cast<const unsigned char*>(mStreams->positions.get());
            mesh.m_vertexStride = sizeof(osg::Vec3f);
            mesh.m_vertexType = PHY_FLOAT;
            mesh.m_numTriangles = static_cast<int>(mStreams->indexCount / 3);
            mesh.m_triangleIndexBase = reinterpret_cast<const unsigned char*>(mStreams->indices.get());
            mesh.m_triangleIndexStride = 3 * sizeof(std::uint16_t);
            addIndexedMesh(mesh, PHY_SHORT);
        }
        void getLockedVertexIndexBase(unsigned char**, int&, PHY_ScalarType&, int&, unsigned char**,
            int&, int&, PHY_ScalarType&, int = 0) override
        { throw Refusal::Unsupported; } // This representation never grants mutable stream access.
    private:
        std::shared_ptr<const Streams> mStreams;
    };

    class Representation
    {
        class ConsumerLease
        {
        public:
            explicit ConsumerLease(std::shared_ptr<StreamBudget::Charge> charge) : mCharge(std::move(charge))
            { mCharge->beginConstruction(); }
            ~ConsumerLease()
            { mCompleted ? mCharge->leaveCurrent() : mCharge->abortConstruction(); }
            void complete() { mCharge->completeConstruction(); mCompleted = true; }
        private:
            std::shared_ptr<StreamBudget::Charge> mCharge;
            bool mCompleted = false;
        };
    public:
        explicit Representation(std::shared_ptr<const Streams> streams)
            : mStreams(std::move(streams)), mConsumer(mStreams->charge), mInterface(std::make_unique<MeshInterface>(mStreams)),
              mShape(build(*mInterface))
        { mConsumer.complete(); }
        ~Representation()
        {
            // Destroy BVH before its raw array interface, then drop this active
            // consumer. The immutable data remains alive through both actions.
            mShape.reset(); mInterface.reset();
        }
        Representation(const Representation&) = delete;
        Representation& operator=(const Representation&) = delete;
        btBvhTriangleMeshShape& shape() noexcept { return *mShape; }
        const btBvhTriangleMeshShape& shape() const noexcept { return *mShape; }
        const MeshInterface& interface() const noexcept { return *mInterface; }
        const Streams& streams() const noexcept { return *mStreams; }
    private:
        static std::unique_ptr<btBvhTriangleMeshShape> build(MeshInterface& mesh)
        {
#ifdef OPENMW_COMPACT_COLLISION_TEST_HOOKS
            if (TestHooks::beforeBvh) TestHooks::beforeBvh();
#endif
            return std::make_unique<btBvhTriangleMeshShape>(&mesh, true);
        }
        // Reverse member destruction also preserves BVH -> interface -> data.
        std::shared_ptr<const Streams> mStreams;
        ConsumerLease mConsumer;
        std::unique_ptr<MeshInterface> mInterface;
        std::unique_ptr<btBvhTriangleMeshShape> mShape;
    };
    struct ShapeResult
    {
        std::unique_ptr<Representation> representation;
        Refusal refusal = Refusal::Unsupported;
    };
    class Snapshot
    {
    public:
        Snapshot() = default;
        explicit Snapshot(std::shared_ptr<const Streams> data) : mStreams(std::move(data)) {}
        explicit operator bool() const noexcept { return static_cast<bool>(mStreams); }
        std::uint64_t knownStreamBytes() const noexcept { return mStreams ? mStreams->knownStreamBytes() : 0; }
        ShapeResult instantiate() const noexcept
        {
            if (!mStreams) return {};
            if (mStreams->charge->phase() == Phase::Retiring) return {{}, Refusal::Retiring};
            try { return { std::make_unique<Representation>(mStreams), Refusal::None }; }
            catch (Refusal refusal) { return {{}, refusal}; }
            catch (...) { return {{}, Refusal::Construction}; }
        }
        bool retire() { return mStreams && mStreams->charge->retire(); }
        const Streams* data() const noexcept { return mStreams.get(); }
    private:
        std::shared_ptr<const Streams> mStreams;
    };
    struct Result { Snapshot snapshot; Refusal refusal = Refusal::Unsupported; };
    inline Result prepare(std::span<const osg::Vec3f> positions, std::span<const std::uint16_t> indices,
        const Certificate& certificate, const std::shared_ptr<StreamBudget>& budget, Limits limits = {}) noexcept
    {
        if (!certificate.exactStaticTriangleList || !certificate.immutableSnapshot
            || certificate.animated || certificate.skinned || certificate.morphed || certificate.controllers
            || certificate.customOrUnknownSemantics)
            return {{}, Refusal::Unsupported};
        if (!certificate.generation || std::none_of(certificate.sourceIdentity.begin(),
            certificate.sourceIdentity.end(), [](auto byte) { return byte != 0; }))
            return {{}, Refusal::Generation};
        if (positions.empty() || indices.empty()) return {{}, Refusal::Empty};
        if (indices.size() % 3 || positions.size() > limits.vertices || positions.size() > 65536
            || indices.size() / 3 > limits.triangles || indices.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
            return {{}, Refusal::Bounds};
        for (const auto& position : positions)
            if (!std::isfinite(position.x()) || !std::isfinite(position.y()) || !std::isfinite(position.z()))
                return {{}, Refusal::NonFinite};
        for (auto index : indices) if (index >= positions.size()) return {{}, Refusal::Index};
        if (!budget) return {{}, Refusal::Budget};
        try
        {
            const auto bytes = positions.size() * sizeof(osg::Vec3f) + indices.size() * sizeof(std::uint16_t);
            auto reservation = budget->reserve(bytes);
            if (!reservation) return {{}, Refusal::Budget};
#ifdef OPENMW_COMPACT_COLLISION_TEST_HOOKS
            if (TestHooks::afterReservation) TestHooks::afterReservation();
#endif
            auto data = std::make_shared<Streams>(certificate, positions, indices, std::move(reservation));
            return { Snapshot(std::move(data)), Refusal::None };
        }
        catch (...) { return {{}, Refusal::Allocation}; }
    }
}
#endif
