#include "staticassetconformance.hpp"

#include <vsg/all.h>

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_inverse.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace RenderVsg
{
    namespace
    {
        [[nodiscard]] glm::dmat4 toGlm(const vsg::dmat4& source) noexcept
        {
            glm::dmat4 result(1.0);
            for (glm::length_t column = 0; column < 4; ++column)
            {
                for (glm::length_t row = 0; row < 4; ++row)
                    result[column][row]
                        = source(static_cast<unsigned int>(column), static_cast<unsigned int>(row));
            }
            return result;
        }

        [[nodiscard]] vsg::dmat4 toVsg(const glm::dmat4& source) noexcept
        {
            vsg::dmat4 result;
            for (glm::length_t column = 0; column < 4; ++column)
            {
                for (glm::length_t row = 0; row < 4; ++row)
                    result(static_cast<unsigned int>(column), static_cast<unsigned int>(row)) = source[column][row];
            }
            return result;
        }

        [[nodiscard]] bool normalizeSafe(glm::dvec3& value, double epsilon) noexcept
        {
            const double length = glm::length(value);
            if (!std::isfinite(length) || length <= epsilon)
                return false;
            value /= length;
            return true;
        }

        struct BillboardBoundary
        {
            RenderCore::ModelBillboardMode mode = RenderCore::ModelBillboardMode::RigidFaceCamera;
            glm::dmat4 parentWorld{ 1.0 };
            glm::dmat4 billboardLocal{ 1.0 };
            glm::dmat4 descendantLocal{ 1.0 };
        };

        enum class BillboardBoundaryStatus
        {
            None,
            Present,
            Nested,
            Invalid,
        };

        [[nodiscard]] BillboardBoundaryStatus deriveBillboardBoundary(const RenderCore::ModelPayload& payload,
            const StaticDrawPlan& draw, BillboardBoundary& result) noexcept
        {
            if (!draw.node.valid() || draw.node.value() >= payload.nodes.size())
                return BillboardBoundaryStatus::Invalid;

            std::vector<RenderCore::ModelNodeIndex> path;
            RenderCore::ModelNodeIndex current = draw.node;
            for (std::size_t guard = 0; current.valid() && guard <= payload.nodes.size(); ++guard)
            {
                if (current.value() >= payload.nodes.size())
                    return BillboardBoundaryStatus::Invalid;
                path.push_back(current);
                current = payload.nodes[current.value()].parent;
            }
            if (current.valid())
                return BillboardBoundaryStatus::Invalid;
            std::reverse(path.begin(), path.end());

            glm::dmat4 authoredWorld(1.0);
            bool active = false;
            for (const RenderCore::ModelNodeIndex index : path)
            {
                const RenderCore::ModelNodeRecord& node = payload.nodes[index.value()];
                const glm::dmat4 local(node.localTransform);
                if (node.billboard)
                {
                    if (active)
                        return BillboardBoundaryStatus::Nested;
                    active = true;
                    result.mode = *node.billboard;
                    result.parentWorld = authoredWorld;
                    result.billboardLocal = local;
                    result.descendantLocal = glm::dmat4(1.0);
                }
                else if (active)
                    result.descendantLocal *= local;

                authoredWorld *= local;
            }

            if (active != draw.billboard.has_value())
                return BillboardBoundaryStatus::Invalid;
            if (active && draw.billboard != result.mode)
                return BillboardBoundaryStatus::Invalid;
            return active ? BillboardBoundaryStatus::Present : BillboardBoundaryStatus::None;
        }

        [[nodiscard]] double uniformScaleMagnitude(const glm::dmat4& matrix) noexcept
        {
            const glm::dvec3 x(matrix[0]);
            const glm::dvec3 y(matrix[1]);
            const glm::dvec3 z(matrix[2]);
            return std::max({ glm::length(x), glm::length(y), glm::length(z) });
        }

        [[nodiscard]] vsg::dsphere staticDrawBound(
            const RenderCore::MeshPayload& mesh, const glm::dmat4& world) noexcept
        {
            if (mesh.positions.empty())
                return {};

            glm::dvec3 minimum(std::numeric_limits<double>::max());
            glm::dvec3 maximum(std::numeric_limits<double>::lowest());
            for (const glm::vec3& position : mesh.positions)
            {
                const glm::dvec4 transformed = world * glm::dvec4(position, 1.0);
                const glm::dvec3 point(transformed);
                minimum = glm::min(minimum, point);
                maximum = glm::max(maximum, point);
            }
            const glm::dvec3 center = (minimum + maximum) * 0.5;
            const double radius = glm::length(maximum - minimum) * 0.5;
            return { center.x, center.y, center.z, radius };
        }

        [[nodiscard]] vsg::dsphere billboardDrawBound(
            const RenderCore::MeshPayload& mesh, const BillboardBoundary& boundary) noexcept
        {
            const glm::dvec3 billboardTranslation(boundary.billboardLocal[3]);
            const glm::dvec4 center4 = boundary.parentWorld * glm::dvec4(billboardTranslation, 1.0);
            const glm::dvec3 center(center4);

            double childRadius = 0.0;
            for (const glm::vec3& position : mesh.positions)
            {
                const glm::dvec4 child = boundary.descendantLocal * glm::dvec4(position, 1.0);
                childRadius = std::max(childRadius, glm::length(glm::dvec3(child)));
            }

            const double parentScale = uniformScaleMagnitude(boundary.parentWorld);
            const double billboardScale = uniformScaleMagnitude(boundary.billboardLocal);
            const double radius = childRadius * parentScale * billboardScale;
            return { center.x, center.y, center.z, radius };
        }

        class LegacyBillboardTransform final : public vsg::Inherit<vsg::Transform, LegacyBillboardTransform>
        {
        public:
            LegacyBillboardTransform(const glm::dmat4& authoredLocal, RenderCore::ModelBillboardMode mode)
                : mAuthoredLocal(authoredLocal)
                , mMode(mode)
            {
            }

            vsg::dmat4 transform(const vsg::dmat4& modelView) const override
            {
                const glm::dmat4 mv = toGlm(modelView);
                const glm::dmat4 inverseMv = glm::inverse(mv);

                StaticBillboardViewFrame frame;
                frame.eye = glm::dvec3(inverseMv * glm::dvec4(0.0, 0.0, 0.0, 1.0));
                frame.look = glm::dvec3(inverseMv * glm::dvec4(0.0, 0.0, -1.0, 0.0));
                frame.up = glm::dvec3(inverseMv * glm::dvec4(0.0, 1.0, 0.0, 0.0));

                return modelView * toVsg(evaluateLegacyBillboardLocal(mAuthoredLocal, mMode, frame));
            }

        private:
            glm::dmat4 mAuthoredLocal{ 1.0 };
            RenderCore::ModelBillboardMode mMode = RenderCore::ModelBillboardMode::RigidFaceCamera;
        };

        [[nodiscard]] vsg::ref_ptr<vsg::Node> unwrapRawDrawNode(vsg::ref_ptr<vsg::Node> node) noexcept
        {
            if (auto* depthSorted = dynamic_cast<vsg::DepthSorted*>(node.get()))
                return depthSorted->child;
            if (auto* layer = dynamic_cast<vsg::Layer*>(node.get()))
                return layer->child;
            return node;
        }
    }

    glm::dmat4 evaluateLegacyBillboardLocal(const glm::dmat4& authoredLocal,
        RenderCore::ModelBillboardMode mode, const StaticBillboardViewFrame& frame) noexcept
    {
        const glm::dmat3 linear(authoredLocal);
        const double determinant = glm::determinant(linear);
        if (!std::isfinite(determinant))
            return authoredLocal;

        const double scale = std::cbrt(determinant);
        if (!std::isfinite(scale) || std::abs(scale) <= 1e-12)
            return authoredLocal;

        const glm::dmat3 baseRotation = linear / scale;
        const glm::dmat3 inverseBase = glm::inverse(baseRotation);
        glm::dmat3 dynamicRotation(1.0);

        if (mode == RenderCore::ModelBillboardMode::AlwaysFaceCamera
            || mode == RenderCore::ModelBillboardMode::RigidFaceCamera)
        {
            glm::dvec3 relForward = inverseBase * frame.look;
            glm::dvec3 relUp = inverseBase * frame.up;
            if (normalizeSafe(relForward, 1e-12) && normalizeSafe(relUp, 1e-12))
            {
                glm::dvec3 relRight = glm::cross(relUp, relForward);
                if (normalizeSafe(relRight, 1e-12))
                {
                    relUp = glm::cross(relForward, relRight);
                    if (normalizeSafe(relUp, 1e-12))
                    {
                        if (mode == RenderCore::ModelBillboardMode::AlwaysFaceCamera)
                        {
                            const double norm
                                = std::sqrt(relUp.y * relUp.y + relRight.y * relRight.y);
                            if (norm > 1e-6)
                            {
                                const double cosTheta = relUp.y / norm;
                                const double sinTheta = -relRight.y / norm;
                                dynamicRotation[0] = glm::dvec3(-relRight.x * cosTheta - relUp.x * sinTheta,
                                    -relRight.y * cosTheta - relUp.y * sinTheta,
                                    -relRight.z * cosTheta - relUp.z * sinTheta);
                                dynamicRotation[1] = glm::dvec3(relUp.x * cosTheta - relRight.x * sinTheta,
                                    relUp.y * cosTheta - relRight.y * sinTheta,
                                    relUp.z * cosTheta - relRight.z * sinTheta);
                                dynamicRotation[2] = -relForward;
                            }
                        }
                        else
                        {
                            dynamicRotation[0] = relRight;
                            dynamicRotation[1] = relUp;
                            dynamicRotation[2] = relForward;
                        }
                    }
                }
            }
        }
        else if (mode == RenderCore::ModelBillboardMode::RotateAboutUp
            || mode == RenderCore::ModelBillboardMode::RotateAboutUpBethesda)
        {
            const glm::dvec3 translation(authoredLocal[3]);
            const glm::dvec3 relDelta = inverseBase * (frame.eye - translation);
            const double norm = std::sqrt(relDelta.x * relDelta.x + relDelta.z * relDelta.z);
            if (norm > 1e-12)
            {
                const double xNorm = relDelta.x / norm;
                const double zNorm = relDelta.z / norm;
                dynamicRotation[0] = glm::dvec3(zNorm, 0.0, -xNorm);
                dynamicRotation[1] = glm::dvec3(0.0, 1.0, 0.0);
                dynamicRotation[2] = glm::dvec3(xNorm, 0.0, zNorm);
            }
        }

        const glm::dmat3 realizedRotation = baseRotation * dynamicRotation;
        glm::dmat4 result(1.0);
        result[0] = glm::dvec4(realizedRotation[0] * scale, 0.0);
        result[1] = glm::dvec4(realizedRotation[1] * scale, 0.0);
        result[2] = glm::dvec4(realizedRotation[2] * scale, 0.0);
        result[3] = authoredLocal[3];
        return result;
    }

    std::vector<vsg::ref_ptr<vsg::Bin>> createStaticConformanceBins()
    {
        std::vector<vsg::ref_ptr<vsg::Bin>> bins;
        bins.push_back(vsg::Bin::create(StaticTraversalBinNumber, vsg::Bin::NO_SORT));
        bins.push_back(vsg::Bin::create(StaticBackToFrontBinNumber, vsg::Bin::DESCENDING));
        return bins;
    }

    StaticRealizationResult realizeStaticAssetConformant(const RenderCore::RenderWorld& world,
        RenderCore::ModelHandle modelHandle, const StaticAssetPlan& plan, const StaticTextureResolver& textureResolver,
        vsg::ref_ptr<vsg::SharedObjects> sharedObjects)
    {
        StaticAssetRealizer realizer(std::move(sharedObjects));
        StaticRealizationResult result = realizer.realize(world, plan, textureResolver);
        if (!result.valid())
            return result;

        const RenderCore::ModelRecord* model = world.get(modelHandle);
        if (!model || !model->payload || result.root->children.size() != plan.draws.size())
        {
            result.root = {};
            result.diagnostics.emplace_back(
                "Conformant routing requires the published model and one raw VSG child per static draw");
            return result;
        }

        auto routedRoot = vsg::Group::create();
        result.stats.sortedDrawCount = 0u;

        for (std::size_t i = 0; i < plan.draws.size(); ++i)
        {
            const StaticDrawPlan& draw = plan.draws[i];
            const RenderCore::MeshRecord* mesh = world.get(draw.mesh);
            if (!mesh || !mesh->payload)
            {
                result.root = {};
                result.diagnostics.emplace_back("Conformant routing lost a published mesh resource");
                return result;
            }

            vsg::ref_ptr<vsg::Node> rawNode = unwrapRawDrawNode(result.root->children[i]);
            auto* rawTransform = dynamic_cast<vsg::MatrixTransform*>(rawNode.get());
            if (!rawTransform)
            {
                result.root = {};
                result.diagnostics.emplace_back(
                    "Raw static realization shape changed; expected one MatrixTransform per draw");
                return result;
            }

            BillboardBoundary boundary;
            const BillboardBoundaryStatus boundaryStatus = deriveBillboardBoundary(*model->payload, draw, boundary);
            if (boundaryStatus == BillboardBoundaryStatus::Nested)
            {
                result.root = {};
                result.diagnostics.emplace_back(
                    "Nested billboard transforms require an explicit compatibility path and are not flattened silently");
                return result;
            }
            if (boundaryStatus == BillboardBoundaryStatus::Invalid)
            {
                result.root = {};
                result.diagnostics.emplace_back(
                    "Static billboard ancestry does not match the neutral draw plan");
                return result;
            }

            vsg::ref_ptr<vsg::Node> drawNode = rawNode;
            vsg::dsphere bound = staticDrawBound(*mesh->payload, glm::dmat4(draw.worldTransform));
            if (boundaryStatus == BillboardBoundaryStatus::Present)
            {
                auto parent = vsg::MatrixTransform::create(toVsg(boundary.parentWorld));
                auto billboard = LegacyBillboardTransform::create(boundary.billboardLocal, boundary.mode);
                auto descendant = vsg::MatrixTransform::create(toVsg(boundary.descendantLocal));
                for (const vsg::ref_ptr<vsg::Node>& child : rawTransform->children)
                    descendant->addChild(child);
                billboard->addChild(descendant);
                parent->addChild(billboard);
                drawNode = parent;
                bound = billboardDrawBound(*mesh->payload, boundary);
            }

            switch (draw.sortPolicy)
            {
                case StaticDrawSortPolicy::Default:
                    routedRoot->addChild(drawNode);
                    break;
                case StaticDrawSortPolicy::Traversal:
                    routedRoot->addChild(
                        vsg::Layer::create(StaticTraversalBinNumber, static_cast<double>(i), drawNode));
                    break;
                case StaticDrawSortPolicy::BackToFront:
                    routedRoot->addChild(vsg::DepthSorted::create(StaticBackToFrontBinNumber, bound, drawNode));
                    ++result.stats.sortedDrawCount;
                    break;
            }
        }

        result.root = routedRoot;
        return result;
    }
}
