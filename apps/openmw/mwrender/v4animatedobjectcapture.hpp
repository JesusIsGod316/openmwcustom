#ifndef OPENMW_MWRENDER_V4ANIMATEDOBJECTCAPTURE_H
#define OPENMW_MWRENDER_V4ANIMATEDOBJECTCAPTURE_H

#include "v4effectcapture.hpp"

#include <osg/LOD>
#include <osg/Transform>

#include <optional>

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
            NifRender::TextureIdentityCache* identities, Mode mode = Mode::WholeObject,
            std::optional<glm::dvec3> eyePoint = std::nullopt, float lodScale = 1.0f)
            : osg::NodeVisitor(TRAVERSE_ACTIVE_CHILDREN)
            , mIdentityPrefix(std::move(identityPrefix))
            , mVfs(vfs)
            , mTextureIdentities(identities)
            , mMode(mode)
            , mEyePoint(std::move(eyePoint))
            , mLodScale(std::max(lodScale, 1.0e-6f))
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

        // Update-only visitors have no CullVisitor eye point, so osg::LOD's
        // TRAVERSE_ACTIVE_CHILDREN path otherwise evaluates distance from zero.
        // That made compatibility-captured LOD particle objects (notably
        // chimney smoke) switch or overlap the wrong child. Use the semantic
        // main-camera eye when available and preserve the existing path for
        // non-distance LOD modes until their screen-size contract is native.
        void apply(osg::LOD& lod) override
        {
            if (nestedEffectRoot(lod) || !mResult.valid())
                return;
            if (!mEyePoint || lod.getRangeMode() != osg::LOD::DISTANCE_FROM_EYE_POINT)
            {
                traverse(lod);
                return;
            }

            const osg::Matrixd localToWorld = osg::computeLocalToWorld(getNodePath());
            const osg::Vec3d center = lod.getCenter() * localToWorld;
            const glm::dvec3 delta(center.x() - mEyePoint->x, center.y() - mEyePoint->y,
                center.z() - mEyePoint->z);
            const double range = glm::length(delta) * static_cast<double>(mLodScale);
            const auto& ranges = lod.getRangeList();
            const unsigned count = std::min<unsigned>(lod.getNumChildren(), ranges.size());
            for (unsigned i = 0; i < count && mResult.valid(); ++i)
                if (ranges[i].first <= range && range < ranges[i].second)
                    lod.getChild(i)->accept(*this);
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
        std::optional<glm::dvec3> mEyePoint;
        float mLodScale = 1.0f;
        std::size_t mOrdinal = 0;
        V4EffectCaptureResult mResult;
    };
}

#endif
