#include "groundcoverquery.hpp"

#include "../mwworld/groundcoverstore.hpp"

#include <components/esm3/loadland.hpp>
#include <components/esm3/readerscache.hpp>

#include <cmath>
#include <map>
#include <utility>

namespace MWRender
{
    namespace
    {
        class DensityCalculator
        {
        public:
            explicit DensityCalculator(float density)
                : mDensity(density)
            {
            }

            [[nodiscard]] bool isInstanceEnabled()
            {
                if (mDensity >= 1.f)
                    return true;
                mCurrentGroundcover += mDensity;
                if (mCurrentGroundcover < 1.f)
                    return false;
                mCurrentGroundcover -= 1.f;
                return true;
            }

            void reset() noexcept { mCurrentGroundcover = 0.f; }

        private:
            float mCurrentGroundcover = 0.f;
            float mDensity = 0.f;
        };

        [[nodiscard]] bool isInChunkBorders(
            const ESM::CellRef& ref, float minX, float minY, float maxX, float maxY) noexcept
        {
            if (maxX - minX >= 1.f && maxY - minY >= 1.f)
                return true;

            const float cellX = ref.mPos.pos[0] / ESM::Land::REAL_SIZE;
            const float cellY = ref.mPos.pos[1] / ESM::Land::REAL_SIZE;
            if ((minX > std::floor(minX) && cellX < minX)
                || (minY > std::floor(minY) && cellY < minY)
                || (maxX < std::ceil(maxX) && cellX >= maxX)
                || (maxY < std::ceil(maxY) && cellY >= maxY))
                return false;
            return true;
        }
    }

    GroundcoverInstanceMap collectGroundcoverInstances(const MWWorld::GroundcoverStore& store,
        float density, float size, float centerX, float centerY)
    {
        GroundcoverInstanceMap instances;
        if (density <= 0.f || size <= 0.f)
            return instances;

        const float minX = centerX - size / 2.f;
        const float minY = centerY - size / 2.f;
        const float maxX = centerX + size / 2.f;
        const float maxY = centerY + size / 2.f;
        const int startCellX = static_cast<int>(std::floor(minX));
        const int startCellY = static_cast<int>(std::floor(minY));

        DensityCalculator calculator(density);
        ESM::ReadersCache readers;
        for (int cellX = startCellX; static_cast<float>(cellX) < static_cast<float>(startCellX) + size; ++cellX)
        {
            for (int cellY = startCellY; static_cast<float>(cellY) < static_cast<float>(startCellY) + size; ++cellY)
            {
                ESM::Cell cell;
                store.initCell(cell, cellX, cellY);
                if (cell.mContextList.empty())
                    continue;

                calculator.reset();
                std::map<ESM::RefNum, ESM::CellRef> refs;
                for (std::size_t i = 0; i < cell.mContextList.size(); ++i)
                {
                    const std::size_t index = static_cast<std::size_t>(cell.mContextList[i].index);
                    const ESM::ReadersCache::BusyItem reader = readers.get(index);
                    cell.restore(*reader, i);
                    ESM::CellRef ref;
                    bool deleted = false;
                    while (cell.getNextRef(*reader, ref, deleted))
                    {
                        if (!deleted && !refs.contains(ref.mRefNum) && !calculator.isInstanceEnabled())
                            deleted = true;
                        if (!deleted && !isInChunkBorders(ref, minX, minY, maxX, maxY))
                            deleted = true;

                        if (deleted)
                        {
                            refs.erase(ref.mRefNum);
                            continue;
                        }
                        refs.insert_or_assign(ref.mRefNum, std::move(ref));
                    }
                }

                for (auto& [refNum, cellRef] : refs)
                {
                    const VFS::Path::NormalizedView model = store.getGroundcoverModel(cellRef.mRefID);
                    if (model.empty())
                        continue;
                    auto it = instances.find(model);
                    if (it == instances.end())
                        it = instances.emplace_hint(it, VFS::Path::Normalized(model), std::vector<GroundcoverEntry>());
                    it->second.emplace_back(std::move(cellRef));
                }
            }
        }
        return instances;
    }
}
