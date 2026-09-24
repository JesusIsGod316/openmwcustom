#ifndef OPENMW_COMPONENTS_RENDER_BACKEND_VSG_FRAMERESOURCEPOOL_H
#define OPENMW_COMPONENTS_RENDER_BACKEND_VSG_FRAMERESOURCEPOOL_H

#include <components/rendercore/resources.hpp>

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace RenderVsg
{
    // Mutable resources must have separate versions while earlier submissions
    // still read them. CPU frame progression is NOT a completion signal. The
    // caller supplies the fence-backed completion watermark, and marks use only
    // after successful submission. Published graphs retain their own references.
    // This pool is render-thread owned; it does not introduce a GPU wait.
    template <class Object>
    class FrameResourcePool
    {
    public:
        explicit FrameResourcePool(std::size_t maximumVersions)
            : mMaximumVersions(maximumVersions)
        {
            if (maximumVersions == 0)
                throw std::invalid_argument("frame resource pool requires at least one version");
        }

        void beginFrame(RenderCore::FrameId frame, std::optional<RenderCore::FrameId> completedThrough)
        {
            if (!frame.valid() || (mLastSubmitted && frame <= *mLastSubmitted)
                || (completedThrough && (!completedThrough->valid() || *completedThrough >= frame
                    || (mCompletedThrough && *completedThrough < *mCompletedThrough))))
                throw std::invalid_argument("frame resource pool received an invalid frame timeline");
            mPreparedFrame = frame;
            if (completedThrough)
                mCompletedThrough = completedThrough;
            for (auto& [identity, versions] : mObjects)
                for (auto& version : versions)
                    version.selected = false;
        }

        // Returns either a completed version or a fresh default object. A caller
        // may update it in-place or replace it when its immutable layout differs.
        // References remain valid until another acquire for this identity or GC.
        Object& acquire(const std::string& identity)
        {
            if (!mPreparedFrame || identity.empty())
                throw std::invalid_argument("frame resource acquisition requires a frame and identity");
            auto& versions = mObjects[identity];
            if (std::any_of(versions.begin(), versions.end(), [](const Version& v) { return v.selected; }))
                throw std::invalid_argument("duplicate mutable resource identity in a frame: " + identity);
            auto available = std::find_if(versions.begin(), versions.end(),
                [&](const Version& version) { return writable(version); });
            if (available == versions.end())
            {
                if (versions.size() >= mMaximumVersions)
                    throw std::runtime_error("mutable resource versions exceed the submission ring: " + identity);
                versions.emplace_back();
                available = std::prev(versions.end());
            }
            available->selected = true;
            return available->object;
        }

        // Read sharing is safe even while an earlier submission uses this
        // version. The predicate must prove that ALL GPU/record-visible values
        // are unchanged. A const result deliberately prevents the caller from
        // treating this as a writable acquisition; changed values still use
        // acquire() and its fence-backed copy-on-write ring.
        template <class Predicate>
        const Object* selectUnchanged(const std::string& identity, Predicate&& unchanged)
        {
            if (!mPreparedFrame || identity.empty())
                throw std::invalid_argument("immutable resource selection requires a frame and identity");
            const auto found = mObjects.find(identity);
            if (found == mObjects.end()) return nullptr;
            auto& versions = found->second;
            if (std::any_of(versions.begin(), versions.end(), [](const Version& v) { return v.selected; }))
                throw std::invalid_argument("duplicate resource identity in a frame: " + identity);
            for (auto& version : versions)
                if (unchanged(std::as_const(version.object)))
                {
                    version.selected = true;
                    return &version.object;
                }
            return nullptr;
        }

        // Remove disappeared identities only when their final use completed.
        // Active identities retain a bounded ring of reusable versions; topology
        // changes replace writable slots rather than accumulating old layouts.
        void collectUnused()
        {
            for (auto it = mObjects.begin(); it != mObjects.end();)
            {
                auto& versions = it->second;
                const bool active = std::any_of(versions.begin(), versions.end(),
                    [](const Version& version) { return version.selected; });
                if (!active)
                    std::erase_if(versions, [&](const Version& version) { return writable(version); });
                if (versions.empty())
                    it = mObjects.erase(it);
                else
                    ++it;
            }
        }

        [[nodiscard]] bool markSubmitted(RenderCore::FrameId frame) noexcept
        {
            if (!mPreparedFrame || frame != *mPreparedFrame || (mLastSubmitted && frame <= *mLastSubmitted))
                return false;
            for (auto& [identity, versions] : mObjects)
                for (auto& version : versions)
                    if (version.selected)
                        version.lastUse = frame;
            mLastSubmitted = frame;
            mPreparedFrame.reset();
            return true;
        }

        [[nodiscard]] std::size_t size() const noexcept
        {
            std::size_t count = 0;
            for (const auto& [identity, versions] : mObjects)
                count += versions.size();
            return count;
        }

        // Borrowed, read-only observation on the pool owner thread. Does not
        // acquire a version, mark it selected, or advance completion.
        template <class Fn>
        void inspect(Fn&& fn) const
        {
            for (const auto& [identity, versions] : mObjects)
                for (const auto& version : versions)
                    fn(identity, version.object, version.selected, writable(version), version.lastUse);
        }

    private:
        struct Version
        {
            Object object{};
            std::optional<RenderCore::FrameId> lastUse;
            bool selected = false;
        };

        [[nodiscard]] bool writable(const Version& version) const noexcept
        {
            return !version.lastUse || (mCompletedThrough && *version.lastUse <= *mCompletedThrough);
        }

        std::size_t mMaximumVersions;
        std::optional<RenderCore::FrameId> mPreparedFrame;
        std::optional<RenderCore::FrameId> mLastSubmitted;
        std::optional<RenderCore::FrameId> mCompletedThrough;
        std::unordered_map<std::string, std::vector<Version>> mObjects;
    };
}

#endif
