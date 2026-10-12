#ifndef OPTIMIZEDMW_COMPACT_STATIC_COLLISION_ADAPTER_HPP
#define OPTIMIZEDMW_COMPACT_STATIC_COLLISION_ADAPTER_HPP

#include "compact-static-collision-prototype.hpp"
#include <components/resource/bulletshape.hpp>

#include <functional>
#include <type_traits>

namespace Resource::CompactStaticCollisionAdapter
{
    namespace Compact = CompactStaticCollisionPrototype;
    struct Certificate : Compact::Certificate
    {
        // An immutable stream is a particular geometry record, not every mesh
        // in a content file sharing the same file digest and VFS generation.
        std::uint32_t geometryRecord = UINT32_MAX;
    };
    using Refusal = Compact::Refusal;
    using Limits = Compact::Limits;
    using StreamBudget = Compact::StreamBudget;

    // NEW native adapter only: the NIF loader/manager does not select this path.
    // The policy's real caller must certify exact resolved static semantics,
    // generation and content identity. A path or absence of callbacks alone
    // cannot issue that certificate. No parsed file is retained by this owner.
    inline bool eligible(const Certificate& certificate) noexcept
    {
        return certificate.exactStaticTriangleList && certificate.immutableSnapshot
            && !certificate.animated && !certificate.skinned && !certificate.morphed
            && !certificate.controllers && !certificate.customOrUnknownSemantics;
    }
    inline bool identified(const Certificate& certificate) noexcept
    {
        return certificate.generation && certificate.geometryRecord != UINT32_MAX
            && std::any_of(certificate.sourceIdentity.begin(),
            certificate.sourceIdentity.end(), [](auto value) { return value != 0; });
    }
    inline bool matches(const Certificate& stored, const Certificate& current) noexcept
    {
        return eligible(current) && identified(current) && stored.generation == current.generation
            && stored.sourceIdentity == current.sourceIdentity && stored.geometryRecord == current.geometryRecord;
    }

#ifdef OPENMW_COMPACT_COLLISION_ADAPTER_TEST_HOOKS
    struct TestHooks
    {
        inline static void (*afterReservation)() = nullptr;
        inline static void (*beforeBvh)() = nullptr;
        inline static void (*afterBvhBeforePublication)() = nullptr;
        // Non-throwing observers. A fixture may observe actual Bullet frees;
        // these notifications do not themselves simulate a BVH destructor.
        inline static void (*beforeInterfaceDelete)(const btOptimizedBvh*,
            const Compact::MeshInterface*, const Compact::Streams*) noexcept = nullptr;
        inline static void (*afterInterfaceDelete)(const Compact::Streams*) noexcept = nullptr;
        inline static void (*afterBackingRelease)() noexcept = nullptr;
    };
#endif

    struct Backing
    {
        // The stream data owns the same charge, and dies before this extra
        // charge alias. Final debt cannot retire while its actual arrays live.
        std::shared_ptr<StreamBudget::Charge> charge;
        const Certificate certificate;
        std::shared_ptr<const Compact::Streams> streams;
        Backing(std::shared_ptr<StreamBudget::Charge> reservation,
            const Certificate& source, std::shared_ptr<const Compact::Streams> data)
            : charge(std::move(reservation)), certificate(source), streams(std::move(data)) {}
    };

    class ConsumerLease
    {
    public:
        explicit ConsumerLease(std::shared_ptr<StreamBudget::Charge> charge)
            : mCharge(std::move(charge)) { mCharge->beginConstruction(); }
        ~ConsumerLease() { end(); }
        ConsumerLease(const ConsumerLease&) = delete;
        ConsumerLease& operator=(const ConsumerLease&) = delete;
        void complete() { mCharge->completeConstruction(); mCompleted = true; }
        void end() noexcept
        {
            if (!mCharge) return;
            mCompleted ? mCharge->leaveCurrent() : mCharge->abortConstruction();
            mCharge.reset();
        }
    private:
        std::shared_ptr<StreamBudget::Charge> mCharge;
        bool mCompleted = false;
    };

    class ImmutableOwner
    {
    protected:
        explicit ImmutableOwner(std::shared_ptr<const Backing> backing)
            : mBacking(std::move(backing)), mConsumer(mBacking->charge),
              mInterface(std::make_unique<Compact::MeshInterface>(mBacking->streams)),
              mConstructionInterface(mInterface.get())
        {
#ifdef OPENMW_COMPACT_COLLISION_ADAPTER_TEST_HOOKS
            if (TestHooks::beforeBvh) TestHooks::beforeBvh();
#endif
        }
        ~ImmutableOwner()
        {
            // The Bullet base is fully destroyed BEFORE this first base.
            // The pointer value below is only an identity for the fixture's
            // actual aligned-free observation; never dereference the dead BVH.
#ifdef OPENMW_COMPACT_COLLISION_ADAPTER_TEST_HOOKS
            if (TestHooks::beforeInterfaceDelete)
                TestHooks::beforeInterfaceDelete(mBvhIdentity, mInterface.get(), mBacking->streams.get());
#endif
            mInterface.reset();
#ifdef OPENMW_COMPACT_COLLISION_ADAPTER_TEST_HOOKS
            if (TestHooks::afterInterfaceDelete) TestHooks::afterInterfaceDelete(mBacking->streams.get());
#endif
            mConsumer.end();
            mBacking.reset();
#ifdef OPENMW_COMPACT_COLLISION_ADAPTER_TEST_HOOKS
            if (TestHooks::afterBackingRelease) TestHooks::afterBackingRelease();
#endif
        }
        void publish(const btOptimizedBvh* bvh)
        {
            mBvhIdentity = bvh;
#ifdef OPENMW_COMPACT_COLLISION_ADAPTER_TEST_HOOKS
            if (TestHooks::afterBvhBeforePublication) TestHooks::afterBvhBeforePublication();
#endif
            mConsumer.complete();
        }
        const Compact::Streams& ownedStreams() const noexcept { return *mBacking->streams; }
    private:
        // Member destruction order also protects data if construction fails
        // before this base is fully constructed.
        std::shared_ptr<const Backing> mBacking;
        ConsumerLease mConsumer;
        std::unique_ptr<Compact::MeshInterface> mInterface;
        const btOptimizedBvh* mBvhIdentity = nullptr;
    protected:
        // A plain member read during the next base initializer is permitted;
        // calling a Shape/inherited member function before all Shape bases
        // finish would not be. This pointer never owns or outlives mInterface.
        Compact::MeshInterface* const mConstructionInterface;
    };

    // This is a real btBvhTriangleMeshShape usable by the existing polymorphic
    // DeleteCollisionShape and ScaledTriangleMeshShape owners. It owns exactly
    // one BVH, not a second BVH hidden in a composition wrapper. The first base
    // is constructed before the Bullet base and destroyed after it.
    class Shape final : private ImmutableOwner, public btBvhTriangleMeshShape
    {
    public:
        explicit Shape(std::shared_ptr<const Backing> backing)
            : ImmutableOwner(std::move(backing)),
              btBvhTriangleMeshShape(mConstructionInterface, true)
        { publish(getOptimizedBvh()); }
        ~Shape() override
        {
            // Match the existing TriangleMeshShape optional info-map owner;
            // the Bullet BVH base does not own this separately installed map.
            delete getTriangleInfoMap();
            setTriangleInfoMap(nullptr);
        }
        Shape(const Shape&) = delete;
        Shape& operator=(const Shape&) = delete;
        const Compact::Streams& streams() const noexcept { return ownedStreams(); }
    };
    static_assert(std::is_base_of_v<btBvhTriangleMeshShape, Shape>);
    static_assert(std::has_virtual_destructor_v<Shape>);

    struct ShapeResult
    {
        CollisionShapePtr shape;
        Refusal refusal = Refusal::Unsupported;
    };
    class Snapshot
    {
    public:
        Snapshot() = default;
        explicit Snapshot(std::shared_ptr<const Backing> backing) : mBacking(std::move(backing)) {}
        explicit operator bool() const noexcept { return static_cast<bool>(mBacking); }
        const Compact::Streams* data() const noexcept { return mBacking ? mBacking->streams.get() : nullptr; }
        std::weak_ptr<const Compact::Streams> weakData() const noexcept
        { if (!mBacking) return {}; return mBacking->streams; }
        std::uint64_t knownStreamBytes() const noexcept
        { return mBacking ? mBacking->streams->knownStreamBytes() : 0; }
        ShapeResult instantiate(const Certificate& currentSource) const noexcept
        {
            if (!mBacking) return {};
            if (!eligible(currentSource)) return {{}, Refusal::Unsupported};
            if (!matches(mBacking->certificate, currentSource)) return {{}, Refusal::Generation};
            if (mBacking->charge->phase() == Compact::Phase::Retiring) return {{}, Refusal::Retiring};
            try { return { CollisionShapePtr(new Shape(mBacking)), Refusal::None }; }
            catch (Refusal refusal) { return {{}, refusal}; }
            catch (...) { return {{}, Refusal::Construction}; }
        }
        bool retire() { return mBacking && mBacking->charge->retire(); }
    private:
        std::shared_ptr<const Backing> mBacking;
    };
    struct Result { Snapshot snapshot; Refusal refusal = Refusal::Unsupported; };

    inline Result prepare(std::span<const osg::Vec3f> positions, std::span<const std::uint16_t> indices,
        const Certificate& certificate, const std::shared_ptr<StreamBudget>& budget, Limits limits = {}) noexcept
    {
        if (!eligible(certificate)) return {{}, Refusal::Unsupported};
        if (!identified(certificate)) return {{}, Refusal::Generation};
        if (positions.empty() || indices.empty()) return {{}, Refusal::Empty};
        if (indices.size() % 3 || positions.size() > limits.vertices || positions.size() > 65536
            || indices.size() / 3 > limits.triangles
            || indices.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
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
#ifdef OPENMW_COMPACT_COLLISION_ADAPTER_TEST_HOOKS
            if (TestHooks::afterReservation) TestHooks::afterReservation();
#endif
            auto data = std::make_shared<Compact::Streams>(certificate, positions, indices, reservation);
            auto backing = std::make_shared<Backing>(std::move(reservation), certificate, std::move(data));
            return { Snapshot(std::move(backing)), Refusal::None };
        }
        catch (...) { return {{}, Refusal::Allocation}; }
    }

    struct Selection
    {
        CollisionShapePtr shape;
        bool compactSelected = false;
        Refusal refusal = Refusal::Unsupported;
    };
    template<class OrdinaryFactory>
    Selection select(bool enabled, std::span<const osg::Vec3f> positions, std::span<const std::uint16_t> indices,
        const Certificate& certificate, const std::shared_ptr<StreamBudget>& budget, OrdinaryFactory&& ordinary,
        Limits limits = {})
    {
        Refusal refusal = Refusal::Unsupported;
        if (enabled)
        {
            // Scope retires every unpublished snapshot BEFORE the ordinary
            // factory allocates. This cannot add old+failed+fallback backings.
            auto result = prepare(positions, indices, certificate, budget, limits);
            refusal = result.refusal;
            if (result.snapshot)
            {
                auto candidate = result.snapshot.instantiate(certificate);
                refusal = candidate.refusal;
                if (candidate.shape) return {std::move(candidate.shape), true, Refusal::None};
            }
        }
        return { std::invoke(std::forward<OrdinaryFactory>(ordinary)), false, refusal };
    }
}
#endif
