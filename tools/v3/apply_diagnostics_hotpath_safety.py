from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def replace_once(rel: str, old: str, new: str) -> None:
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{rel}: expected exactly one match, found {count}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8", newline="\n")
    print(f"diagnostic-hotpath safety patched {rel}")


# ScopedCsvTimer used to copy phase/detail strings before checking whether its
# writer was enabled. Scene instancing/paging can create these scopes thousands
# of times, so keep the disabled path allocation-light while retaining identical
# CSV contents when enabled.
replace_once(
    "components/debug/v3diagnostics.hpp",
    '''        ScopedCsvTimer(CsvWriter& writer, std::string_view phase, std::string_view detail = {}, double minimumMs = 0.0)
            : mWriter(writer)
            , mPhase(phase)
            , mDetail(detail)
            , mMinimumMs(minimumMs)
            , mEnabled(writer.enabled())
            , mStart(mEnabled ? Clock::now() : Clock::time_point{})
        {
        }''',
    '''        ScopedCsvTimer(CsvWriter& writer, std::string_view phase, std::string_view detail = {}, double minimumMs = 0.0)
            : mWriter(writer)
            , mEnabled(writer.enabled())
            , mMinimumMs(minimumMs)
            , mPhase(mEnabled ? phase : std::string_view{})
            , mDetail(mEnabled ? detail : std::string_view{})
            , mStart(mEnabled ? Clock::now() : Clock::time_point{})
        {
        }''',
)
replace_once(
    "components/debug/v3diagnostics.hpp",
    '''        CsvWriter& mWriter;
        std::string mPhase;
        std::string mDetail;
        double mMinimumMs;
        bool mEnabled;
        Clock::time_point mStart;''',
    '''        CsvWriter& mWriter;
        bool mEnabled;
        double mMinimumMs;
        std::string mPhase;
        std::string mDetail;
        Clock::time_point mStart;''',
)

# Do the same for nested cross-thread traces. Disabled traces now avoid copying
# category/name/detail while enabled traces retain the exact same IDs/parenting
# and CSV output.
replace_once(
    "components/debug/v3diagnostics.hpp",
    '''        TraceScope(std::string_view category, std::string_view name, std::string_view detail = {}, double minimumMs = 0.0)
            : mCategory(category)
            , mName(name)
            , mDetail(detail)
            , mMinimumMs(minimumMs)
            , mEnabled(traceWriter().enabled())
        {
            if (!mEnabled)''',
    '''        TraceScope(std::string_view category, std::string_view name, std::string_view detail = {}, double minimumMs = 0.0)
            : mEnabled(traceWriter().enabled())
            , mMinimumMs(minimumMs)
            , mCategory(mEnabled ? category : std::string_view{})
            , mName(mEnabled ? name : std::string_view{})
            , mDetail(mEnabled ? detail : std::string_view{})
        {
            if (!mEnabled)''',
)
replace_once(
    "components/debug/v3diagnostics.hpp",
    '''        std::string mCategory;
        std::string mName;
        std::string mDetail;
        double mMinimumMs = 0.0;
        bool mEnabled = false;
        unsigned long long mId = 0;''',
    '''        bool mEnabled = false;
        double mMinimumMs = 0.0;
        std::string mCategory;
        std::string mName;
        std::string mDetail;
        unsigned long long mId = 0;''',
)

text = (ROOT / "components/debug/v3diagnostics.hpp").read_text(encoding="utf-8")
required = [
    "mPhase(mEnabled ? phase : std::string_view{})",
    "mDetail(mEnabled ? detail : std::string_view{})",
    "mCategory(mEnabled ? category : std::string_view{})",
    "mName(mEnabled ? name : std::string_view{})",
]
missing = [needle for needle in required if needle not in text]
if missing:
    raise RuntimeError("V3 diagnostic hot-path preflight failed; missing:\n" + "\n".join(missing))

print("V3 disabled-diagnostics hot-path safety pass completed successfully.")
