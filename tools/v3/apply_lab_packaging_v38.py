from pathlib import Path


base = Path(__file__).with_name("apply_lab_packaging_v37.py")
text = base.read_text(encoding="utf-8")
needle = (
    'v37_shadow = Path(__file__).with_name("apply_v37_shadow_stabilization.py")\n'
    'exec(compile(v37_shadow.read_text(encoding="utf-8"), str(v37_shadow), "exec"),\n'
    '    {"__file__": str(v37_shadow), "__name__": "__main__"})\n\n'
    "''' + marker"
)
replacement = (
    'v37_shadow = Path(__file__).with_name("apply_v37_shadow_stabilization.py")\n'
    'exec(compile(v37_shadow.read_text(encoding="utf-8"), str(v37_shadow), "exec"),\n'
    '    {"__file__": str(v37_shadow), "__name__": "__main__"})\n\n'
    'v38_traversal = Path(__file__).with_name("apply_v38_traversal_gpu.py")\n'
    'exec(compile(v38_traversal.read_text(encoding="utf-8"), str(v38_traversal), "exec"),\n'
    '    {"__file__": str(v38_traversal), "__name__": "__main__"})\n\n'
    "''' + marker"
)
if text.count(needle) != 1:
    raise RuntimeError("Unable to insert V3.8 traversal layer after complete V3.7 stack")
text = text.replace(needle, replacement, 1)
exec(compile(text, str(base), "exec"), {"__file__": str(base), "__name__": "__main__"})

readme_path = Path(__file__).resolve().parents[2] / "V3-LAB-README.txt"
with readme_path.open("a", encoding="utf-8", newline="\n") as readme:
    readme.write(r'''

V3.8 traversal smoothness / GPU efficiency
==========================================

Primary objective:
- Improve normal exterior gameplay frame-time consistency and sustained FPS. Door/interior transition
  hitches are secondary unless a traversal optimization helps them incidentally.

World batching modes:
- 0: exact V3.7/upstream paging merge heuristic.
- 1 conservative: stronger merge pressure for immutable distant chunks.
- 2 moderate: much stronger distant merge pressure, repeated-template preference, post-transform vertex-cache optimization.
- 3 aggressive: force every eligible distant template into OpenMW's existing merge pipeline, allow repeated active-grid
  templates to merge, and run both post-transform and access-order mesh optimization.

The implementation deliberately extends OpenMW's existing worker-side ObjectPaging merge optimizer. It retains the
existing eligibility filters, LOD selection, StateSet/material compatibility, alpha behavior, update-traversal exclusion,
refnum markers, and geometry merge implementation rather than bolting on a second incompatible batching system.

GPU residency modes:
- 0: V3.7 pressure admission/sweep behavior only.
- 1 conservative: under hard pressure, expire only very stale cache-only scene templates.
- 2 moderate: pressure-driven scene-template and paged-chunk reclamation.
- 3 aggressive: shorter pressure ages and cache-only groundcover reclamation too.

The residency path reuses GenericObjectCache reference-count safety. Live/external scene nodes refresh their timestamps
and cannot be removed. Long host-side NIF/image/keyframe caches remain untouched so the system continues to prefer the
32-GB host RAM budget while reclaiming stale GPU-backed render graphs when the 8-GB adapter is pressured.

V3.8 launcher choices:
- 39 clean traversal baseline: proven V3.6/V3.7 stack, unvalidated companion keyframe preload and far stabilization off,
  all new V3.8 mechanisms off.
- 40/41/42 batching conservative/moderate/aggressive.
- 43/44/45 GPU residency conservative/moderate/aggressive.
- 46/47/48 combined conservative/moderate/aggressive.

These are runtime modes inside one executable, not separate build cycles.
''')
