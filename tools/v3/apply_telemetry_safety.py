from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def replace_once(rel, old, new):
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{rel}: expected exactly one match, found {count}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8", newline="\n")
    print(f"telemetry-safety patched {rel}")


# The every-frame stream is intentionally much more buffered than sparse hitch
# telemetry. Flushing every ~60 frames can itself introduce periodic I/O noise
# into a frametime benchmark; 600 lines keeps the file crash-tolerant enough
# while reducing measurement perturbation substantially.
replace_once(
    "components/debug/v3hitchtelemetry.hpp",
    '''        static constexpr unsigned BaselineInterval = 120;
        static constexpr unsigned FlushInterval = 60;''',
    '''        static constexpr unsigned BaselineInterval = 120;
        static constexpr unsigned FlushInterval = 60;
        static constexpr unsigned AllFrameFlushInterval = 600;''',
)
replace_once(
    "components/debug/v3hitchtelemetry.hpp",
    '''            if (++mAllFrameLinesSinceFlush >= FlushInterval)
            {
                mAllFrameStream.flush();''',
    '''            if (++mAllFrameLinesSinceFlush >= AllFrameFlushInterval)
            {
                mAllFrameStream.flush();''',
)

print("V3 telemetry measurement-safety pass completed successfully.")
