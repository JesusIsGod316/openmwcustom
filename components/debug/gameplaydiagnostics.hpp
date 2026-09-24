#ifndef OPENMW_DEBUG_GAMEPLAYDIAGNOSTICS_H
#define OPENMW_DEBUG_GAMEPLAYDIAGNOSTICS_H

#include "runtimediagnostics.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>

// Opt-in observation only. Hooks execute on the engine frame thread; other
// threads have an inactive TLS context. No gameplay state, fences or settings
// are changed. Samples are bounded, including per-frame detail and file size.
namespace Debug::GameplayDiagnostics
{
    inline bool enabled()
    {
        static const bool value = [] {
            const char* mode = std::getenv("OPENMW_GAMEPLAY_DIAGNOSTICS");
            return mode && std::string_view(mode) == "1";
        }();
        return value;
    }

    inline std::string quote(std::string_view text)
    {
        std::string result = "\"";
        constexpr char hex[] = "0123456789abcdef";
        for (unsigned char ch : text)
        {
            if (ch == '"' || ch == '\\') { result += '\\'; result += char(ch); }
            else if (ch < 32) { result += "\\u00"; result += hex[ch >> 4]; result += hex[ch & 15]; }
            else result += char(ch);
        }
        return result + '"';
    }

    struct Context
    {
        bool sample = false;
        std::uint64_t frame = 0;
        unsigned lines = 0;
        unsigned dropped = 0;
        unsigned skeletonUpdates = 0, skeletonSkippedCull = 0, skeletonSkippedInactive = 0;
        unsigned cameraCallbacks = 0;
        unsigned materialProbes = 0;
        unsigned captureActors = 0, captureObjects = 0, useAnimObjects = 0;
        unsigned rigEvaluations = 0, morphEvaluations = 0, ordinaryGeometries = 0;
        unsigned geometrySnapshotHits = 0, geometrySnapshotMisses = 0;
        unsigned inheritedStateHits = 0, inheritedStateMisses = 0;
        double actorCaptureMs = 0, objectCaptureMs = 0;
        double geometryTransformMs = 0, inheritedStateMs = 0, materialCaptureMs = 0, geometryGuardMs = 0, geometryBuildMs = 0;
        unsigned materialValueHits = 0, materialValueMisses = 0, objectPlanRebuilds = 0, objectPlanReuses = 0, objectPlanFallbacks = 0;
        unsigned nativeObjectBuilds = 0, nativeObjectUpdates = 0, nativeObjectReuses = 0, nativeObjectFallbacks = 0;
        double nativeBindingMs = 0, nativeIdentityMs = 0, nativeCopyMs = 0;
    };
    inline thread_local Context context;
    inline bool sampling() { return context.sample; }
    inline bool detailedSampling()
    {
        return sampling() && RuntimeDiagnostics::mode() != RuntimeDiagnostics::Mode::Standard;
    }

    // Optional case-sensitive identity/path substring. This only selects
    // diagnostic examples; it must never affect capture or material admission.
    inline std::string_view materialProbeFilter()
    {
        static const std::string value = [] {
            const char* filter = std::getenv("OPENMW_V4_MATERIAL_PROBE_FILTER");
            return filter ? std::string(filter).substr(0, 256) : std::string();
        }();
        return value;
    }

    using Fields = std::initializer_list<std::pair<std::string_view, std::string>>;
    inline void recordEvent(std::string_view type, Fields fields = {}, bool independent = false) noexcept
    {
        if (!sampling() && !(independent && enabled())) return;
        const bool detail = type == "actor_pose" || type == "actor_geometry" || type == "pick" || type == "camera"
            || type == "material_probe" || type == "texture_probe" || type == "population_cause";
        if (detail && context.lines >= 128) { ++context.dropped; return; }
        ++context.lines; // preserve stage ends/frame summary even when detail is capped
        if (RuntimeDiagnostics::enabled())
        {
            RuntimeDiagnostics::legacy(context.frame, type, fields);
            return;
        }
        try
        {
            struct Sink
            {
                std::ofstream stream;
                std::uint64_t rows = 0;
                Sink()
                {
                    if (const char* path = std::getenv("OPENMW_GAMEPLAY_DIAGNOSTICS_FILE"))
                        stream.open(std::filesystem::path(std::u8string(path, path + std::strlen(path))), std::ios::app);
                }
            };
            static Sink sink;
            if (!sink.stream || sink.rows > 100000) return;
            if (sink.rows == 100000)
            {
                ++sink.rows;
                sink.stream << "{\"schema\":1,\"frame\":" << context.frame << ",\"type\":\"capture_limit\"}\n";
                sink.stream.flush();
                return;
            }
            ++sink.rows;
            sink.stream << "{\"schema\":1,\"frame\":" << context.frame << ",\"type\":" << quote(type);
            for (const auto& [key, value] : fields)
                sink.stream << ',' << quote(key) << ':' << quote(value);
            sink.stream << "}\n";
            sink.stream.flush(); // retain the last entered stage after a native crash
        }
        catch (...) {} // diagnostics must never turn a recoverable frame into a failure
    }

    using Clock = std::chrono::steady_clock;
    struct CapturePhase
    {
        bool active = sampling();
        double* value;
        Clock::time_point start{};
        explicit CapturePhase(double& output) : value(&output) { if (active) start = Clock::now(); }
        void next(double& output)
        {
            if (active) { const auto now = Clock::now(); *value += std::chrono::duration<double,std::milli>(now-start).count(); start=now; }
            value = &output;
        }
        ~CapturePhase() { if (active) *value += std::chrono::duration<double,std::milli>(Clock::now()-start).count(); }
    };
    // Aggregate sub-scopes, not thousands of per-object log lines. Includes
    // early failures; all values are CPU envelopes nested in dynamic_capture.
    struct CaptureWork
    {
        bool active = sampling(), actor;
        Clock::time_point start{};
        explicit CaptureWork(bool isActor) : actor(isActor)
        {
            if (active) { start=Clock::now(); actor ? ++context.captureActors : ++context.captureObjects; }
        }
        ~CaptureWork()
        {
            if (active)
                (actor ? context.actorCaptureMs : context.objectCaptureMs)
                    += std::chrono::duration<double,std::milli>(Clock::now()-start).count();
        }
    };
    // Main-thread loading operations are observed even outside sampled frames.
    // They do not enable expensive per-actor/frame sampling or mutate TLS frame
    // state. Unique IDs disambiguate nested operations with identical names.
    struct Operation
    {
        std::string_view name;
        std::string identity;
        Clock::time_point start{};
        std::uint64_t id = 0;
        int exceptions = 0;
        explicit Operation(std::string_view value, std::string_view source = {}) : name(value)
        {
            if (!enabled()) return;
            static std::uint64_t sequence = 0;
            id = ++sequence;
            identity = source;
            exceptions = std::uncaught_exceptions();
            start = Clock::now();
            recordEvent("operation_begin", {{"id", std::to_string(id)}, {"name", std::string(name)},
                {"identity", identity}}, true);
        }
        Operation(const Operation&) = delete;
        Operation& operator=(const Operation&) = delete;
        ~Operation()
        {
            if (!id) return;
            recordEvent("operation_end", {{"id", std::to_string(id)}, {"name", std::string(name)},
                {"identity", identity}, {"unwinding", std::to_string(std::uncaught_exceptions() > exceptions)},
                {"ms", std::to_string(std::chrono::duration<double, std::milli>(Clock::now() - start).count())}}, true);
        }
    };
    struct Stage
    {
        std::string_view name;
        Clock::time_point start{};
        bool active;
        int exceptions;
        explicit Stage(std::string_view value) : name(value), active(sampling()), exceptions(std::uncaught_exceptions())
        {
            if (active) { start = Clock::now(); recordEvent("stage_begin", {{"name", std::string(name)}}); }
        }
        ~Stage()
        {
            if (active) recordEvent("stage_end", {{"name", std::string(name)},
                {"ms", std::to_string(std::chrono::duration<double, std::milli>(Clock::now() - start).count())},
                {"unwinding", std::to_string(std::uncaught_exceptions() > exceptions)}});
        }
    };

    inline constexpr bool sampleSequence(std::uint64_t sequence)
    {
        return sequence <= 12 || sequence % (sequence <= 36000 ? 30 : 300) == 0;
    }

    struct Frame
    {
        bool active = false;
        bool completed = false;
        Clock::time_point start{};
        explicit Frame(std::uint64_t frame, bool vulkan, bool viewerDone)
        {
            if (!enabled()) return;
            // Initial samples plus a sustained sequence. No clocks or string
            // construction in disabled mode; sparse sampling thereafter.
            static std::uint64_t sequence = 0;
            ++sequence;
            // Long manual sessions keep sparse coverage instead of silently
            // stopping before a late cell transition. The file cap still emits
            // an explicit capture_limit record and bounds disk use.
            active = sampleSequence(sequence);
            context = {};
            context.sample = active;
            context.frame = frame;
            if (active)
            {
                start = Clock::now();
                recordEvent("frame_begin", {{"vulkan", std::to_string(vulkan)}, {"viewer_done", std::to_string(viewerDone)}});
            }
        }
        ~Frame()
        {
            if (active)
            {
                if (context.captureActors || context.captureObjects)
                {
                    recordEvent("capture_work", {{"actors",std::to_string(context.captureActors)},
                        {"objects",std::to_string(context.captureObjects)}, {"use_anim_objects",std::to_string(context.useAnimObjects)},
                        {"actor_ms",std::to_string(context.actorCaptureMs)}, {"object_ms",std::to_string(context.objectCaptureMs)},
                        {"rig_evaluations",std::to_string(context.rigEvaluations)},
                        {"morph_evaluations",std::to_string(context.morphEvaluations)},
                        {"ordinary_geometry_visits",std::to_string(context.ordinaryGeometries)},
                        {"geometry_snapshot_hits",std::to_string(context.geometrySnapshotHits)},
                        {"geometry_snapshot_misses",std::to_string(context.geometrySnapshotMisses)},
                        {"inherited_state_hits",std::to_string(context.inheritedStateHits)},
                        {"inherited_state_misses",std::to_string(context.inheritedStateMisses)}});
                    recordEvent("capture_phases", {
                        {"transform_ms",std::to_string(context.geometryTransformMs)},
                        {"state_ms",std::to_string(context.inheritedStateMs)},
                        {"material_ms",std::to_string(context.materialCaptureMs)},
                        {"mesh_guard_ms",std::to_string(context.geometryGuardMs)},
                        {"mesh_build_ms",std::to_string(context.geometryBuildMs)},
                        {"material_value_hits",std::to_string(context.materialValueHits)},
                        {"material_value_misses",std::to_string(context.materialValueMisses)},
                        {"object_plan_rebuilds",std::to_string(context.objectPlanRebuilds)},
                        {"object_plan_reuses",std::to_string(context.objectPlanReuses)},
                        {"object_plan_fallbacks",std::to_string(context.objectPlanFallbacks)}});
                    recordEvent("native_objects", {
                        {"native_object_builds",std::to_string(context.nativeObjectBuilds)},
                        {"native_object_updates",std::to_string(context.nativeObjectUpdates)},
                        {"native_object_reuses",std::to_string(context.nativeObjectReuses)},
                        {"native_object_fallbacks",std::to_string(context.nativeObjectFallbacks)},
                        {"native_binding_ms",std::to_string(context.nativeBindingMs)},
                        {"native_identity_ms",std::to_string(context.nativeIdentityMs)},
                        {"native_copy_ms",std::to_string(context.nativeCopyMs)}});
                }
                recordEvent("frame_end", {{"completed", std::to_string(completed)},
                    {"ms", std::to_string(std::chrono::duration<double, std::milli>(Clock::now() - start).count())},
                    {"skeleton_updates", std::to_string(context.skeletonUpdates)},
                    {"skeleton_skipped_cull", std::to_string(context.skeletonSkippedCull)},
                    {"skeleton_skipped_inactive", std::to_string(context.skeletonSkippedInactive)},
                    {"camera_callbacks", std::to_string(context.cameraCallbacks)},
                    {"detail_limit", std::to_string(context.dropped != 0)}});
                context.sample = false;
            }
        }
    };

    // Byte fingerprint of already-owned float arrays; never hashes file data.
    inline std::uint64_t fingerprint(const void* data, std::size_t bytes)
    {
        const auto* p = static_cast<const unsigned char*>(data);
        std::uint64_t hash = 14695981039346656037ull;
        for (std::size_t i = 0; i < bytes; ++i) hash = (hash ^ p[i]) * 1099511628211ull;
        return hash;
    }

    template<class MatrixA, class MatrixB>
    double matrixDifference(const MatrixA& a, const MatrixB& b)
    {
        double delta = 0;
        for (unsigned row = 0; row < 4; ++row)
            for (unsigned col = 0; col < 4; ++col)
            {
                const double d = std::abs(double(a(row, col)) - double(b(row, col)));
                if (!std::isfinite(d)) return -1; // explicit invalid, not an apparent match
                delta = std::max(delta, d);
            }
        return delta;
    }
}
#endif
