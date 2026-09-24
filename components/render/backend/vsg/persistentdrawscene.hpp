#ifndef OPENMW_COMPONENTS_RENDER_BACKEND_VSG_PERSISTENTDRAWSCENE_H
#define OPENMW_COMPONENTS_RENDER_BACKEND_VSG_PERSISTENTDRAWSCENE_H

#include "immediateeffectrealizer.hpp"
#include "effectvisibility.hpp"
#include "framecompletion.hpp"
#include "pipelineinventory.hpp"
#include <components/misc/environmentflag.hpp>
#include <components/rendercore/persistentdraw.hpp>
#include <vsg/nodes/MatrixTransform.h>
#include <vsg/nodes/Switch.h>
#include <unordered_map>

namespace RenderVsg
{
    // Render-thread owned retained scene. Geometry/material arrays are immutable;
    // only placement and membership change, after the preceding record tasks
    // have joined. Vulkan records matrix values, not pointers into these nodes.
    // Removed/rebound GPU owners remain retained until their last submission's
    // fence completes, independently of the lifetime of any CPU frame snapshot.
    class PersistentDrawScene
    {
    public:
        PersistentDrawScene() : mRoot(vsg::Group::create()), mEmpty(vsg::Group::create()),
            mPublishedRoot(vsg::Group::create()) { mPublishedRoot->addChild(mRoot); }
        vsg::ref_ptr<vsg::Group> root() const { return mPublishedRoot; }
        std::size_t size() const { return mResidents.size(); }
        std::size_t pendingRetirements() const { return mRetired.size(); }
        std::size_t realized = 0, placements = 0, removed = 0;

        template<class Compile>
        bool synchronize(const std::shared_ptr<const RenderCore::PersistentDrawFrame>& frame,
            std::optional<RenderCore::FrameId> completed, const StaticTextureResolver& resolver,
            Compile&& compile, std::string& diagnostic)
        {
            realized = placements = removed = 0;
            if (completed) (void)mRetired.collect(*completed);
            if (frame == mFrame) return true;
            const bool reset = !frame || !mFrame || frame->stream() != mFrame->stream()
                || frame->baseRevision() != mFrame->revision();
            auto pending = vsg::Group::create();
            std::vector<std::uint32_t> newlyRealized;
            auto shared = vsg::SharedObjects::create();
            mRetired.reserveAdditional(mResidents.size() + (frame ? frame->changes().size() : 0));
            const auto retire = [&](vsg::ref_ptr<vsg::Node> node) {
                if (node && mLastUse && (!completed || *completed < *mLastUse))
                {
                    if (!mRetired.queue(*mLastUse, std::move(node)))
                        throw std::logic_error("persistent draw retirement timeline rejected");
                }
            };
            const auto erase = [&](std::uint32_t slot) {
                const auto it = mResidents.find(slot);
                if (it == mResidents.end()) return;
                retire(it->second.published);
                mPages[slot / PageSize]->children[slot % PageSize] = mEmpty;
                mResidents.erase(it); ++removed;
            };
            bool valid = true;
            const auto update = [&](const RenderCore::PersistentDrawEntry& entry) {
                if (!valid) return;
                const auto existing = mResidents.find(entry.handle.slot);
                const bool resourceChanged = existing == mResidents.end() || !existing->second.ready
                    || existing->second.resource != entry.resource || existing->second.handle != entry.handle;
                if (resourceChanged)
                {
                    const auto& draw = entry.resource->draw();
                    // Immutable vertex/index/UV streams must not enter VSG's
                    // per-frame dynamic-transfer scan. A changed asset replaces
                    // its owner; placement is recorded as push-constant values.
                    auto asset = realizeImmediateEffectDraw(draw, resolver, shared, false);
                    if (!asset.valid()) { valid = false; diagnostic = asset.diagnostic; return; }
                    auto placed = vsg::MatrixTransform::create();
                    // Current local-space bounds are conservatively transformed
                    // independently by main, shadow, reflection and map traversals.
                    if (auto bound = effectCullBound(draw))
                    {
                        auto cull = vsg::CullGroup::create(); cull->bound = *bound;
                        cull->addChild(asset.root); placed->addChild(cull);
                    }
                    else placed->addChild(asset.root);
                    auto semantic = vsg::Switch::create();
                    // Same view masks as the ordinary world/effect control path.
                    semantic->addChild(placementMask(draw.semanticFlags), placed);
                    auto visible = vsg::Switch::create();
                    visible->addChild(entry.visible ? vsg::MASK_ALL : vsg::MASK_OFF, semantic);
                    if (existing != mResidents.end()) retire(existing->second.published);
                    // A failed realizer must not leave a phantom resident with
                    // no published page; reset/unload must remain safe on retry.
                    mResidents.insert_or_assign(entry.handle.slot,
                        Resident{entry.handle, entry.resource, placed, visible});
                    while (mPages.size() <= entry.handle.slot / PageSize)
                    {
                        auto page = vsg::Group::create(); page->children.assign(PageSize, mEmpty);
                        mPages.push_back(page); mRoot->addChild(page);
                    }
                    mPages[entry.handle.slot / PageSize]->children[entry.handle.slot % PageSize] = visible;
                    // Compile the semantic node, even if currently hidden.
                    pending->addChild(semantic); newlyRealized.push_back(entry.handle.slot); ++realized;
                }
                auto& resident = mResidents.at(entry.handle.slot);
                resident.placement->matrix = toVsg(entry.transform);
                resident.published->children.front().mask = entry.visible ? vsg::MASK_ALL : vsg::MASK_OFF;
                ++placements;
            };
            if (reset)
            {
                std::vector<std::uint32_t> stale;
                for (const auto& [slot, resident] : mResidents)
                {
                    const auto* entry = frame ? frame->get(slot) : nullptr;
                    if (!entry || resident.handle != entry->handle) stale.push_back(slot);
                }
                for (auto slot : stale) erase(slot);
                if (frame) frame->forEach(update);
            }
            else for (auto slot : frame->changes())
            {
                if (const auto* entry = frame->get(slot)) update(*entry);
                else erase(slot);
            }
            if (!valid || (!pending->children.empty() && !compile(pending))) return false;
            for (auto slot : newlyRealized) mResidents.at(slot).ready = true;
            if ((realized || removed) && Misc::environmentFlag<"OPENMW_V4_PIPELINE_INVENTORIES">())
                mPublishedRoot->children = {sealPipelineInventory(mRoot)};
            mFrame = frame;
            return true;
        }
        void markSubmitted(RenderCore::FrameId frame) { mLastUse = frame; }
    private:
        static constexpr auto PageSize = RenderCore::PersistentDrawFrame::PageSize;
        static vsg::dmat4 toVsg(const glm::mat4& matrix)
        {
            vsg::dmat4 result;
            for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r) result[c][r] = matrix[c][r];
            return result;
        }
        static vsg::Mask placementMask(std::uint64_t flags)
        {
            vsg::Mask mask = vsg::MASK_ALL;
            if (!(flags & RenderCore::semanticFlag(RenderCore::InstanceSemanticFlag::ShadowCaster))) mask &= ~vsg::Mask{0x1};
            if (!(flags & RenderCore::semanticFlag(RenderCore::InstanceSemanticFlag::ReflectionEligible))) mask &= ~vsg::Mask{0x2};
            if (!(flags & RenderCore::semanticFlag(RenderCore::InstanceSemanticFlag::RefractionEligible))) mask &= ~vsg::Mask{0x4};
            return mask;
        }
        struct Resident
        {
            RenderCore::PersistentDrawHandle handle;
            std::shared_ptr<const RenderCore::PersistentDrawResource> resource;
            vsg::ref_ptr<vsg::MatrixTransform> placement;
            vsg::ref_ptr<vsg::Switch> published;
            bool ready = false;
        };
        vsg::ref_ptr<vsg::Group> mRoot, mEmpty, mPublishedRoot;
        std::vector<vsg::ref_ptr<vsg::Group>> mPages;
        std::unordered_map<std::uint32_t, Resident> mResidents;
        std::shared_ptr<const RenderCore::PersistentDrawFrame> mFrame;
        FrameRetirementQueue<vsg::ref_ptr<vsg::Node>> mRetired;
        std::optional<RenderCore::FrameId> mLastUse;
    };
}
#endif
