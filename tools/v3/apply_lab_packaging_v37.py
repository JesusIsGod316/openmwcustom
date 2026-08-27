from pathlib import Path


base = Path(__file__).with_name("apply_lab_packaging.py")
text = base.read_text(encoding="utf-8")
marker = '''# Install the V3 helper launchers and exact applied-source snapshot beside the
# runtime executable. This script only runs through the V3 harness, so upstream/default builds are unaffected.'''
insert = '''# V3.7 is deliberately layered after every V3.6 generator and before packaging
# validates/captures V3-applied-source.patch.
v37_core = Path(__file__).with_name("apply_v37_core.py")
exec(compile(v37_core.read_text(encoding="utf-8"), str(v37_core), "exec"),
    {"__file__": str(v37_core), "__name__": "__main__"})

v37_hitch = Path(__file__).with_name("apply_v37_hitch_paths.py")
exec(compile(v37_hitch.read_text(encoding="utf-8"), str(v37_hitch), "exec"),
    {"__file__": str(v37_hitch), "__name__": "__main__"})

''' + marker
if text.count(marker) != 1:
    raise RuntimeError("Unable to locate V3 packaging insertion marker")
text = text.replace(marker, insert, 1)
exec(compile(text, str(base), "exec"), {"__file__": str(base), "__name__": "__main__"})

# The base packager intentionally retains the V3.6 historical notes. Append the
# V3.7 delta so the installed lab README explains only what changed on this branch.
readme_path = Path(__file__).resolve().parents[2] / "V3-LAB-README.txt"
with readme_path.open("a", encoding="utf-8", newline="\n") as readme:
    readme.write(r'''

V3.7 hitch/shadow/residency delta
================================

Already promoted in the normal V3.6 performance profile on this branch:
- Visually validated 2-pixel far-cascade caster pruning, with [V3] v3.7 disable far caster pruning as a dedicated kill switch.

New V3.7 experiments (default off unless a V3.7 launcher choice selects them):
- v3.7 active event fast path: avoids empty global OnActive dispatch work while preserving event order/count and local activation semantics.
- Loaded-container empty-handler fast path: unconditional semantics-preserving shortcut; unloaded containers still materialize at the original semantic point.
- v3.7 companion keyframe preload: broadens preload-worker .kf warm-up from legacy x-prefixed NIFs to any preloaded NIF with a same-name .kf, deduplicated per preload item.
- v3.7 relaxed resource cache sweep: changes only ResourceSystem sweep cadence (default experiment value 5 seconds); cache expiry values and normal one-second behavior remain unchanged when disabled.

V3.7 unified launcher choices:
- 33: normal V3.7 candidate = V3.6 profile + active-event fast path + companion-keyframe preload + relaxed resource sweep.
- 34: active-event fast path isolated.
- 35: companion-keyframe preload isolated + hitch attribution.
- 36: V3.7 hitch combined + deep attribution.

The historical V3.6 choices 24-32 intentionally leave every new V3.7 experimental switch off so they remain valid same-executable comparison points.
''')
