from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def replace_once(rel, old, new):
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{rel}: expected exactly one V3.2 adaptive-v2 match, found {count}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8", newline="\n")
    print(f"V3.2 adaptive-v2 patched {rel}")


# V1 remains byte-for-byte selectable as "adaptive". V2 only changes the
# predictive preload pacing path when explicitly selected as "adaptive-v2".
# Required cell loading remains outside this scheduler.
old_block = '''            const double lastFrameMs = Debug::V3HitchTelemetry::lastFrameWallMs();
            const bool pressure = Settings::RamCache::adaptiveStreamingEnabled()
                && lastFrameMs > Settings::RamCache::streamingTargetFrameMs();
            const bool defer = pressure && (Debug::V3HitchTelemetry::currentFrame() & 1u);
            if (!defer)
                preloadCells(duration);
            else if (Debug::V3Diagnostics::streamingWriter().enabled())
            {
                std::ostringstream row;
                row << Debug::V3HitchTelemetry::currentFrame() << ',' << Debug::V3Diagnostics::epochMs()
                    << ',' << Debug::V3Diagnostics::csvQuote("defer") << ',' << Debug::V3Diagnostics::csvQuote("cell_preload") << ',' << Debug::V3Diagnostics::csvQuote("pressure") << ',' << lastFrameMs << ",1,1";
                Debug::V3Diagnostics::streamingWriter().writeLine(row.str());
            }'''

new_block = '''            const double lastFrameMs = Debug::V3HitchTelemetry::lastFrameWallMs();
            const unsigned frame = Debug::V3HitchTelemetry::currentFrame();
            const float targetMs = Settings::RamCache::streamingTargetFrameMs();

            const bool adaptiveV1 = Settings::RamCache::adaptiveStreamingEnabled();
            const bool adaptiveV2 = Settings::RamCache::adaptiveStreamingV2Enabled();
            bool defer = false;
            const char* deferDetail = "pressure";
            int deferLimit = 1;
            int deferCount = 1;

            if (adaptiveV1)
            {
                // Preserve the exact V1 experiment for historical A/B testing.
                const bool pressure = lastFrameMs > targetMs;
                defer = pressure && (frame & 1u);
            }
            else if (adaptiveV2)
            {
                // V2 uses a smoothed pressure signal and hysteresis so one slow
                // frame cannot repeatedly starve predictive preload work.
                static double v32SmoothedFrameMs = 0.0;
                static unsigned v32ConsecutiveDefers = 0;
                static bool v32PressureMode = false;

                if (lastFrameMs > 0.0)
                {
                    constexpr double alpha = 0.20;
                    v32SmoothedFrameMs = v32SmoothedFrameMs <= 0.0
                        ? lastFrameMs
                        : (v32SmoothedFrameMs * (1.0 - alpha) + lastFrameMs * alpha);
                }

                const double enterPressureMs = static_cast<double>(targetMs) * 1.05;
                const double leavePressureMs = static_cast<double>(targetMs) * 0.92;
                if (!v32PressureMode && v32SmoothedFrameMs > enterPressureMs)
                    v32PressureMode = true;
                else if (v32PressureMode && v32SmoothedFrameMs < leavePressureMs)
                {
                    v32PressureMode = false;
                    v32ConsecutiveDefers = 0;
                }

                const unsigned maxDefers = static_cast<unsigned>(
                    std::max(Settings::RamCache::streamingMaxDefers(), 0));
                const bool alternatingOpportunity = (frame & 1u) != 0u;

                // Forced progress: after maxDefers consecutive skips, the next
                // opportunity always runs preloadCells even while under pressure.
                defer = v32PressureMode && maxDefers != 0u && alternatingOpportunity
                    && v32ConsecutiveDefers < maxDefers;
                if (defer)
                    ++v32ConsecutiveDefers;
                else if (v32PressureMode && v32ConsecutiveDefers >= maxDefers)
                    v32ConsecutiveDefers = 0;
                else if (!v32PressureMode)
                    v32ConsecutiveDefers = 0;

                deferDetail = "v2_smoothed_pressure";
                deferLimit = static_cast<int>(maxDefers);
                deferCount = static_cast<int>(v32ConsecutiveDefers);
            }

            if (!defer)
                preloadCells(duration);
            else if (Debug::V3Diagnostics::streamingWriter().enabled())
            {
                std::ostringstream row;
                row << frame << ',' << Debug::V3Diagnostics::epochMs()
                    << ',' << Debug::V3Diagnostics::csvQuote("defer") << ','
                    << Debug::V3Diagnostics::csvQuote("cell_preload") << ','
                    << Debug::V3Diagnostics::csvQuote(deferDetail) << ',' << lastFrameMs << ','
                    << deferLimit << ',' << deferCount;
                Debug::V3Diagnostics::streamingWriter().writeLine(row.str());
            }'''

replace_once("apps/openmw/mwworld/scene.cpp", old_block, new_block)

print("V3.2 Adaptive Scheduler v2 source patch completed successfully.")
