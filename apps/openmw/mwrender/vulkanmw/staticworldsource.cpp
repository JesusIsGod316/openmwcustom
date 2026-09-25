#include "staticworldsource.hpp"

#include "../../mwworld/cell.hpp"
#include "../../mwworld/cellstore.hpp"
#include "../../mwworld/class.hpp"
#include "../../mwworld/ptr.hpp"

#include <components/esm/position.hpp>

#include <glm/gtc/quaternion.hpp>

#include <string>

namespace MWRender::VulkanMW
{
    namespace
    {
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

        [[nodiscard]] RenderCore::Rotation referenceRotation(const ESM::Position& position) noexcept
        {
            // Exact equivalent of the established OpenMW reference rotation
            // composition, expressed directly with GLM rather than osg::Quat.
            return glm::normalize(
                glm::angleAxis(position.rot[2], glm::vec3(0.f, 0.f, -1.f))
                * glm::angleAxis(position.rot[1], glm::vec3(0.f, -1.f, 0.f))
                * glm::angleAxis(position.rot[0], glm::vec3(-1.f, 0.f, 0.f)));
        }
    }

    std::optional<std::string> makeCellIdentity(const MWWorld::CellStore& cell)
    {
        const MWWorld::Cell* source = cell.getCell();
        if (!source)
            return std::nullopt;
        return cellIdentity(*source);
    }

    std::optional<std::string> makeReferenceIdentity(const MWWorld::Ptr& ptr)
    {
        if (ptr.isEmpty())
            return std::nullopt;
        const ESM::RefNum refNum = ptr.getCellRef().getRefNum();
        if (!refNum.isSet())
            return std::nullopt;
        return "ref:" + refNum.toString();
    }

    std::optional<RenderNative::StaticWorldCellSource> makeStaticWorldCellSource(const MWWorld::CellStore& cell)
    {
        const MWWorld::Cell* source = cell.getCell();
        if (!source || cell.getState() != MWWorld::CellStore::State_Loaded)
            return std::nullopt;

        RenderNative::StaticWorldCellSource result;
        result.cell.identity = cellIdentity(*source);
        result.cell.worldspaceIdentity = source->getWorldSpace().serializeText();
        result.exterior = source->isExterior();
        if (result.exterior)
        {
            result.gridX = source->getGridX();
            result.gridY = source->getGridY();
        }
        return result;
    }

    std::optional<RenderCore::StaticInstanceSource> makeStaticInstanceSource(
        const MWWorld::Ptr& ptr, RenderCore::ModelHandle model, RenderCore::AxisAlignedBounds localBounds)
    {
        if (ptr.isEmpty() || !ptr.getCell() || !model.valid() || !ptr.getRefData().isEnabled()
            || ptr.getClass().isActor() || ptr.getClass().useAnim())
            return std::nullopt;

        const std::optional<std::string> identity = makeReferenceIdentity(ptr);
        const std::optional<RenderNative::StaticWorldCellSource> cell = makeStaticWorldCellSource(*ptr.getCell());
        if (!identity || !cell)
            return std::nullopt;

        const ESM::Position& position = ptr.getRefData().getPosition();
        const float scale = ptr.getCellRef().getScale();

        RenderCore::StaticInstanceSource result;
        result.identity = *identity;
        result.cellIdentity = cell->cell.identity;
        result.model = model;
        result.transform.translation = { position.pos[0], position.pos[1], position.pos[2] };
        result.transform.rotation = referenceRotation(position);
        result.transform.scale = { scale, scale, scale };
        result.localBounds = localBounds;
        return result;
    }
}
