#ifndef OPENMW_COMPONENTS_RENDER_NATIVE_STATICWORLDSERVICE_H
#define OPENMW_COMPONENTS_RENDER_NATIVE_STATICWORLDSERVICE_H

#include <components/rendercore/activecellproducer.hpp>
#include <components/rendercore/staticpopulationproducer.hpp>

#include <cstdint>
#include <string_view>
#include <utility>

namespace RenderNative
{
    struct StaticWorldCellSource
    {
        RenderCore::ActiveCellSource cell;
        bool exterior = false;
        std::int32_t gridX = 0;
        std::int32_t gridY = 0;
    };

    enum class StaticWorldMutationStatus : std::uint8_t
    {
        Applied,
        AlreadyCurrent,
        NotFound,
        InvalidSource,
        PublishFailed,
    };

    struct StaticWorldMutationResult
    {
        StaticWorldMutationStatus status = StaticWorldMutationStatus::InvalidSource;
        RenderCore::ActiveCellPublishStatus cellStatus = RenderCore::ActiveCellPublishStatus::AlreadyPresent;
        RenderCore::StaticPopulationPublishStatus populationStatus
            = RenderCore::StaticPopulationPublishStatus::AlreadyPresent;

        [[nodiscard]] bool accepted() const noexcept
        {
            return status == StaticWorldMutationStatus::Applied
                || status == StaticWorldMutationStatus::AlreadyCurrent
                || status == StaticWorldMutationStatus::NotFound;
        }
    };

    [[nodiscard]] inline bool acceptedStaticCellStatus(RenderCore::ActiveCellPublishStatus status) noexcept
    {
        return status == RenderCore::ActiveCellPublishStatus::Applied
            || status == RenderCore::ActiveCellPublishStatus::AlreadyPresent
            || status == RenderCore::ActiveCellPublishStatus::NotFound;
    }

    [[nodiscard]] inline bool acceptedStaticPopulationStatus(
        RenderCore::StaticPopulationPublishStatus status) noexcept
    {
        return status == RenderCore::StaticPopulationPublishStatus::Applied
            || status == RenderCore::StaticPopulationPublishStatus::AlreadyPresent;
    }

    // Phase 2 source-side static-world ownership.
    //
    // This service coordinates individually addressable interior instances with
    // data-oriented exterior population chunks. It consumes only neutral
    // RenderCore sources/handles; game objects, OSG scene nodes, VSG objects and
    // Vulkan resources never cross this boundary.
    class StaticWorldService final
    {
    public:
        StaticWorldService(RenderCore::ActiveCellProducer& cells, RenderCore::StaticPopulationProducer& populations)
            : mCells(cells)
            , mPopulations(populations)
        {
        }

        [[nodiscard]] StaticWorldMutationResult activateCell(const StaticWorldCellSource& source)
        {
            if (source.cell.identity.empty() || source.cell.worldspaceIdentity.empty())
                return { StaticWorldMutationStatus::InvalidSource };

            const RenderCore::ActiveCellPublishResult cell = mCells.addCell(source.cell);
            if (!acceptedStaticCellStatus(cell.status) || cell.status == RenderCore::ActiveCellPublishStatus::NotFound)
                return { StaticWorldMutationStatus::PublishFailed, cell.status };

            if (!source.exterior)
            {
                return { cell.status == RenderCore::ActiveCellPublishStatus::Applied
                            ? StaticWorldMutationStatus::Applied
                            : StaticWorldMutationStatus::AlreadyCurrent,
                    cell.status };
            }

            RenderCore::StaticPopulationCellSource population;
            population.identity = source.cell.identity;
            population.worldspaceIdentity = source.cell.worldspaceIdentity;
            population.gridX = source.gridX;
            population.gridY = source.gridY;
            const RenderCore::StaticPopulationPublishStatus populationStatus
                = mPopulations.addCell(std::move(population));
            if (!acceptedStaticPopulationStatus(populationStatus))
            {
                if (cell.status == RenderCore::ActiveCellPublishStatus::Applied)
                    static_cast<void>(mCells.removeCell(source.cell.identity));
                return { StaticWorldMutationStatus::PublishFailed, cell.status, populationStatus };
            }

            return { cell.status == RenderCore::ActiveCellPublishStatus::Applied
                        || populationStatus == RenderCore::StaticPopulationPublishStatus::Applied
                    ? StaticWorldMutationStatus::Applied
                    : StaticWorldMutationStatus::AlreadyCurrent,
                cell.status, populationStatus };
        }

        [[nodiscard]] StaticWorldMutationResult deactivateCell(std::string_view identity)
        {
            if (identity.empty())
                return { StaticWorldMutationStatus::InvalidSource };

            const RenderCore::StaticPopulationPublishStatus population = mPopulations.removeCell(identity);
            const RenderCore::ActiveCellPublishResult cell = mCells.removeCell(identity);
            if (!acceptedStaticPopulationStatus(population) || !acceptedStaticCellStatus(cell.status))
                return { StaticWorldMutationStatus::PublishFailed, cell.status, population };

            const bool changed = population == RenderCore::StaticPopulationPublishStatus::Applied
                || cell.status == RenderCore::ActiveCellPublishStatus::Applied;
            return { changed ? StaticWorldMutationStatus::Applied : StaticWorldMutationStatus::NotFound,
                cell.status, population };
        }

        [[nodiscard]] StaticWorldMutationResult upsertStatic(
            const RenderCore::StaticInstanceSource& source, bool exterior)
        {
            if (source.identity.empty() || source.cellIdentity.empty() || !source.model.valid())
                return { StaticWorldMutationStatus::InvalidSource };

            if (exterior)
            {
                const RenderCore::ActiveCellPublishResult retired = mCells.removeInstance(source.identity);
                if (retired.status != RenderCore::ActiveCellPublishStatus::Applied
                    && retired.status != RenderCore::ActiveCellPublishStatus::NotFound)
                    return { StaticWorldMutationStatus::PublishFailed, retired.status };

                RenderCore::StaticPopulationInstanceSource population;
                population.identity = source.identity;
                population.cellIdentity = source.cellIdentity;
                population.model = source.model;
                population.transform = source.transform;
                population.localBounds = source.localBounds;
                population.lod = source.lod;
                population.semanticFlags = source.semanticFlags;
                population.lightingEnabled = source.lightingEnabled;
                const RenderCore::StaticPopulationPublishStatus published = mPopulations.upsert(std::move(population));
                if (!acceptedStaticPopulationStatus(published))
                    return { StaticWorldMutationStatus::PublishFailed, retired.status, published };
                return { published == RenderCore::StaticPopulationPublishStatus::Applied
                            || retired.status == RenderCore::ActiveCellPublishStatus::Applied
                        ? StaticWorldMutationStatus::Applied
                        : StaticWorldMutationStatus::AlreadyCurrent,
                    retired.status, published };
            }

            const RenderCore::StaticPopulationPublishStatus retired = mPopulations.remove(source.identity);
            if (!acceptedStaticPopulationStatus(retired))
                return { StaticWorldMutationStatus::PublishFailed,
                    RenderCore::ActiveCellPublishStatus::AlreadyPresent, retired };
            const RenderCore::ActiveCellPublishResult published = mCells.upsertStaticInstance(source);
            if (!acceptedStaticCellStatus(published.status)
                || published.status == RenderCore::ActiveCellPublishStatus::NotFound)
                return { StaticWorldMutationStatus::PublishFailed, published.status, retired };
            return { published.status == RenderCore::ActiveCellPublishStatus::Applied
                        || retired == RenderCore::StaticPopulationPublishStatus::Applied
                    ? StaticWorldMutationStatus::Applied
                    : StaticWorldMutationStatus::AlreadyCurrent,
                published.status, retired };
        }

        [[nodiscard]] StaticWorldMutationResult activatePopulationCell(
            RenderCore::StaticPopulationCellSource source)
        {
            if (source.identity.empty() || source.worldspaceIdentity.empty())
                return { StaticWorldMutationStatus::InvalidSource };
            const RenderCore::StaticPopulationPublishStatus population = mPopulations.addCell(std::move(source));
            if (!acceptedStaticPopulationStatus(population))
                return { StaticWorldMutationStatus::PublishFailed,
                    RenderCore::ActiveCellPublishStatus::AlreadyPresent, population };
            return { population == RenderCore::StaticPopulationPublishStatus::Applied
                    ? StaticWorldMutationStatus::Applied
                    : StaticWorldMutationStatus::AlreadyCurrent,
                RenderCore::ActiveCellPublishStatus::AlreadyPresent, population };
        }

        [[nodiscard]] StaticWorldMutationResult deactivatePopulationCell(std::string_view identity)
        {
            if (identity.empty())
                return { StaticWorldMutationStatus::InvalidSource };
            const RenderCore::StaticPopulationPublishStatus population = mPopulations.removeCell(identity);
            if (!acceptedStaticPopulationStatus(population))
                return { StaticWorldMutationStatus::PublishFailed,
                    RenderCore::ActiveCellPublishStatus::AlreadyPresent, population };
            return { population == RenderCore::StaticPopulationPublishStatus::Applied
                    ? StaticWorldMutationStatus::Applied
                    : StaticWorldMutationStatus::NotFound,
                RenderCore::ActiveCellPublishStatus::AlreadyPresent, population };
        }

        [[nodiscard]] StaticWorldMutationResult upsertPopulation(RenderCore::StaticPopulationInstanceSource source)
        {
            if (source.identity.empty() || source.cellIdentity.empty() || !source.model.valid())
                return { StaticWorldMutationStatus::InvalidSource };
            const RenderCore::StaticPopulationPublishStatus population = mPopulations.upsert(std::move(source));
            if (!acceptedStaticPopulationStatus(population))
                return { StaticWorldMutationStatus::PublishFailed,
                    RenderCore::ActiveCellPublishStatus::AlreadyPresent, population };
            return { population == RenderCore::StaticPopulationPublishStatus::Applied
                    ? StaticWorldMutationStatus::Applied
                    : StaticWorldMutationStatus::AlreadyCurrent,
                RenderCore::ActiveCellPublishStatus::AlreadyPresent, population };
        }

        [[nodiscard]] StaticWorldMutationResult removeStatic(std::string_view identity)
        {
            if (identity.empty())
                return { StaticWorldMutationStatus::InvalidSource };

            const RenderCore::ActiveCellPublishResult instance = mCells.removeInstance(identity);
            const RenderCore::StaticPopulationPublishStatus population = mPopulations.remove(identity);
            if ((instance.status != RenderCore::ActiveCellPublishStatus::Applied
                    && instance.status != RenderCore::ActiveCellPublishStatus::NotFound)
                || !acceptedStaticPopulationStatus(population))
                return { StaticWorldMutationStatus::PublishFailed, instance.status, population };

            const bool changed = instance.status == RenderCore::ActiveCellPublishStatus::Applied
                || population == RenderCore::StaticPopulationPublishStatus::Applied;
            return { changed ? StaticWorldMutationStatus::Applied : StaticWorldMutationStatus::NotFound,
                instance.status, population };
        }

    private:
        RenderCore::ActiveCellProducer& mCells;
        RenderCore::StaticPopulationProducer& mPopulations;
    };
}

#endif
