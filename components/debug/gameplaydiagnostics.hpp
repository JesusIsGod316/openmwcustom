#ifndef OPENMW_DEBUG_GAMEPLAYDIAGNOSTICS_H
#define OPENMW_DEBUG_GAMEPLAYDIAGNOSTICS_H

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
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
    };
    inline thread_local Context context;
    inline bool sampling() { return context.sample; }

    using Fields = std::initializer_list<std::pair<std::string_view, std::string>>;
    inline void emit(std::string_view type, Fields fields = {}, bool independent = false) noexcept
    {
        if (!sampling() && !(independent && enabled())) return;
        const bool detail = type == "actor_pose" || type == "actor_geometry" || type == "pick" || type == "camera";
        if (detail && context.lines >= 128) { ++context.dropped; return; }
        ++context.lines; // preserve stage ends/frame summary even when detail is capped
        try
        {
            struct Sink
            {
                std::ofstream stream;
                std::uint64_t rows = 0;
                Sink()
                {
                    if (const char* path = std::getenv("OPENMW_GAMEPLAY_DIAGNOSTICS_FILE"))
                        stream.open(std::filesystem::u8path(path), std::ios::app);
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
            emit("operation_begin", {{"id", std::to_string(id)}, {"name", std::string(name)},
                {"identity", identity}}, true);
        }
        Operation(const Operation&) = delete;
        Operation& operator=(const Operation&) = delete;
        ~Operation()
        {
            if (!id) return;
            emit("operation_end", {{"id", std::to_string(id)}, {"name", std::string(name)},
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
            if (active) { start = Clock::now(); emit("stage_begin", {{"name", std::string(name)}}); }
        }
        ~Stage()
        {
            if (active) emit("stage_end", {{"name", std::string(name)},
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
                emit("frame_begin", {{"vulkan", std::to_string(vulkan)}, {"viewer_done", std::to_string(viewerDone)}});
            }
        }
        ~Frame()
        {
            if (active)
            {
                emit("frame_end", {{"completed", std::to_string(completed)},
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
