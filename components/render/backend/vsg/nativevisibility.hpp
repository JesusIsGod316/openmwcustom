#ifndef OPENMW_COMPONENTS_RENDER_VSG_NATIVEVISIBILITY_H
#define OPENMW_COMPONENTS_RENDER_VSG_NATIVEVISIBILITY_H

#include "staticworldplan.hpp"
#include <components/rendercore/framerenderstate.hpp>
#include <extern/maskedoc/MaskedOcclusionCulling.h>
#include <vsg/nodes/Group.h>
#include <vsg/vk/CommandBuffer.h>
#include <vsg/vk/State.h>
#include <array>
#include <cmath>
#include <limits>
#include <memory>

namespace RenderVsg
{
    // CPU visibility is a main-view recording decision, never a scene mutation.
    // Compilation always traverses children. Other views ignore main-camera
    // occlusion; optionally they perform their own conservative frustum test.
    class MainViewVisibility final : public vsg::Inherit<vsg::Group, MainViewVisibility>
    {
    public:
        std::uint32_t mainViewId = 0;
        bool visible = true;
        bool bounded = false;
        bool terrain = false;
        bool perViewFrustum = std::getenv("OPENMW_V4_MULTIVIEW_FRUSTUM") != nullptr;
        glm::dvec3 minimum{ std::numeric_limits<double>::max() };
        glm::dvec3 maximum{ std::numeric_limits<double>::lowest() };
        struct Occluder
        {
            std::shared_ptr<const RenderCore::MeshPayload> mesh;
            RenderCore::MeshSurface surface;
            glm::dmat4 transform{1.0};
            RenderCore::CullMode cull;
            RenderCore::FrontFaceWinding winding;
        };
        std::vector<Occluder> occluders;

        bool visibleFor(std::uint32_t viewId) const noexcept { return visible || viewId != mainViewId; }
        void accept(vsg::RecordTraversal& traversal) const override
        {
            const auto* command = traversal.getCommandBuffer();
            if (command && !visibleFor(command->viewID)) return;
            if (perViewFrustum && bounded)
            {
                // These are world-space aggregate bounds outside the instance
                // transforms. Use THIS traversal's frustum: reflection, local
                // map and each shadow cascade must not inherit main visibility.
                // Reject before entering the entire population's draw/state DAG.
                const auto center = (minimum + maximum) * .5;
                double radius = glm::length(maximum - center);
                radius += std::max(1e-4, radius * 1e-5);
                if (!traversal.getState()->intersect(vsg::dsphere(center.x,center.y,center.z,radius))) return;
            }
            vsg::Group::accept(traversal);
        }

        // Called only while constructing a replacement generation. Bounds come
        // from actual vertices, not potentially stale authored bounding boxes.
        bool include(const RenderCore::RenderWorld& world, const StaticAssetPlan& asset,
            const RenderCore::WorldTransform& placement, bool isTerrain)
        {
            terrain = terrain || isTerrain;
            const auto matrix = staticInstancePlacementMatrix(placement);
            for (const auto& draw : asset.draws)
            {
                const auto* mesh = world.get(draw.mesh);
                const auto* material = world.get(draw.material);
                if (!mesh || !mesh->payload || mesh->skinned || mesh->morphed || draw.billboard
                    || !material || material->treeAnimation || material->refraction || material->softEffect
                    || !material->depthTest || material->stencil.enabled || material->wireframe
                    || draw.surface.topology == RenderCore::PrimitiveTopology::Points
                    || draw.surface.topology == RenderCore::PrimitiveTopology::Lines)
                    return false;
                const auto transform = matrix * glm::dmat4(draw.worldTransform);
                for (const auto& position : mesh->payload->positions)
                {
                    const glm::dvec4 point = transform * glm::dvec4(position, 1.0);
                    if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z)
                        || std::abs(point.w - 1.0) > 1e-6)
                        return false;
                    minimum = glm::min(minimum, glm::dvec3(point));
                    maximum = glm::max(maximum, glm::dvec3(point));
                    bounded = true;
                }
                // Only exact opaque LAND geometry writes the software depth.
                // Alpha cutouts, decals and blended LAND overlays cannot occlude.
                if (isTerrain && material->terrainLayer && !material->alphaBlendEnabled
                    && !material->alphaTestEnabled && material->alpha == 1.f
                    && material->depthTest && material->depthWrite && !material->wireframe
                    && !material->decal && !material->stencil.enabled
                    && draw.surface.topology == RenderCore::PrimitiveTopology::Triangles)
                    occluders.push_back({mesh->payload, draw.surface, transform,
                        draw.pipeline.fixedFunction.raster.cullMode, draw.pipeline.fixedFunction.raster.frontFace});
            }
            return true;
        }
    };

    struct ProjectedVisibilityBounds
    {
        bool outside = false;
        bool queryable = false;
        float left = -1.f, top = -1.f, right = 1.f, bottom = 1.f, nearestW = 0.f;
    };

    inline ProjectedVisibilityBounds projectVisibilityBounds(
        const MainViewVisibility& node, const glm::dmat4& clip)
    {
        ProjectedVisibilityBounds result;
        if (!node.bounded) return result;
        std::array<glm::dvec4, 8> points;
        for (unsigned i = 0; i < 8; ++i)
        {
            points[i] = clip * glm::dvec4(
                i & 1 ? node.maximum.x : node.minimum.x,
                i & 2 ? node.maximum.y : node.minimum.y,
                i & 4 ? node.maximum.z : node.minimum.z, 1.0);
            for (unsigned j = 0; j < 4; ++j)
                if (!std::isfinite(points[i][j])) return result;
        }
        // Vulkan Z in [0,w]. An expanded plane test tolerates floating-point
        // disagreement with the GPU. Crossing any plane stays potentially visible.
        for (unsigned plane = 0; plane < 6; ++plane)
        {
            bool outside = true;
            for (const auto& p : points)
            {
                const double d[] = {p.w + p.x, p.w - p.x, p.w + p.y, p.w - p.y, p.z, p.w - p.z};
                outside = outside && d[plane] < -1e-5 * std::max(1.0, std::abs(p.w));
            }
            if (outside) { result.outside = true; return result; }
        }
        glm::dvec2 low(1.0), high(-1.0);
        double nearest = std::numeric_limits<double>::max();
        for (const auto& p : points)
        {
            // Camera/near-plane crossings are never an occlusion query.
            if (p.w <= 1e-3 || p.z < 0.0 || p.z >= p.w) return result;
            low = glm::min(low, glm::dvec2(p) / p.w);
            high = glm::max(high, glm::dvec2(p) / p.w);
            nearest = std::min(nearest, p.w);
        }
        // Two software pixels of conservative expansion; depth moves toward eye.
        result.left = static_cast<float>(std::max(-1.0, low.x - 4.0 / 384.0));
        result.right = static_cast<float>(std::min(1.0, high.x + 4.0 / 384.0));
        result.top = static_cast<float>(std::max(-1.0, low.y - 4.0 / 216.0));
        result.bottom = static_cast<float>(std::min(1.0, high.y + 4.0 / 216.0));
        result.nearestW = static_cast<float>(nearest * 0.999);
        result.queryable = result.left < result.right && result.top < result.bottom;
        return result;
    }

    class NativeVisibility
    {
        struct Destroy { void operator()(MaskedOcclusionCulling* p) const { if (p) MaskedOcclusionCulling::Destroy(p); } };
        std::unique_ptr<MaskedOcclusionCulling, Destroy> mDepth;
        std::vector<glm::vec4> mVertices;
        std::vector<unsigned> mIndices;
    public:
        struct Stats { std::size_t candidates=0, frustum=0, occluded=0, triangles=0, examined=0; };
        // Current-camera, current-generation, single-writer depth. No temporal
        // reuse and no asynchronous result can hide geometry after a camera cut.
        Stats update(const std::vector<vsg::ref_ptr<MainViewVisibility>>& nodes,
            const RenderCore::FrameView& view, bool occlusion)
        {
            Stats stats;
            const auto& projection = view.current.projection;
            const glm::dmat4 clip = glm::dmat4(projection.matrix) * glm::dmat4(view.current.view);
            // Reciprocal W depth requires an ordinary perspective projection.
            const auto& p = projection.matrix;
            occlusion = occlusion && !view.clipPlane && std::abs(p[2][3] + 1.f) < 1e-6f
                && p[0][3] == 0.f && p[1][3] == 0.f && p[3][3] == 0.f
                && p[0][2] == 0.f && p[1][2] == 0.f;
            if (occlusion && !mDepth)
            {
                mDepth.reset(MaskedOcclusionCulling::Create());
                if (mDepth) mDepth->SetResolution(384, 216);
            }
            occlusion = occlusion && bool(mDepth);
            if (occlusion)
            {
                mDepth->ClearBuffer();
                mDepth->SetNearClipPlane(0.001f);
                mVertices.clear();
                mIndices.clear();
                // Bounded work. Dropped triangles only reduce culling, never
                // replace geometry by unsafe bounding-box occluders.
                for (const auto& node : nodes)
                {
                    if (!node->bounded || projectVisibilityBounds(*node, clip).outside) continue;
                    for (const auto& draw : node->occluders)
                    {
                        const auto transform = clip * draw.transform;
                        const auto& indices = draw.mesh->indices;
                        const auto& vertices = draw.mesh->positions;
                        const std::size_t end = std::min(indices.size(),
                            std::size_t(draw.surface.firstIndex) + draw.surface.indexCount);
                        for (std::size_t i=draw.surface.firstIndex; i+2<end && stats.examined<32768; i+=3)
                        {
                            ++stats.examined;
                            glm::vec4 triangle[3];
                            bool safe = true;
                            for (unsigned j=0; j<3; ++j)
                            {
                                if (indices[i+j] >= vertices.size()) { safe=false; break; }
                                const auto c = transform * glm::dvec4(vertices[indices[i+j]], 1.0);
                                // Skip clipped triangles rather than reconstructing
                                // coverage at a clip boundary with a different rule.
                                if (!std::isfinite(c.x) || !std::isfinite(c.y) || !std::isfinite(c.z)
                                    || !std::isfinite(c.w) || c.w <= 0.001 || c.z < 0.0 || c.z > c.w
                                    || std::abs(c.x) > c.w || std::abs(c.y) > c.w) { safe=false; break; }
                                triangle[j] = glm::vec4(c);
                            }
                            if (!safe) continue;
                            const glm::vec2 a = glm::vec2(triangle[0])/triangle[0].w;
                            const glm::vec2 b = glm::vec2(triangle[1])/triangle[1].w;
                            const glm::vec2 c = glm::vec2(triangle[2])/triangle[2].w;
                            // Vulkan signed area is NEGATIVE of the shoelace
                            // sum for a positive-height (down-Y) viewport.
                            const float area = (b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x);
                            if (!std::isfinite(area) || std::abs(area) < 1e-10f) continue;
                            const bool front = draw.winding == RenderCore::FrontFaceWinding::Clockwise
                                ? area > 0.f : area < 0.f;
                            if ((draw.cull == RenderCore::CullMode::Back && !front)
                                || (draw.cull == RenderCore::CullMode::Front && front)) continue;
                            // Move software depth away from the camera without
                            // changing projected XY; rasterization remains conservative.
                            for (auto& vertex : triangle) vertex *= 1.001f;
                            for (const auto& vertex : triangle)
                            {
                                mIndices.push_back(static_cast<unsigned>(mVertices.size()));
                                mVertices.push_back(vertex);
                            }
                            ++stats.triangles;
                        }
                    }
                }
                if (!mVertices.empty())
                    mDepth->RenderTriangles(&mVertices.front().x, mIndices.data(),
                        static_cast<int>(mIndices.size()/3), nullptr, MaskedOcclusionCulling::BACKFACE_NONE);
            }
            for (const auto& node : nodes)
            {
                ++stats.candidates;
                const auto bounds = projectVisibilityBounds(*node, clip);
                node->visible = !bounds.outside;
                if (!node->visible) { ++stats.frustum; continue; }
                if (occlusion && !node->terrain && bounds.queryable
                    && mDepth->TestRect(bounds.left, bounds.top, bounds.right, bounds.bottom, bounds.nearestW)
                        == MaskedOcclusionCulling::OCCLUDED)
                { node->visible = false; ++stats.occluded; }
            }
            return stats;
        }
    };
}
#endif
