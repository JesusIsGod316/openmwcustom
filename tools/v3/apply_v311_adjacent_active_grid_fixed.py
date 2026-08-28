from pathlib import Path


# V3.7+ generated CellPreloader source already has settings access in the inherited
# stack, so V3.11 must not require the old pristine adjacent include pair. Execute
# the V3.11 layer after removing only that redundant include-edit operation.
base = Path(__file__).with_name("apply_v311_adjacent_active_grid.py")
text = base.read_text(encoding="utf-8")
redundant = '''replace_exact(
    "apps/openmw/mwworld/cellpreloader.cpp",
    \'\'\'#include <components/resource/scenemanager.hpp>
#include <components/terrain/view.hpp>\'\'\',
    \'\'\'#include <components/resource/scenemanager.hpp>
#include <components/settings/values.hpp>
#include <components/terrain/view.hpp>\'\'\',
)

'''
if text.count(redundant) != 1:
    raise RuntimeError("Unable to remove redundant V3.11 CellPreloader settings include edit")
text = text.replace(redundant, "", 1)
exec(compile(text, str(base), "exec"), {"__file__": str(base), "__name__": "__main__"})
