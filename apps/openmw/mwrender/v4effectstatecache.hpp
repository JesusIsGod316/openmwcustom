#ifndef OPENMW_MWRENDER_V4EFFECTSTATECACHE_H
#define OPENMW_MWRENDER_V4EFFECTSTATECACHE_H

#include <algorithm>
#include <cstdint>
#include <map>
#include <osg/Node>
#include <osg/StateSet>
#include <osg/observer_ptr>
#include <vector>

namespace MWRender::v4_effect_detail
{
    // Owning-thread structural cache, NOT a cache of mutable material values.
    // Merged attributes/uniforms still reference the live controller outputs.
    // Exact list/override comparisons catch edits without dirty notifications.
    // Only neutral values extracted afterwards may cross the frame handoff.
    class EffectStateCache
    {
    public:
        explicit EffectStateCache(std::size_t capacity = 2048)
            : mCapacity(capacity)
        {
        }

        osg::ref_ptr<osg::StateSet> get(const osg::NodePath& path, const osg::StateSet* drawable)
        {
            if (++mCalls % 1024 == 0)
                pruneExpired();
            mKey.clear();
            for (const auto* node : path)
                if (node && node->getStateSet())
                    mKey.push_back(node->getStateSet());
            if (drawable)
                mKey.push_back(drawable);
            // Do not retain arbitrary large/custom state chains. Ordinary
            // merging remains the exact fallback, including for capacity zero.
            std::size_t bindings = 0;
            for (const auto* state : mKey)
            {
                bindings += state->getModeList().size() + state->getAttributeList().size()
                    + state->getUniformList().size() + state->getDefineList().size();
                for (const auto& unit : state->getTextureAttributeList())
                    bindings += unit.size();
                for (const auto& unit : state->getTextureModeList())
                    bindings += unit.size();
            }
            const bool admissible = mCapacity && mKey.size() <= 32 && bindings <= 512;
            if (admissible)
            {
                auto found = mEntries.find(mKey);
                if (found != mEntries.end() && current(found->second))
                {
                    found->second.used = ++mClock;
                    ++hits;
                    return found->second.merged;
                }
            }
            ++misses;
            auto merged = osg::ref_ptr<osg::StateSet>(new osg::StateSet);
            for (const auto* state : mKey)
                merged->merge(*state);
            if (!admissible)
                return merged;
            // Expired source chains must not keep unloaded texture backing
            // indefinitely. Prune on misses and use bounded LRU for live chains.
            if (mEntries.size() >= mCapacity)
                pruneExpired();
            mEntries.erase(mKey);
            if (mEntries.size() >= mCapacity)
            {
                const auto oldest = std::min_element(mEntries.begin(), mEntries.end(),
                    [](const auto& a, const auto& b) { return a.second.used < b.second.used; });
                mEntries.erase(oldest);
            }
            Entry entry;
            entry.merged = merged;
            entry.used = ++mClock;
            entry.sources.reserve(mKey.size());
            for (const auto* state : mKey)
                entry.sources.push_back({ state, new osg::StateSet(*state, osg::CopyOp::SHALLOW_COPY) });
            mEntries.emplace(mKey, std::move(entry));
            return merged;
        }

        std::uint64_t hits = 0, misses = 0;

    private:
        struct Source
        {
            osg::observer_ptr<const osg::StateSet> owner;
            osg::ref_ptr<osg::StateSet> structure;
        };
        struct Entry
        {
            std::vector<Source> sources;
            osg::ref_ptr<osg::StateSet> merged;
            std::uint64_t used = 0;
        };
        void pruneExpired()
        {
            std::erase_if(mEntries, [](const auto& value) {
                return std::any_of(value.second.sources.begin(), value.second.sources.end(),
                    [](const Source& source) { return !source.owner.valid(); });
            });
        }
        static bool current(const Entry& entry)
        {
            for (const auto& source : entry.sources)
            {
                const auto* a = source.owner.get();
                const auto& b = *source.structure;
                if (!a || a->getModeList() != b.getModeList() || a->getAttributeList() != b.getAttributeList()
                    || a->getTextureModeList() != b.getTextureModeList()
                    || a->getTextureAttributeList() != b.getTextureAttributeList()
                    || a->getUniformList() != b.getUniformList() || a->getDefineList() != b.getDefineList()
                    || a->getRenderBinMode() != b.getRenderBinMode() || a->getBinNumber() != b.getBinNumber()
                    || a->getBinName() != b.getBinName() || a->getRenderingHint() != b.getRenderingHint()
                    || a->getNestRenderBins() != b.getNestRenderBins())
                    return false;
            }
            return true;
        }
        std::size_t mCapacity;
        std::uint64_t mClock = 0, mCalls = 0;
        std::vector<const osg::StateSet*> mKey;
        struct KeyLess
        {
            bool operator()(
                const std::vector<const osg::StateSet*>& a, const std::vector<const osg::StateSet*>& b) const
            {
                return std::lexicographical_compare(
                    a.begin(), a.end(), b.begin(), b.end(), std::less<const osg::StateSet*>{});
            }
        };
        std::map<std::vector<const osg::StateSet*>, Entry, KeyLess> mEntries;
    };
}
#endif
