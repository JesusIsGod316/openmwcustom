#include "v4semanticsource.hpp"

#include "camera.hpp"

#include "../mwworld/cell.hpp"
#include "../mwworld/cellstore.hpp"
#include "../mwworld/class.hpp"
#include "../mwworld/ptr.hpp"

#include <components/misc/convert.hpp>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <cstddef>
#include <string>

namespace MWRender
{
    namespace
    {
        [[nodiscard]] glm::mat4 toGlmView(const osg::Matrixf& source) noexcept
        {
            // OSG uses row vectors. Transposing its mathematical matrix gives
            // the equivalent GLM column-vector transform; GLM's [column][row]
            // indexing therefore reads OSG at (column,row).
            glm::mat4 result(1.0f);
            for (std::size_t column = 0; column < 4; ++column)
            {
                for (std::size_t row = 0; row < 4; ++row)
                    result[column][row] = source(column, row);
            }
            return result;
        }

        [[nodiscard]] std::string cellIdentity(const MWWorld::Cell& cell)
        {
            std::string result = "world:" + cell.getWorldSpace().serializeText();
            if (cell.isExterior())
            {
                result += "/exterior:" + std::to_string(cell.getGridX()) + "," + std::to_string(cell.getGridY());
                return result;
            }
            return result + "/interior:" + cell.getId().serializeText();
        }
    }

    std::optional<RenderCore::ActiveCellSource> makeV4ActiveCellSource(const MWWorld::CellStore& cell)
    {
        const MWWorld::Cell* source = cell.getCell();
        if (!source || cell.getState() != MWWorld::CellStore::State_Loaded)
            return std::nullopt;

        RenderCore::ActiveCellSource result;
        result.identity = cellIdentity(*source);
        result.worldspaceIdentity = source->getWorldSpace().serializeText();
        return result;
    }

    std::optional<RenderCore::StaticInstanceSource> makeV4StaticInstanceSource(
        const MWWorld::Ptr& ptr, RenderCore::ModelHandle model, RenderCore::AxisAlignedBounds localBounds)
    {
        if (ptr.isEmpty() || !ptr.getCell() || !model.valid() || !ptr.getRefData().isEnabled()
            || ptr.getClass().isActor() || ptr.getClass().useAnim())
            return std::nullopt;
        const ESM::RefNum refNum = ptr.getCellRef().getRefNum();
        const std::optional<RenderCore::ActiveCellSource> cell = makeV4ActiveCellSource(*ptr.getCell());
        if (!refNum.isSet() || !cell)
            return std::nullopt;

        const ESM::Position& position = ptr.getRefData().getPosition();
        const osg::Quat rotation = Misc::Convert::makeOsgQuat(position);
        const float scale = ptr.getCellRef().getScale();
        RenderCore::StaticInstanceSource result;
        result.identity = "ref:" + refNum.toString();
        result.cellIdentity = cell->identity;
        result.model = model;
        result.transform.translation = { position.pos[0], position.pos[1], position.pos[2] };
        result.transform.rotation = { rotation.w(), rotation.x(), rotation.y(), rotation.z() };
        result.transform.scale = { scale, scale, scale };
        result.localBounds = localBounds;
        return result;
    }

    std::optional<RenderCore::CameraState> makeV4MainCameraState(const Camera& camera,
        RenderCore::Extent2D extent, double verticalFieldOfViewDegrees, double nearPlane, double farPlane)
    {
        if (!extent.valid() || !std::isfinite(verticalFieldOfViewDegrees) || verticalFieldOfViewDegrees <= 0.0
            || verticalFieldOfViewDegrees >= 180.0 || !std::isfinite(nearPlane) || !std::isfinite(farPlane)
            || nearPlane <= 0.0 || farPlane <= nearPlane)
            return std::nullopt;

        RenderCore::CameraState result;
        result.view = toGlmView(camera.getViewMatrix());
        const glm::mat4 cameraWorld = glm::inverse(result.view);
        result.worldPosition = glm::dvec3(cameraWorld[3]);
        result.worldOrientation = glm::normalize(glm::quat_cast(glm::mat3(cameraWorld)));

        constexpr double pi = 3.14159265358979323846;
        const float radians = static_cast<float>(verticalFieldOfViewDegrees * pi / 180.0);
        const float aspect = static_cast<float>(extent.width) / static_cast<float>(extent.height);
        // Swapping far/near is the finite reversed-Z RH_ZO form. Vulkan's
        // framebuffer convention then requires the explicit Y inversion.
        result.projection.matrix
            = glm::perspectiveRH_ZO(radians, aspect, static_cast<float>(farPlane), static_cast<float>(nearPlane));
        result.projection.matrix[1][1] *= -1.0f;
        result.projection.depthRange = RenderCore::ClipDepthRange::ZeroToOne;
        result.projection.depthDirection = RenderCore::DepthDirection::Reversed;
        result.projection.yDirection = RenderCore::ClipYDirection::Down;
        result.projection.nearPlane = nearPlane;
        result.projection.farPlane = farPlane;
        return result;
    }
}
