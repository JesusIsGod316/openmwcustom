#include "residencyledger.hpp"

#include <algorithm>
#include <limits>

namespace RenderVsg
{
    std::size_t ResidencyLedger::index(ResidencyCategory category) noexcept
    {
        return static_cast<std::size_t>(category);
    }

    bool ResidencyLedger::checkedAdd(std::uint64_t& target, std::uint64_t bytes) noexcept
    {
        if (bytes > std::numeric_limits<std::uint64_t>::max() - target)
            return false;
        target += bytes;
        return true;
    }

    bool ResidencyLedger::checkedSubtract(std::uint64_t& target, std::uint64_t bytes) noexcept
    {
        if (bytes > target)
            return false;
        target -= bytes;
        return true;
    }

    bool ResidencyLedger::addLogicalLive(ResidencyCategory category, std::uint64_t bytes)
    {
        if (index(category) >= ResidencyCategoryCount)
            return false;
        std::scoped_lock lock(mMutex);
        return checkedAdd(mCategories[index(category)].logicalLiveBytes, bytes);
    }

    bool ResidencyLedger::removeLogicalLive(ResidencyCategory category, std::uint64_t bytes)
    {
        if (index(category) >= ResidencyCategoryCount)
            return false;
        std::scoped_lock lock(mMutex);
        return checkedSubtract(mCategories[index(category)].logicalLiveBytes, bytes);
    }

    bool ResidencyLedger::beginUpload(ResidencyCategory category, std::uint64_t bytes)
    {
        if (index(category) >= ResidencyCategoryCount)
            return false;
        std::scoped_lock lock(mMutex);
        return checkedAdd(mCategories[index(category)].pendingUploadBytes, bytes);
    }

    bool ResidencyLedger::cancelUpload(ResidencyCategory category, std::uint64_t bytes)
    {
        if (index(category) >= ResidencyCategoryCount)
            return false;
        std::scoped_lock lock(mMutex);
        return checkedSubtract(mCategories[index(category)].pendingUploadBytes, bytes);
    }

    bool ResidencyLedger::commitUpload(ResidencyCategory category, std::uint64_t bytes, bool pinned)
    {
        if (index(category) >= ResidencyCategoryCount)
            return false;

        std::scoped_lock lock(mMutex);
        ResidencyCounters& counters = mCategories[index(category)];
        if (counters.pendingUploadBytes < bytes)
            return false;
        if (bytes > std::numeric_limits<std::uint64_t>::max() - counters.residentBytes)
            return false;
        std::uint64_t& residencyClass = pinned ? counters.pinnedBytes : counters.evictableBytes;
        if (bytes > std::numeric_limits<std::uint64_t>::max() - residencyClass)
            return false;

        counters.pendingUploadBytes -= bytes;
        counters.residentBytes += bytes;
        residencyClass += bytes;
        return true;
    }

    std::uint64_t ResidencyLedger::pendingClassBytesLocked(ResidencyCategory category, bool pinned) const noexcept
    {
        std::uint64_t total = 0;
        for (const Retirement& retirement : mRetirements)
        {
            if (retirement.category == category && retirement.pinned == pinned)
                total += retirement.bytes;
        }
        return total;
    }

    bool ResidencyLedger::queueRetire(
        ResidencyCategory category, std::uint64_t bytes, bool pinned, RenderCore::FrameId lastUseFrame)
    {
        if (index(category) >= ResidencyCategoryCount || !lastUseFrame.valid())
            return false;

        std::scoped_lock lock(mMutex);
        ResidencyCounters& counters = mCategories[index(category)];
        if (counters.pendingRetireBytes > counters.residentBytes
            || bytes > counters.residentBytes - counters.pendingRetireBytes)
            return false;

        const std::uint64_t residencyClass = pinned ? counters.pinnedBytes : counters.evictableBytes;
        const std::uint64_t alreadyQueued = pendingClassBytesLocked(category, pinned);
        if (alreadyQueued > residencyClass || bytes > residencyClass - alreadyQueued)
            return false;
        if (!checkedAdd(counters.pendingRetireBytes, bytes))
            return false;

        mRetirements.push_back(Retirement{ category, bytes, pinned, lastUseFrame });
        return true;
    }

    std::uint64_t ResidencyLedger::collectRetired(RenderCore::FrameId completedFrame)
    {
        if (!completedFrame.valid())
            return 0;

        std::scoped_lock lock(mMutex);
        std::uint64_t released = 0;
        auto firstPending = std::remove_if(mRetirements.begin(), mRetirements.end(), [&](const Retirement& retirement) {
            if (retirement.lastUseFrame > completedFrame)
                return false;

            ResidencyCounters& counters = mCategories[index(retirement.category)];
            std::uint64_t& residencyClass = retirement.pinned ? counters.pinnedBytes : counters.evictableBytes;
            if (!checkedSubtract(counters.pendingRetireBytes, retirement.bytes)
                || !checkedSubtract(counters.residentBytes, retirement.bytes)
                || !checkedSubtract(residencyClass, retirement.bytes))
                return false;

            released += retirement.bytes;
            return true;
        });
        mRetirements.erase(firstPending, mRetirements.end());
        return released;
    }

    ResidencySnapshot ResidencyLedger::snapshot() const
    {
        std::scoped_lock lock(mMutex);
        ResidencySnapshot result;
        result.categories = mCategories;

        for (const ResidencyCounters& category : result.categories)
        {
            checkedAdd(result.total.logicalLiveBytes, category.logicalLiveBytes);
            checkedAdd(result.total.residentBytes, category.residentBytes);
            checkedAdd(result.total.pendingUploadBytes, category.pendingUploadBytes);
            checkedAdd(result.total.pendingRetireBytes, category.pendingRetireBytes);
            checkedAdd(result.total.pinnedBytes, category.pinnedBytes);
            checkedAdd(result.total.evictableBytes, category.evictableBytes);
        }
        return result;
    }

    std::size_t ResidencyLedger::pendingRetirementCount() const
    {
        std::scoped_lock lock(mMutex);
        return mRetirements.size();
    }
}
