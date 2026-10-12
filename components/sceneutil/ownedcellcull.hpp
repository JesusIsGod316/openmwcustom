#ifndef OPENMW_SCENEUTIL_OWNEDCELLCULL_H
#define OPENMW_SCENEUTIL_OWNEDCELLCULL_H

#include <osg/Group>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace SceneUtil::OwnedCellCull
{
    inline std::atomic_bool enabled{false}; // startup only, before scene publication
    struct Counters
    {
        inline static std::atomic_uint64_t publications{0}, refusals{0}, liveBytes{0}, peakBytes{0};
        inline static std::atomic_uint64_t boundReadsAvoided{0}, classificationVisitsAvoided{0}, invalidatedReads{0};
    };
    inline constexpr std::uint64_t MaximumBytes = 8 * 1024 * 1024;
    inline constexpr unsigned MaximumChildren = 4096;
    // Reserve allocator/control-block bookkeeping as well as vector capacity.
    // This is a metadata allowance; strong child references can also extend
    // the lifetime of payloads already owned by the scene.
    inline constexpr std::uint64_t AllocationAllowance = 64;

    struct Child
    {
        osg::ref_ptr<osg::Node> node;
        osg::BoundingSphere bound;
    };
    struct Snapshot
    {
        std::vector<Child> children;
        std::vector<unsigned> large, small;
        std::uint64_t generation = 0, charge = 0;
        ~Snapshot() { Counters::liveBytes.fetch_sub(charge, std::memory_order_relaxed); }
    };

    // This root owns publication. getBound's cached-valid bit is the same
    // invalidation boundary the ordinary OSG traversal uses. A STATIC flag,
    // previous camera visibility or a guessed frame number grants no reuse.
    class Group final : public osg::Group
    {
    public:
        explicit Group(float largeRadius = 400.f) : mLargeRadius(largeRadius) {}
        Group(const Group& other, const osg::CopyOp& copy = osg::CopyOp::SHALLOW_COPY)
            : osg::Group(other, copy), mLargeRadius(other.mLargeRadius) {}
        META_Node(SceneUtil, Group)

        osg::BoundingSphere computeBound() const override
        {
            // Preserve OSG's exact group aggregation, including drawable boxes,
            // absolute-reference transforms and initial-bound handling.
            const auto result = osg::Group::computeBound();
            auto previous = std::move(mSnapshot);
            ++mGeneration;
            const auto count = getNumChildren();
            if (!mGeneration || !enabled.load(std::memory_order_relaxed) || count > MaximumChildren
                || getComputeBoundingSphereCallback()) return result;
            if (previous && previous.use_count() == 1 && previous->children.capacity() >= count
                && previous->large.capacity() >= count && previous->small.capacity() >= count)
            {
                // An immutable reader still holding the previous generation
                // forbids this reuse. Only the sole root owner may republish.
                populate(*previous, count);
                mSnapshot = std::move(previous);
                ++Counters::publications;
                return result;
            }
            previous.reset();
            const std::uint64_t estimate = AllocationAllowance + sizeof(Snapshot)
                + std::uint64_t(count) * (sizeof(Child) + 2 * sizeof(unsigned));
            auto old = Counters::liveBytes.load(std::memory_order_relaxed);
            while (old <= MaximumBytes && estimate <= MaximumBytes - old)
            {
                if (Counters::liveBytes.compare_exchange_weak(old, old + estimate, std::memory_order_relaxed))
                {
                    bool assignedCharge = false;
                    try
                    {
                        auto snapshot = std::make_shared<Snapshot>();
                        snapshot->charge = estimate;
                        assignedCharge = true;
                        snapshot->children.reserve(count); snapshot->large.reserve(count); snapshot->small.reserve(count);
                        populate(*snapshot, count);
                        // Actual capacities must fit the reservation. An allocator
                        // choosing larger vectors cannot silently exceed this cap.
                        const auto actual = AllocationAllowance + sizeof(Snapshot) + snapshot->children.capacity() * sizeof(Child)
                            + (snapshot->large.capacity() + snapshot->small.capacity()) * sizeof(unsigned);
                        if (actual > estimate) { ++Counters::refusals; return result; }
                        auto peak = Counters::peakBytes.load(std::memory_order_relaxed);
                        while (peak < old + estimate && !Counters::peakBytes.compare_exchange_weak(
                            peak, old + estimate, std::memory_order_relaxed)) {}
                        mSnapshot = std::move(snapshot);
                        ++Counters::publications;
                        return result;
                    }
                    catch (const std::bad_alloc&)
                    {
                        // make_shared failure precedes charge ownership. Vector
                        // failure follows it and is released by Snapshot.
                        if (!assignedCharge)
                            Counters::liveBytes.fetch_sub(estimate, std::memory_order_relaxed);
                        ++Counters::refusals;
                        return result;
                    }
                }
            }
            ++Counters::refusals;
            return result;
        }
        std::shared_ptr<const Snapshot> snapshot() const { return current(mSnapshot) ? mSnapshot : nullptr; }
        bool current(const std::shared_ptr<const Snapshot>& value) const noexcept
        {
            return value && _boundingSphereComputed && !getComputeBoundingSphereCallback()
                && value.get() == mSnapshot.get() && value->generation == mGeneration
                && value->children.size() == getNumChildren();
        }
    private:
        void populate(Snapshot& snapshot, unsigned count) const
        {
            snapshot.generation = mGeneration;
            snapshot.large.clear(); snapshot.small.clear(); snapshot.children.resize(count);
            for (unsigned i = 0; i < count; ++i)
            {
                auto* child = const_cast<osg::Node*>(getChild(i));
                snapshot.children[i] = {child, child->getBound()};
                const auto& bound = snapshot.children[i].bound;
                (bound.valid() && bound.radius() >= mLargeRadius ? snapshot.large : snapshot.small).push_back(i);
            }
        }
        float mLargeRadius;
        mutable std::uint64_t mGeneration = 0;
        mutable std::shared_ptr<Snapshot> mSnapshot;
    };

    // One current cull callback owns this reader. Checks precede every reuse;
    // callbacks that dirty/recompute the root immediately select live OSG.
    class Read
    {
    public:
        explicit Read(osg::Group& node)
        {
            if (enabled.load(std::memory_order_relaxed))
                mGroup = dynamic_cast<Group*>(&node);
            if (mGroup) mSnapshot = mGroup->snapshot();
        }
        ~Read()
        {
            if (!mGroup) return;
            Counters::boundReadsAvoided.fetch_add(mBounds, std::memory_order_relaxed);
            Counters::classificationVisitsAvoided.fetch_add(mVisits, std::memory_order_relaxed);
            Counters::invalidatedReads.fetch_add(mInvalid, std::memory_order_relaxed);
        }
        Read(const Read&) = delete;
        bool current() const noexcept { return mGroup && mGroup->current(mSnapshot); }
        bool next(unsigned& cursor, unsigned& position, unsigned count, bool large)
        {
            if (!current())
            {
                if (mSnapshot && !mFellBack) { ++mInvalid; mFellBack = true; }
                return cursor < count;
            }
            const auto& indices = large ? mSnapshot->large : mSnapshot->small;
            while (position < indices.size() && indices[position] < cursor) ++position;
            if (position == indices.size())
            {
                mVisits += count - cursor;
                return false;
            }
            mVisits += indices[position] - cursor;
            cursor = indices[position++];
            return cursor < count;
        }
        const osg::BoundingSphere& bound(unsigned index, osg::Node& child)
        {
            if (current() && index < mSnapshot->children.size() && mSnapshot->children[index].node == &child)
            { ++mBounds; return mSnapshot->children[index].bound; }
            return child.getBound();
        }
    private:
        Group* mGroup = nullptr;
        std::shared_ptr<const Snapshot> mSnapshot;
        std::uint64_t mBounds = 0, mVisits = 0, mInvalid = 0;
        bool mFellBack = false;
    };
}
#endif
