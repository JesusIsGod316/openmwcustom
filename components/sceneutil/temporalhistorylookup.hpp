#ifndef OPENMW_SCENEUTIL_TEMPORALHISTORYLOOKUP_H
#define OPENMW_SCENEUTIL_TEMPORALHISTORYLOOKUP_H
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>

namespace SceneUtil::TemporalHistoryLookup
{
    inline std::atomic_bool enabled{false};
    inline std::atomic_uint64_t indexedLookups{0}, compatibilityComparisons{0};
    inline constexpr unsigned MaximumSurfaces = 2048;
    struct Entry { std::uint64_t identity; unsigned ordinal; };
    struct Scratch { std::array<Entry, MaximumSurfaces> entries; };
    // Draw-owned scratch over one immutable previous-frame surface vector.
    // Identity AND original ordinal preserve the original first compatible
    // match for an instance with several projections or materials.
    template<class Surfaces, class Identity>
    class Read
    {
    public:
        Read(const Surfaces& surfaces, Identity identity, std::unique_ptr<Scratch>& scratch) : mSurfaces(surfaces)
        {
            mRequested = enabled.load(std::memory_order_relaxed);
            mIndexed = mRequested && surfaces.size()<=MaximumSurfaces;
            if (!mIndexed) return;
            // One bounded allocation per draw context, reused on later frames.
            // The disabled control neither allocates nor counts comparisons.
            if (!scratch)
            {
                try { scratch = std::make_unique<Scratch>(); }
                catch (const std::bad_alloc&) { mIndexed = false; return; }
            }
            mScratch = scratch.get();
            mCount=static_cast<unsigned>(surfaces.size());
            auto& entries = mScratch->entries;
            for (unsigned i=0;i<mCount;++i) entries[i]={identity(surfaces[i]),i};
            std::sort(entries.begin(),entries.begin()+mCount,[](const auto& a,const auto& b) {
                return a.identity<b.identity || (a.identity==b.identity && a.ordinal<b.ordinal);
            });
        }
        ~Read()
        {
            if (!mRequested) return;
            indexedLookups.fetch_add(mLookups,std::memory_order_relaxed);
            compatibilityComparisons.fetch_add(mComparisons,std::memory_order_relaxed);
        }
        Read(const Read&)=delete;
        template<class Compatible>
        auto find(std::uint64_t identity, Compatible compatible)
        {
            if (!mRequested)
                return std::find_if(mSurfaces.begin(),mSurfaces.end(),compatible);
            if (!mIndexed)
                return std::find_if(mSurfaces.begin(),mSurfaces.end(),[&](const auto& previous) {
                    ++mComparisons; return compatible(previous);
                });
            ++mLookups;
            auto& entries = mScratch->entries;
            const auto end=entries.begin()+mCount;
            auto found=std::lower_bound(entries.begin(),end,identity,[](const auto& entry,auto id) {return entry.identity<id;});
            for (;found!=end && found->identity==identity;++found)
            {
                ++mComparisons;
                if (compatible(mSurfaces[found->ordinal])) return mSurfaces.begin()+found->ordinal;
            }
            return mSurfaces.end();
        }
    private:
        const Surfaces& mSurfaces;
        Scratch* mScratch = nullptr; // draw-context owner outlives this reader
        unsigned mCount=0;
        std::uint64_t mLookups=0,mComparisons=0;
        bool mIndexed=false;
        bool mRequested=false;
    };
}
#endif
