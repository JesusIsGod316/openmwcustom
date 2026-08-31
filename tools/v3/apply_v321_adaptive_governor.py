import os
from pathlib import Path

ROOT = Path(os.environ.get("OPENMW_PATCH_ROOT", Path(__file__).resolve().parents[2])).resolve()


def replace_exact(rel: str, old: str, new: str, expected: int = 1) -> None:
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != expected:
        raise RuntimeError(f"{rel}: expected {expected} V3.21 adaptive match(es), found {count}")
    path.write_text(text.replace(old, new, expected), encoding="utf-8", newline="\n")
    print(f"V3.21 adaptive patched {rel} ({count} match(es))")


# MODE 127 evolves MODE 126 only at the completed-set admission stage. The
# compile object cap remains the same fixed CP1 value, which avoids mutating ICO
# compile policy concurrently with OSG render/graphics threads.
replace_exact(
    "components/settings/categories/cells.hpp",
    'mIndex, "V3", "v3.21 completion governor mode", makeClampSanitizerInt(0, 1) };',
    'mIndex, "V3", "v3.21 completion governor mode", makeClampSanitizerInt(0, 2) };',
)

settings_anchor = '''        SettingValue<float> mV321CompileConservativeRatio{
            mIndex, "V3", "v3.21 compile conservative ratio", makeClampSanitizerFloat(0.1, 1.0) };'''
replace_exact(
    "components/settings/categories/cells.hpp",
    settings_anchor,
    settings_anchor
    + '''
        // MODE 127 adaptive merge-admission controls. Adaptation uses only the
        // previously completed frame and a bounded EMA/debt state.
        SettingValue<float> mV321AdaptiveTargetMilliseconds{
            mIndex, "V3", "v3.21 adaptive target milliseconds", makeClampSanitizerFloat(8.0, 50.0) };
        SettingValue<float> mV321AdaptiveFrameEmaAlpha{
            mIndex, "V3", "v3.21 adaptive frame ema alpha", makeClampSanitizerFloat(0.05, 1.0) };
        SettingValue<int> mV321AdaptiveMergeMin{
            mIndex, "V3", "v3.21 adaptive merge minimum", makeClampSanitizerInt(1, 8) };
        SettingValue<int> mV321AdaptiveMergeMax{
            mIndex, "V3", "v3.21 adaptive merge maximum", makeClampSanitizerInt(1, 16) };
        SettingValue<int> mV321AdaptiveDebtCap{
            mIndex, "V3", "v3.21 adaptive debt cap", makeClampSanitizerInt(0, 32) };
        SettingValue<int> mV321AdaptiveDebtRepayPerFrame{
            mIndex, "V3", "v3.21 adaptive debt repay per frame", makeClampSanitizerInt(0, 4) };''',
)

default_anchor = "v3.21 compile conservative ratio = 0.25"
replace_exact(
    "files/settings-default.cfg",
    default_anchor,
    default_anchor
    + '''
# MODE 127 adaptive completed-set admission. Target is deliberately above the
# current ~22 ms steady center so adaptation reacts to tail pressure instead of
# permanently starving useful work.
v3.21 adaptive target milliseconds = 24.0
v3.21 adaptive frame ema alpha = 0.20
v3.21 adaptive merge minimum = 1
v3.21 adaptive merge maximum = 4
v3.21 adaptive debt cap = 8
v3.21 adaptive debt repay per frame = 1''',
)

# Extend both runtime parsers to accept MODE 127's governor value 2.
replace_exact(
    "apps/openmw/mwrender/renderingmanager.cpp",
    "return parsed >= 0 && parsed <= 1 ? parsed : configured;",
    "return parsed >= 0 && parsed <= 2 ? parsed : configured;",
)
replace_exact(
    "apps/openmw/mwrender/renderingmanager.cpp",
    "if (v321CompletionGovernorMode == 1)",
    "if (v321CompletionGovernorMode > 0)",
)
replace_exact(
    "apps/openmw/engine.cpp",
    "return parsed >= 0 && parsed <= 1 ? parsed : configured;",
    "return parsed >= 0 && parsed <= 2 ? parsed : configured;",
)
replace_exact(
    "apps/openmw/engine.cpp",
    "if (v321CompletionGovernorMode == 1)",
    "if (v321CompletionGovernorMode > 0)",
)

budget_anchor = '''                    unsigned int admissionBudget = baseBudget;
                    if (deferred.size() > baseBudget && oldestAge >= maxDeferredFrames)
                        admissionBudget += forcedBudget;

                    while (admittedThisFrame < admissionBudget && !deferred.empty())
                    {
                        completed.push_back(deferred.front().mSet);
                        deferred.pop_front();
                        ++admittedThisFrame;
                    }

                    if (admittedThisFrame > baseBudget)
                        forcedThisFrame = admittedThisFrame - baseBudget;'''

budget_replacement = '''                    unsigned int admissionBudget = baseBudget;
                    unsigned int adaptiveDebtRepaid = 0;
                    static double adaptiveFrameEmaMs = 0.0;
                    static unsigned int adaptiveDebt = 0;
                    const double previousFrameMs = Debug::V3HitchTelemetry::lastFrameWallMs();

                    if (v321CompletionGovernorMode == 2)
                    {
                        const double targetMs
                            = static_cast<double>(Settings::cells().mV321AdaptiveTargetMilliseconds);
                        const double alpha
                            = static_cast<double>(Settings::cells().mV321AdaptiveFrameEmaAlpha);
                        const unsigned int minBudget
                            = static_cast<unsigned int>(Settings::cells().mV321AdaptiveMergeMin);
                        const unsigned int configuredMax
                            = static_cast<unsigned int>(Settings::cells().mV321AdaptiveMergeMax);
                        const unsigned int maxBudget = std::max(minBudget, configuredMax);
                        const unsigned int debtCap
                            = static_cast<unsigned int>(Settings::cells().mV321AdaptiveDebtCap);
                        const unsigned int repayCap
                            = static_cast<unsigned int>(Settings::cells().mV321AdaptiveDebtRepayPerFrame);

                        // Use only a completed prior-frame signal. Never react to
                        // partial timing from the frame currently being serviced.
                        if (previousFrameMs > 0.0)
                        {
                            adaptiveFrameEmaMs = adaptiveFrameEmaMs <= 0.0
                                ? previousFrameMs
                                : adaptiveFrameEmaMs * (1.0 - alpha) + previousFrameMs * alpha;
                        }

                        if (previousFrameMs >= targetMs + 6.0)
                            admissionBudget = minBudget;
                        else if (previousFrameMs >= targetMs + 2.0)
                            admissionBudget = std::max(minBudget, baseBudget > 1 ? baseBudget - 1 : 1u);
                        else if (previousFrameMs > 0.0 && previousFrameMs <= targetMs - 3.0
                            && (adaptiveFrameEmaMs <= 0.0 || adaptiveFrameEmaMs <= targetMs))
                            admissionBudget = maxBudget;
                        else
                            admissionBudget = std::min(maxBudget, std::max(minBudget, baseBudget));

                        // Debt represents only service withheld relative to the
                        // fixed MODE 126 budget. It is bounded and repaid only on
                        // slack frames, with a separate per-frame repayment cap.
                        if (!deferred.empty() && admissionBudget < baseBudget && adaptiveDebt < debtCap)
                        {
                            const unsigned int withheld = baseBudget - admissionBudget;
                            adaptiveDebt += std::min(withheld, debtCap - adaptiveDebt);
                        }

                        if (!deferred.empty() && adaptiveDebt > 0 && previousFrameMs > 0.0
                            && previousFrameMs <= targetMs - 2.0
                            && (adaptiveFrameEmaMs <= 0.0 || adaptiveFrameEmaMs <= targetMs))
                        {
                            const unsigned int room = maxBudget > admissionBudget ? maxBudget - admissionBudget : 0;
                            adaptiveDebtRepaid = std::min(repayCap, std::min(adaptiveDebt, room));
                            admissionBudget += adaptiveDebtRepaid;
                            adaptiveDebt -= adaptiveDebtRepaid;
                        }
                    }

                    const unsigned int adaptiveBudgetBeforeForced = admissionBudget;
                    if (deferred.size() > admissionBudget && oldestAge >= maxDeferredFrames)
                        admissionBudget += forcedBudget;

                    while (admittedThisFrame < admissionBudget && !deferred.empty())
                    {
                        completed.push_back(deferred.front().mSet);
                        deferred.pop_front();
                        ++admittedThisFrame;
                    }

                    if (admittedThisFrame > adaptiveBudgetBeforeForced)
                        forcedThisFrame = admittedThisFrame - adaptiveBudgetBeforeForced;

                    if (reportResource && v321CompletionGovernorMode == 2)
                    {
                        stats->setAttribute(frameNumber, "V321 Adaptive PreviousFrameMs", previousFrameMs);
                        stats->setAttribute(frameNumber, "V321 Adaptive FrameEmaMs", adaptiveFrameEmaMs);
                        stats->setAttribute(frameNumber, "V321 Adaptive MergeBudget", adaptiveBudgetBeforeForced);
                        stats->setAttribute(frameNumber, "V321 Adaptive Debt", adaptiveDebt);
                        stats->setAttribute(frameNumber, "V321 Adaptive DebtRepaid", adaptiveDebtRepaid);
                    }'''
replace_exact("apps/openmw/engine.cpp", budget_anchor, budget_replacement)

identity_anchor = "openmw-custom-v3.21-cp1-completion-governor"
replace_exact(
    "apps/openmw/engine.cpp",
    identity_anchor,
    identity_anchor + " / openmw-custom-v3.21-cp1-adaptive-governor",
)

for rel, required in {
    "components/settings/categories/cells.hpp": (
        "mV321AdaptiveTargetMilliseconds",
        "mV321AdaptiveFrameEmaAlpha",
        "mV321AdaptiveMergeMin",
        "mV321AdaptiveMergeMax",
        "mV321AdaptiveDebtCap",
        "mV321AdaptiveDebtRepayPerFrame",
    ),
    "files/settings-default.cfg": (
        "v3.21 adaptive target milliseconds = 24.0",
        "v3.21 adaptive merge minimum = 1",
        "v3.21 adaptive merge maximum = 4",
        "v3.21 adaptive debt cap = 8",
    ),
    "apps/openmw/engine.cpp": (
        "openmw-custom-v3.21-cp1-adaptive-governor",
        "V321 Adaptive PreviousFrameMs",
        "V321 Adaptive MergeBudget",
        "V321 Adaptive Debt",
        "lastFrameWallMs()",
        "v321CompletionGovernorMode == 2",
    ),
}.items():
    text = (ROOT / rel).read_text(encoding="utf-8")
    for marker in required:
        if marker not in text:
            raise RuntimeError(f"V3.21 adaptive generated source missing {marker!r} in {rel}")

print("V3.21 CP1 MODE 127 adaptive slack/debt governor applied")
