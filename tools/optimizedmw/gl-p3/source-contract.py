#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[3]
optimizer_h = (root / "components/sceneutil/optimizer.hpp").read_text()
optimizer_cpp = (root / "components/sceneutil/optimizer.cpp").read_text()
objectpaging = (root / "apps/openmw/mwrender/objectpaging.cpp").read_text()
cells = (root / "components/settings/categories/cells.hpp").read_text()
defaults = (root / "files/settings-default.cfg").read_text()

checks = {
    "setting": "optimizedmw submission compaction" in cells and "optimizedmw submission compaction = false" in defaults,
    "switchable_optimizer": "setMergeCompatibleIndexTypes" in optimizer_h,
    "mixed_width_merge": "mergePromotedDrawElements" in optimizer_cpp,
    "strong_only_gate": "&& compile && !p2RequiredReadiness && v38BatchingMode >= 2" in objectpaging,
    "mechanical_counter": "p3_index_merges=" in objectpaging,
}
failed = [name for name, ok in checks.items() if not ok]
if failed:
    raise SystemExit("P3 source contract failed: " + ", ".join(failed))
print("OptimizedMW GL-P3 source contract passed")
