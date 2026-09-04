#ifndef OPENMW_COMPONENTS_DEBUG_V36CONTROLLERTRACE_H
#define OPENMW_COMPONENTS_DEBUG_V36CONTROLLERTRACE_H

#include <algorithm>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <string_view>

#include "v3diagnostics.hpp"
#include "v3hitchtelemetry.hpp"

namespace Debug::V36ControllerTrace
{
    enum class Phase { KeyframeLookup, NodeMap, ControllerClone, SourceAssign };

    inline V3Diagnostics::CsvWriter& writer()
    {
        static V3Diagnostics::CsvWriter writer("OPENMW_V36_CONTROLLER_FILE",
            "frame,epoch_ms,keyframe,base_model,total_ms,keyframe_lookup_ms,node_map_ms,controller_clone_ms,"
            "source_assign_ms,other_ms,controller_count,controller_types");
        return writer;
    }

    class Scope
    {
    public:
        Scope(std::string_view keyframe, std::string_view baseModel)
            : mEnabled(writer().enabled())
            , mKeyframe(mEnabled ? keyframe : std::string_view{})
            , mBaseModel(mEnabled ? baseModel : std::string_view{})
            , mStart(mEnabled ? V3Diagnostics::Clock::now() : V3Diagnostics::Clock::time_point{})
        {
        }

        ~Scope()
        {
            if (!mEnabled)
                return;
            const double total = V3Diagnostics::elapsedMs(mStart);
            if (total < 0.25)
                return;
            const double accounted = mKeyframeMs + mNodeMapMs + mCloneMs + mAssignMs;
            std::ostringstream types;
            bool first = true;
            for (const auto& [name, count] : mTypes)
            {
                if (!first)
                    types << ';';
                first = false;
                types << name << ':' << count;
            }
            std::ostringstream row;
            row << V3HitchTelemetry::currentFrame() << ',' << V3Diagnostics::epochMs() << ','
                << V3Diagnostics::csvQuote(mKeyframe) << ',' << V3Diagnostics::csvQuote(mBaseModel) << ','
                << std::fixed << std::setprecision(4) << total << ',' << mKeyframeMs << ',' << mNodeMapMs << ','
                << mCloneMs << ',' << mAssignMs << ',' << std::max(0.0, total - accounted) << ','
                << mControllerCount << ',' << V3Diagnostics::csvQuote(types.str());
            writer().writeLine(row.str());
        }

        void add(Phase phase, double ms)
        {
            switch (phase)
            {
                case Phase::KeyframeLookup: mKeyframeMs += ms; break;
                case Phase::NodeMap: mNodeMapMs += ms; break;
                case Phase::ControllerClone: mCloneMs += ms; break;
                case Phase::SourceAssign: mAssignMs += ms; break;
            }
        }

        void controller(std::string_view type)
        {
            if (!mEnabled)
                return;
            ++mControllerCount;
            ++mTypes[std::string(type)];
        }

        bool enabled() const { return mEnabled; }

    private:
        bool mEnabled;
        std::string mKeyframe;
        std::string mBaseModel;
        V3Diagnostics::Clock::time_point mStart;
        double mKeyframeMs = 0.0;
        double mNodeMapMs = 0.0;
        double mCloneMs = 0.0;
        double mAssignMs = 0.0;
        unsigned int mControllerCount = 0;
        std::map<std::string, unsigned int> mTypes;
    };

    class PhaseScope
    {
    public:
        PhaseScope(Scope& scope, Phase phase)
            : mScope(scope)
            , mPhase(phase)
            , mStart(scope.enabled() ? V3Diagnostics::Clock::now() : V3Diagnostics::Clock::time_point{})
        {
        }
        ~PhaseScope()
        {
            if (mStart != V3Diagnostics::Clock::time_point{})
                mScope.add(mPhase, V3Diagnostics::elapsedMs(mStart));
        }
    private:
        Scope& mScope;
        Phase mPhase;
        V3Diagnostics::Clock::time_point mStart;
    };
}

#endif
