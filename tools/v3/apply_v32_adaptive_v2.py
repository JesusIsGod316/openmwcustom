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
            bool forcedProgress = false;
            const char* eventDetail = "pressure";
            int deferLimit = 1;
            int deferCount = 1;

            // This state is main-thread-only: Scene::update owns predictive
            // preload scheduling. Reset it when v2 is not selected so changing
            // scheduler modes cannot inherit stale pressure or job age.
            static double v32SmoothedFrameMs = 0.0;
            static unsigned v32BadFrameStreak = 0;
            static unsigned v32RecoveryFrameStreak = 0;
            static unsigned v32ConsecutiveDefers = 0;
            static bool v32PressureMode = false;
            static bool v32WasActive = false;

            if (!adaptiveV2 && v32WasActive)
            {
                v32SmoothedFrameMs = 0.0;
                v32BadFrameStreak = 0;
                v32RecoveryFrameStreak = 0;
                v32ConsecutiveDefers = 0;
                v32PressureMode = false;
                v32WasActive = false;
            }

            if (adaptiveV1)
            {
                // Preserve the exact V1 experiment for historical A/B testing.
                const bool pressure = lastFrameMs > targetMs;
                defer = pressure && (frame & 1u);
            }
            else if (adaptiveV2)
            {
                v32WasActive = true;

                if (lastFrameMs > 0.0)
                {
                    constexpr double alpha = 0.20;
                    v32SmoothedFrameMs = v32SmoothedFrameMs <= 0.0
                        ? lastFrameMs
                        : (v32SmoothedFrameMs * (1.0 - alpha) + lastFrameMs * alpha);
                }

                const double enterPressureMs = static_cast<double>(targetMs) * 1.05;
                const double leavePressureMs = static_cast<double>(targetMs) * 0.92;
                constexpr unsigned enterPressureFrames = 3;
                constexpr unsigned leavePressureFrames = 6;

                if (!v32PressureMode)
                {
                    v32RecoveryFrameStreak = 0;
                    if (v32SmoothedFrameMs > enterPressureMs)
                        ++v32BadFrameStreak;
                    else
                        v32BadFrameStreak = 0;

                    if (v32BadFrameStreak >= enterPressureFrames)
                    {
                        v32PressureMode = true;
                        v32BadFrameStreak = 0;
                    }
                }
                else
                {
                    if (v32SmoothedFrameMs < leavePressureMs)
                        ++v32RecoveryFrameStreak;
                    else
                        v32RecoveryFrameStreak = 0;

                    if (v32RecoveryFrameStreak >= leavePressureFrames)
                    {
                        v32PressureMode = false;
                        v32RecoveryFrameStreak = 0;
                        v32ConsecutiveDefers = 0;
                    }
                }

                const unsigned maxDefers = static_cast<unsigned>(
                    std::max(Settings::RamCache::streamingMaxDefers(), 0));

                // Forced progress: after maxDefers consecutive skips, the next
                // opportunity always runs preloadCells even while under pressure.
                if (v32PressureMode && maxDefers != 0u)
                {
                    if (v32ConsecutiveDefers < maxDefers)
                    {
                        defer = true;
                        ++v32ConsecutiveDefers;
                    }
                    else
                    {
                        forcedProgress = true;
                        v32ConsecutiveDefers = 0;
                    }
                }
                else
                    v32ConsecutiveDefers = 0;

                eventDetail = forcedProgress ? "v2_forced_progress" : "v2_smoothed_pressure";
                deferLimit = static_cast<int>(maxDefers);
                deferCount = static_cast<int>(v32ConsecutiveDefers);
            }
            if (!defer)
                preloadCells(duration);
            if ((defer || forcedProgress) && Debug::V3Diagnostics::streamingWriter().enabled())
            {
                std::ostringstream row;
                row << frame << ',' << Debug::V3Diagnostics::epochMs()
                    << ',' << Debug::V3Diagnostics::csvQuote(defer ? "defer" : "force") << ','
                    << Debug::V3Diagnostics::csvQuote("cell_preload") << ','
                    << Debug::V3Diagnostics::csvQuote(eventDetail) << ',' << lastFrameMs << ','
                    << deferLimit << ',' << deferCount;
                Debug::V3Diagnostics::streamingWriter().writeLine(row.str());
            }'''

replace_once("apps/openmw/mwworld/scene.cpp", old_block, new_block)

print("V3.2 Adaptive Scheduler v2 source patch completed successfully.")
