import os
from pathlib import Path

ROOT = Path(os.environ.get("OPENMW_PATCH_ROOT", Path(__file__).resolve().parents[2])).resolve()
engine = ROOT / "apps/openmw/engine.cpp"
text = engine.read_text(encoding="utf-8")

include_anchor = "#include <chrono>\n"
if text.count(include_anchor) != 1:
    raise RuntimeError(f"V3.19 focus include anchor mismatch: {text.count(include_anchor)}")
if "#include <cstdlib>\n" not in text:
    text = text.replace(include_anchor, include_anchor + "#include <cstdlib>\n", 1)

call_anchor = "        mWorld->updateFocusObject();"
if text.count(call_anchor) != 1:
    raise RuntimeError(f"V3.19 focus call anchor mismatch: {text.count(call_anchor)}")
replacement = r'''        // V3.19: focus-object GUI refresh can reuse the previous result for a small
        // number of ordinary gameplay frames. Activation/input queries remain untouched.
        // GUI mode always refreshes every frame to preserve mouse/console behavior.
        static const unsigned v319FocusCadence = [] {
            const char* value = std::getenv("OPENMW_V319_FOCUS_CADENCE");
            if (value == nullptr || *value == '\0')
                return 1u;
            const int parsed = std::atoi(value);
            return parsed >= 1 && parsed <= 3 ? static_cast<unsigned>(parsed) : 1u;
        }();
        if (v319FocusCadence <= 1 || mWindowManager->isGuiMode() || frameNumber % v319FocusCadence == 0)
            mWorld->updateFocusObject();'''
text = text.replace(call_anchor, replacement, 1)

for marker in (
    'OPENMW_V319_FOCUS_CADENCE',
    'mWindowManager->isGuiMode()',
    'frameNumber % v319FocusCadence == 0',
    'Activation/input queries remain untouched.',
):
    if marker not in text:
        raise RuntimeError(f"V3.19 focus layer missing marker: {marker}")

engine.write_text(text, encoding="utf-8", newline="\n")
print("V3.19 focus temporal-coherence layer added")
