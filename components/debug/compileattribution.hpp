#ifndef OPENMW_COMPONENTS_DEBUG_COMPILEATTRIBUTION_H
#define OPENMW_COMPONENTS_DEBUG_COMPILEATTRIBUTION_H

#include "hitchtrace.hpp"
#include "v3diagnostics.hpp"

#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <new>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>

namespace Debug::CompileAttribution
{
#ifdef P9_COMPILE_TRACE_TEST_HOOKS
    inline std::atomic<unsigned> metadataAllocationFaults{ 0 };
#endif
    inline bool requested()
    {
        static const bool enabled = [] {
            const char* value = std::getenv("OPENMW_P9_COMPILE_ATTRIBUTION");
            return value && std::string_view(value) == "1";
        }();
        return enabled;
    }

    inline V3Diagnostics::CsvWriter& writer()
    {
        static V3Diagnostics::CsvWriter output("OPENMW_P9_COMPILE_ATTRIBUTION_FILE",
            "schema,frame,epoch_ms,event,context,frame_known,scheduler,set,set_generation,op,span,branch,"
            "selected_set_first_seen_frame,selected_set_age_frames,op_first_observed_frame,op_age_observed_frames,"
            "demand_first_observed_frame,last_progress_observed_frame,tracking_status,urgency,quarantine,forcing_"
            "reason,"
            "age_threshold_frames,hard_age_threshold_frames,budget_ms,predicted_ms,actual_ms,target_frame_ms,"
            "max_budget_ms,base_target_fps,elapsed_before_ms,last_handoff_ms,headroom_ms,resource_bytes,"
            "prepared_reused,required,cancelled,kind,compile_class,texture,image0,image0_revision,image0_filename,"
            "image0_known,texture_object_before,texture_object_after,queue_depth,global_oldest_age_frames,"
            "active_candidates,quarantined_candidates,metadata_allocation_failures,format_allocation_failures,"
            "registry_capacity_failures,registry_allocation_failures");
        return output;
    }

    constexpr unsigned UnknownFrame = std::numeric_limits<unsigned>::max();

    struct Observation
    {
        std::uint64_t generation = 0;
        unsigned opFirst = UnknownFrame;
        unsigned demandFirst = UnknownFrame;
        unsigned lastProgress = UnknownFrame;
        std::string_view status = "unobserved";
    };

    // Only instantiated for the explicitly requested family. This records
    // scheduler observations, not the unobserved producer's first-demand time.
    // No pointers here extend resource lifetime or claim revision validity.
    class ObservedSets
    {
    public:
        static constexpr std::size_t MaximumSets = 32768;

        Observation observe(const void* set, const void* op, unsigned frame, bool demanded) noexcept
        {
            auto found = mSets.find(set);
            if (found == mSets.end())
            {
                if (mSets.size() >= MaximumSets)
                {
                    ++mCapacityFailures;
                    return { 0, UnknownFrame, UnknownFrame, UnknownFrame, "unknown_capacity" };
                }
                try
                {
                    found = mSets
                                .emplace(set,
                                    Seen{ ++mGeneration, op, frame, demanded ? frame : UnknownFrame, UnknownFrame })
                                .first;
                }
                catch (const std::bad_alloc&)
                {
                    ++mAllocationFailures;
                    return { 0, UnknownFrame, UnknownFrame, UnknownFrame, "unknown_allocation" };
                }
            }
            Seen& seen = found->second;
            if (seen.op != op)
            {
                seen.op = op;
                seen.opFirst = frame;
            }
            if (demanded && seen.demandFirst == UnknownFrame)
                seen.demandFirst = frame;
            return { seen.generation, seen.opFirst, seen.demandFirst, seen.lastProgress, "observed" };
        }

        void progressed(const void* set, unsigned frame) noexcept
        {
            if (auto found = mSets.find(set); found != mSets.end())
                found->second.lastProgress = frame;
        }

        void forget(const void* set) noexcept { mSets.erase(set); }
        std::uint64_t capacityFailures() const noexcept { return mCapacityFailures; }
        std::uint64_t allocationFailures() const noexcept { return mAllocationFailures; }

        template <class IsLive>
        void prune(IsLive isLive)
        {
            for (auto it = mSets.begin(); it != mSets.end();)
            {
                if (!isLive(it->first))
                    it = mSets.erase(it);
                else
                    ++it;
            }
        }

    private:
        struct Seen
        {
            std::uint64_t generation;
            const void* op;
            unsigned opFirst;
            unsigned demandFirst;
            unsigned lastProgress;
        };
        std::unordered_map<const void*, Seen> mSets;
        std::uint64_t mGeneration = 0;
        std::uint64_t mCapacityFailures = 0, mAllocationFailures = 0;
    };

    struct Row
    {
        unsigned frame = 0;
        unsigned context = UnknownFrame;
        bool frameKnown = false;
        std::uintptr_t scheduler = 0;
        std::uintptr_t set = 0;
        std::uintptr_t op = 0;
        std::uint64_t span = 0;
        std::string_view event = "op";
        std::string_view branch = "p4";
        unsigned setFirst = UnknownFrame;
        unsigned setAge = 0;
        Observation observed;
        std::string_view urgency = "unknown";
        std::string_view quarantine = "unknown";
        std::string_view forcing = "unknown";
        unsigned ageThreshold = 0;
        unsigned hardAgeThreshold = 0;
        double budgetMs = 0;
        double predictedMs = 0;
        double actualMs = 0;
        double targetFrameMs = 0;
        double maxBudgetMs = 0;
        double baseTargetFps = 0;
        double elapsedBeforeMs = 0;
        double lastHandoffMs = 0;
        double headroomMs = 0;
        std::size_t resourceBytes = 0;
        bool preparedReused = false;
        bool required = false;
        bool cancelled = false;
        std::string_view kind = "unknown";
        std::string_view compileClass = "unknown";
        std::uintptr_t texture = 0;
        std::uintptr_t image0 = 0;
        unsigned image0Revision = 0;
        std::string image0Filename;
        bool image0Known = false;
        unsigned textureObjectBefore = 0;
        unsigned textureObjectAfter = 0;
        std::size_t queueDepth = 0;
        unsigned globalOldestAge = 0;
        std::size_t activeCandidates = 0;
        unsigned quarantinedCandidates = 0;
        std::uint64_t metadataFailures = 0;
        std::uint64_t formatFailures = 0;
        std::uint64_t registryCapacityFailures = 0, registryAllocationFailures = 0;
    };

    inline bool write(const Row& value)
    {
        try
        {
            auto& output = writer();
            if (!output.enabled())
                return false;
            std::ostringstream row;
            auto text = [&row](std::string_view value) { row << V3Diagnostics::csvQuote(value); };
            auto frame = [&row](unsigned value) {
                if (value != UnknownFrame)
                    row << value;
            };
            row << "1," << value.frame << ',' << V3Diagnostics::epochMs() << ',';
            text(value.event);
            row << ',' << value.context << ',' << value.frameKnown << ',' << value.scheduler << ',' << value.set << ','
                << value.observed.generation << ',' << value.op << ',' << value.span << ',';
            text(value.branch);
            row << ',';
            frame(value.setFirst);
            row << ',' << value.setAge << ',';
            frame(value.observed.opFirst);
            row << ',';
            if (value.observed.opFirst != UnknownFrame && value.frame >= value.observed.opFirst)
                row << value.frame - value.observed.opFirst;
            row << ',';
            frame(value.observed.demandFirst);
            row << ',';
            frame(value.observed.lastProgress);
            row << ',';
            text(value.observed.status);
            row << ',';
            text(value.urgency);
            row << ',';
            text(value.quarantine);
            row << ',';
            text(value.forcing);
            row << ',' << value.ageThreshold << ',' << value.hardAgeThreshold << ',' << std::fixed
                << std::setprecision(6) << value.budgetMs << ',' << value.predictedMs << ',' << value.actualMs << ','
                << value.targetFrameMs << ',' << value.maxBudgetMs << ',' << value.baseTargetFps << ','
                << value.elapsedBeforeMs << ',' << value.lastHandoffMs << ',' << value.headroomMs << ','
                << value.resourceBytes << ',' << value.preparedReused << ',' << value.required << ',' << value.cancelled
                << ',';
            text(value.kind);
            row << ',';
            text(value.compileClass);
            row << ',' << value.texture << ',' << value.image0 << ',' << value.image0Revision << ',';
            text(value.image0Filename);
            row << ',' << value.image0Known << ',' << value.textureObjectBefore << ',' << value.textureObjectAfter
                << ',' << value.queueDepth << ',' << value.globalOldestAge << ',' << value.activeCandidates << ','
                << value.quarantinedCandidates << ',' << value.metadataFailures << ',' << value.formatFailures
                << ',' << value.registryCapacityFailures << ',' << value.registryAllocationFailures;
            output.writeLine(row.str());
            return true;
        }
        catch (const std::bad_alloc&)
        {
            return false;
        }
    }
}

#endif
