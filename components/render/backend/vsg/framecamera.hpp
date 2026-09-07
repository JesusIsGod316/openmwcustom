#ifndef OPENMW_COMPONENTS_RENDER_BACKEND_VSG_FRAMECAMERA_H
#define OPENMW_COMPONENTS_RENDER_BACKEND_VSG_FRAMECAMERA_H

#include <components/rendercore/framerenderstate.hpp>

#include <vsg/app/Camera.h>
#include <vsg/core/Inherit.h>
#include <vsg/maths/mat4.h>

#include <cstdint>

namespace RenderVsg
{
    [[nodiscard]] inline vsg::dmat4 toVsgMatrix(const glm::mat4& source) noexcept
    {
        vsg::dmat4 result;
        for (std::uint32_t column = 0; column < 4; ++column)
        {
            for (std::uint32_t row = 0; row < 4; ++row)
                result(column, row) = static_cast<double>(source[column][row]);
        }
        return result;
    }

    [[nodiscard]] inline vsg::dmat4 toVsgMatrix(const glm::dmat4& source) noexcept
    {
        vsg::dmat4 result;
        for (std::uint32_t column = 0; column < 4; ++column)
        {
            for (std::uint32_t row = 0; row < 4; ++row)
                result(column, row) = source[column][row];
        }
        return result;
    }

    // VSG's parameterized Perspective/LookAt types cannot preserve an already
    // authored projection or view matrix exactly. These mutable objects carry
    // the immutable FrameRenderState matrices across the backend boundary
    // without decomposition, convention conversion, or precision loss beyond
    // the producer's deliberate float matrix representation.
    class FrameProjectionMatrix final : public vsg::Inherit<vsg::ProjectionMatrix, FrameProjectionMatrix>
    {
    public:
        FrameProjectionMatrix() = default;
        explicit FrameProjectionMatrix(const glm::mat4& value)
            : matrix(toVsgMatrix(value))
        {
        }

        [[nodiscard]] vsg::dmat4 transform() const override { return matrix; }
        void set(const glm::mat4& value) noexcept { matrix = toVsgMatrix(value); }

        vsg::dmat4 matrix;
    };

    class FrameViewMatrix final : public vsg::Inherit<vsg::ViewMatrix, FrameViewMatrix>
    {
    public:
        FrameViewMatrix() = default;
        explicit FrameViewMatrix(const glm::mat4& value)
            : matrix(toVsgMatrix(value))
        {
        }

        [[nodiscard]] vsg::dmat4 transform(const vsg::dvec3& offset = {}) const override
        {
            if (offset == origin)
                return matrix;
            return matrix * vsg::translate(origin - offset);
        }
        void set(const glm::mat4& value) noexcept { matrix = toVsgMatrix(value); }

        vsg::dmat4 matrix;
    };

    struct FrameCameraObjects
    {
        vsg::ref_ptr<FrameProjectionMatrix> projection;
        vsg::ref_ptr<FrameViewMatrix> view;
        vsg::ref_ptr<vsg::Camera> camera;

        [[nodiscard]] static FrameCameraObjects create(const RenderCore::FrameView& frameView)
        {
            FrameCameraObjects result;
            result.projection = FrameProjectionMatrix::create(frameView.current.projection.matrix);
            result.view = FrameViewMatrix::create(frameView.current.view);
            result.camera = vsg::Camera::create(result.projection, result.view,
                vsg::ViewportState::create(
                    VkExtent2D{ frameView.extent.width, frameView.extent.height }));
            return result;
        }

        void update(const RenderCore::FrameView& frameView) noexcept
        {
            projection->set(frameView.current.projection.matrix);
            view->set(frameView.current.view);
            camera->viewportState->set(0, 0, frameView.extent.width, frameView.extent.height);
        }
    };
}

#endif
