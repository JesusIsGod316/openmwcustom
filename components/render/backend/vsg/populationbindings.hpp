#ifndef OPENMW_COMPONENTS_RENDER_VSG_POPULATIONBINDINGS_H
#define OPENMW_COMPONENTS_RENDER_VSG_POPULATIONBINDINGS_H

#include "staticworldplan.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace RenderVsg
{
    // A placement keeps its GPU row for this resident's entire lifetime. A
    // producer may reorder its payload without reassigning another object's
    // row. Cardinality/identity changes deliberately require a new resident;
    // the old graph and buffers then follow the existing GPU-fence retirement.
    // This is a binding to RenderWorld data, not another owning scene mirror.
    class PopulationBindings
    {
    public:
        struct Change
        {
            std::size_t slot;
            RenderCore::PopulationInstanceRecord value;
        };
        struct Update
        {
            std::uint64_t serial = 0;
            const PopulationBindings* owner = nullptr;
            std::vector<Change> changes;
        };

        explicit PopulationBindings(std::span<const RenderCore::PopulationInstanceRecord> placements)
            : mRows(placements.begin(), placements.end())
        {
            mSlots.reserve(mRows.size());
            mValid = !mRows.empty();
            for (std::size_t slot = 0; slot < mRows.size(); ++slot)
                if (mRows[slot].sourceIdentity.empty()
                    || !mSlots.emplace(mRows[slot].sourceIdentity, slot).second)
                    mValid = false;
        }

        [[nodiscard]] bool valid() const noexcept { return mValid; }
        [[nodiscard]] std::span<const RenderCore::PopulationInstanceRecord> rows() const noexcept { return mRows; }
        [[nodiscard]] std::optional<Update> prepare(
            std::span<const RenderCore::PopulationInstanceRecord> placements) const
        {
            if (!mValid || mSerial == std::numeric_limits<std::uint64_t>::max()
                || placements.size() != mRows.size()) return std::nullopt;
            Update update{mSerial, this, {}};
            std::vector<bool> seen(mRows.size());
            for (const auto& placement : placements)
            {
                const auto found = mSlots.find(placement.sourceIdentity);
                if (found == mSlots.end() || seen[found->second]) return std::nullopt;
                const auto slot = found->second;
                seen[slot] = true;
                const auto& previous = mRows[slot];
                // These fields also own traversal masks/material behavior. A
                // transform upload cannot safely change that graph contract.
                if (previous.semanticFlags != placement.semanticFlags
                    || previous.lightingEnabled != placement.lightingEnabled) return std::nullopt;
                if (!equivalentPopulationPlacement(previous, placement))
                    update.changes.push_back({slot, placement});
            }
            std::sort(update.changes.begin(), update.changes.end(),
                [](const Change& left, const Change& right) { return left.slot < right.slot; });
            return update;
        }

        [[nodiscard]] bool accepts(const Update& update) const noexcept
        {
            if (!mValid || mSerial == std::numeric_limits<std::uint64_t>::max()
                || update.owner != this || update.serial != mSerial) return false;
            std::optional<std::size_t> previous;
            for (const auto& change : update.changes)
            {
                if (change.slot >= mRows.size() || (previous && change.slot <= *previous)) return false;
                const auto& row = mRows[change.slot];
                if (row.sourceIdentity != change.value.sourceIdentity || row.semanticFlags != change.value.semanticFlags
                    || row.lightingEnabled != change.value.lightingEnabled) return false;
                previous = change.slot;
            }
            return true;
        }
        [[nodiscard]] bool publish(const Update& update) noexcept
        {
            if (!accepts(update)) return false;
            for (const auto& change : update.changes)
            {
                // Identities never change here, so publication has no string
                // allocation after residency has accepted the transaction.
                auto& row = mRows[change.slot];
                row.transform = change.value.transform;
                row.localBounds = change.value.localBounds;
                row.lod = change.value.lod;
            }
            ++mSerial;
            return true;
        }

    private:
        std::vector<RenderCore::PopulationInstanceRecord> mRows;
        std::unordered_map<std::string, std::size_t> mSlots;
        std::uint64_t mSerial = 1;
        bool mValid = false;
    };
}
#endif
