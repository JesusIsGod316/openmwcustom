#ifndef OPENMW_SCENEUTIL_CULLVIEWCACHE_H
#define OPENMW_SCENEUTIL_CULLVIEWCACHE_H

#include <osg/Camera>
#include <osg/Matrixd>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace SceneUtil::CullViewCache
{
    inline bool enabled()
    {
        static const bool value = [] {
            const char* v = std::getenv("OPENMW_P9_CULL_INPUT_REUSE");
            return v && std::strcmp(v, "1") == 0;
        }();
        return value;
    }

    // A traversal-local calculation cache, NOT a visibility-result cache.
    // No frame-number reuse, additional occlusion tests or scenegraph ownership.
    class Scope
    {
    public:
        inline static thread_local const Scope* current = nullptr;
        explicit Scope(const osg::Camera& camera)
            : mParent(current), mCamera(&camera), mView(camera.getViewMatrix())
        {
            if (mParent && mParent->matches(camera))
            {
                mInverse = mParent->mInverse;
                mValid = true;
            }
            else
            {
                mValid = mInverse.invert(mView);
                for (unsigned r = 0; r < 4; ++r)
                    for (unsigned c = 0; c < 4; ++c)
                        mValid = mValid && std::isfinite(mView(r, c)) && std::isfinite(mInverse(r, c));
            }
            current = this;
        }
        ~Scope() { current = mParent; }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
        bool matches(const osg::Camera& camera) const
        {
            return mValid && mCamera == &camera && mView == camera.getViewMatrix();
        }
        static const osg::Matrixd* find(const osg::Camera& camera)
        {
            return current && current->matches(camera) ? &current->mInverse : nullptr;
        }
    private:
        const Scope* mParent;
        const osg::Camera* mCamera;
        osg::Matrixd mView, mInverse;
        bool mValid = false;
    };
}
#endif
