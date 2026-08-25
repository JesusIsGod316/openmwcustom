from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
path = ROOT / "components/sceneutil/workqueue.cpp"
lines = path.read_text(encoding="utf-8").splitlines()

replacements = {
    "enqueue": '                << Debug::V3Diagnostics::threadId() << \',\' << Debug::V3Diagnostics::csvQuote("enqueue") << \',\' << itemId << \',\'',
    "start": '                    << Debug::V3Diagnostics::threadId() << \',\' << Debug::V3Diagnostics::csvQuote("start") << \',\' << itemId << \',\'',
    "end": '                    << Debug::V3Diagnostics::threadId() << \',\' << Debug::V3Diagnostics::csvQuote("end") << \',\' << itemId << \',\'',
}

counts = {key: 0 for key in replacements}
for i, line in enumerate(lines):
    if "Debug::V3Diagnostics::threadId()" not in line:
        continue
    for label, replacement in replacements.items():
        if label in line:
            lines[i] = replacement
            counts[label] += 1
            break

for label, count in counts.items():
    if count != 1:
        raise RuntimeError(f"components/sceneutil/workqueue.cpp: expected one {label} telemetry row, found {count}")

path.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
print("V3 workqueue telemetry string-safety pass completed successfully.")
