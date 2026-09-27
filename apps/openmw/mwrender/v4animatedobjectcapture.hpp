#ifndef OPENMW_MWRENDER_V4ANIMATEDOBJECTCAPTURE_H
#define OPENMW_MWRENDER_V4ANIMATEDOBJECTCAPTURE_H

#include "v4effectcapture.hpp"

namespace MWRender
{
    // Captures the evaluated object without advancing its OSG update callbacks.
    // The particles-only mode is paired with V4PersistentObject: ordinary meshes
    // stay retained, while OSG remains authoritative for live particle state.
    class V4AnimatedObjectCaptureVisitor final : public osg::NodeVisitor
    {
    public:
        enum class Mode { WholeObject, ParticlesOnly };

        V4AnimatedObjectCaptureVisitor(std::string identityPrefix, const VFS::Manager& vfs,
            NifRender::TextureIdentityCache* identities, Mode mode = Mode::WholeObject)
            : osg::NodeVisitor(TRAVERSE_ACTIVE_CHILDREN)
            , mIdentityPrefix(std::move(identityPrefix))
            , mVfs(vfs)
            , mTextureIdentities(identities)
            , mMode(mode)
        {
        }

        void apply(osg::Node& node) override
        {
            if (nestedEffectRoot(node)) return;
            if (const auto* particles = dynamic_cast<const osgParticle::ParticleSystem*>(&node))
            {
                if (!v4_effect_detail::captureParticleSystem(*particles, getNodePath(), mVfs,
                        nextIdentity("system"), mResult.draws, mResult.diagnostic, mTextureIdentities))
                    return;
            }
            if (mResult.valid()) traverse(node);
        }

        void apply(osg::Geode& geode) override
        {
            if (nestedEffectRoot(geode)) return;
            if (mResult.valid()) traverse(geode);
        }

        void apply(osg::Drawable& drawable) override
        {
            if (nestedEffectRoot(drawable)) return;
            if (mMode == Mode::ParticlesOnly)
            {
                if (auto* particles = dynamic_cast<osgParticle::ParticleSystem*>(&drawable))
                {
                    if (!v4_effect_detail::captureParticleSystem(*particles, getNodePath(), mVfs,
                            nextIdentity("system"), mResult.draws, mResult.diagnostic, mTextureIdentities))
                        return;
                }
                else if (dynamic_cast<osg::Geometry*>(&drawable))
                    ++mOrdinal; // Preserve whole-object particle identities across paths.
            }
            else if (auto* geometry = v4_effect_detail::evaluatedGeometry(drawable, *this, mResult.diagnostic))
            {
                RenderCore::ImmediateEffectDraw draw;
                if (v4_effect_detail::captureGeometry(*geometry, getNodePath(), mVfs,
                        nextIdentity("geometry"), draw, mResult.diagnostic, mTextureIdentities))
                    mResult.draws.push_back(std::move(draw));
            }
            else if (auto* particles = dynamic_cast<osgParticle::ParticleSystem*>(&drawable))
            {
                if (!v4_effect_detail::captureParticleSystem(*particles, getNodePath(), mVfs,
                        nextIdentity("system"), mResult.draws, mResult.diagnostic, mTextureIdentities))
                    return;
            }
            if (mResult.valid()) traverse(drawable);
        }

        [[nodiscard]] V4EffectCaptureResult take() { return std::move(mResult); }

    private:
        [[nodiscard]] bool nestedEffectRoot(const osg::Node& node) const noexcept
        {
            return getNodePath().size() > 1u && v4_effect_detail::isEffectRoot(node);
        }

        [[nodiscard]] std::string nextIdentity(std::string_view kind)
        {
            return mIdentityPrefix + ":" + std::string(kind) + ":" + std::to_string(mOrdinal++);
        }

        std::string mIdentityPrefix;
        const VFS::Manager& mVfs;
        NifRender::TextureIdentityCache* mTextureIdentities;
        Mode mMode;
        std::size_t mOrdinal = 0;
        V4EffectCaptureResult mResult;
    };
}

#endif
