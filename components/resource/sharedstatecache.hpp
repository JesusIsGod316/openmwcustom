#ifndef OPENMW_COMPONENTS_RESOURCE_SHAREDSTATECACHE_H
#define OPENMW_COMPONENTS_RESOURCE_SHAREDSTATECACHE_H
#include "cachemaintenance.hpp"
#include <osgDB/SharedStateManager>
#include <osg/StateSet>
#include <osg/Texture>
#include <mutex>
#include <utility>
#include <vector>

namespace Resource
{
    class SharedStateManager : public osgDB::SharedStateManager
    {
    public:
        size_t getNumSharedTextures() const { return _sharedTextureList.size(); }

        size_t getNumSharedStateSets() const { return _sharedStateSetList.size(); }

        std::pair<TextureSet, StateSetSet> detachAll()
        {
            TextureSet textures;
            StateSetSet states;
            {
                std::lock_guard<OpenThreads::Mutex> lock(_listMutex);
                textures.swap(_sharedTextureList); states.swap(_sharedStateSetList);
                mTextureNext = _sharedTextureList.end(); mStateNext = _sharedStateSetList.end();
            }
            return {std::move(textures), std::move(states)};
        }

        void pruneBudgeted(std::vector<osg::ref_ptr<osg::Object>>& release, CacheMaintenanceBudget& budget)
        {
            std::lock_guard<OpenThreads::Mutex> lock(_listMutex);
            // share() only inserts; this class owns all budgeted pruning. Its
            // clear path resets cursors. Values remain alive in release until
            // BOTH list and SceneManager share locks have been released.
            const auto visit = [&](auto& values, auto& next) {
                if (next == values.end()) next = values.begin();
                if (next == values.end()) return false;
                if (!budget.scan()) return false;
                auto it = next++;
                if ((*it)->referenceCount() == 1 && budget.release())
                { release.emplace_back(it->get()); values.erase(it); }
                return true;
            };
            for (unsigned i = 0; i < 128 && release.size() < 8 && budget.available(); ++i)
            {
                const bool didWork = mTextures ? visit(_sharedTextureList, mTextureNext)
                                             : visit(_sharedStateSetList, mStateNext);
                mTextures = !mTextures;
                if (!didWork && _sharedTextureList.empty() && _sharedStateSetList.empty()) break;
            }
        }
    private:
        TextureSet::iterator mTextureNext = _sharedTextureList.end();
        StateSetSet::iterator mStateNext = _sharedStateSetList.end();
        bool mTextures = false;
    };

}
#endif
