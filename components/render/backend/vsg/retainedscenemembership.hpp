#ifndef OPENMW_RENDER_VSG_RETAINEDSCENEMEMBERSHIP_H
#define OPENMW_RENDER_VSG_RETAINEDSCENEMEMBERSHIP_H
#include <vsg/nodes/Group.h>
#include "pipelineinventory.hpp"
#include <components/misc/environmentflag.hpp>
#include <string>
#include <unordered_map>

namespace RenderVsg
{
    // CPU graph membership only. The caller's fence-backed resident pool owns
    // removed/replaced GPU resources; this class never authorizes GPU writes.
    class RetainedSceneMembership
    {
    public:
        RetainedSceneMembership() : mRoot(vsg::Group::create()), mPublished(vsg::Group::create())
        { mPublished->addChild(mRoot); }
        auto root() const { return mInventories ? mPublished : mRoot; }
        void begin() { ++mCapture; }
        void select(const std::string& key, vsg::ref_ptr<vsg::Node> node)
        {
            const auto [it, added] = mMembers.try_emplace(key, Entry{mRoot->children.size(), mCapture});
            if (added) { mKeys.push_back(key); mRoot->addChild(std::move(node)); mChanged = true; }
            else
            {
                it->second.seen = mCapture;
                mChanged |= mRoot->children[it->second.index] != node;
                mRoot->children[it->second.index] = std::move(node);
            }
        }
        void finish()
        {
            for (auto it = mMembers.begin(); it != mMembers.end();)
            {
                if (it->second.seen == mCapture) { ++it; continue; }
                const auto index = it->second.index;
                if (index + 1 != mKeys.size())
                {
                    mKeys[index] = std::move(mKeys.back());
                    mRoot->children[index] = std::move(mRoot->children.back());
                    mMembers.at(mKeys[index]).index = index;
                }
                mKeys.pop_back(); mRoot->children.pop_back(); it = mMembers.erase(it);
                mChanged = true;
            }
            if (mChanged && mInventories) mPublished->children = {sealPipelineInventory(mRoot)};
            mChanged = false;
        }
    private:
        struct Entry { std::size_t index; std::uint64_t seen; };
        vsg::ref_ptr<vsg::Group> mRoot, mPublished;
        bool mChanged = false;
        const bool mInventories = Misc::environmentFlag<"OPENMW_V4_PIPELINE_INVENTORIES">();
        std::unordered_map<std::string, Entry> mMembers;
        std::vector<std::string> mKeys;
        std::uint64_t mCapture = 0;
    };
}
#endif
