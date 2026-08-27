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
    'v38_shadow = Path(__file__).with_name("apply_v38_shadow_traversal.py")\n'
    'exec(compile(v38_shadow.read_text(encoding="utf-8"), str(v38_shadow), "exec"),\n'
    '    {"__file__": str(v38_shadow), "__name__": "__main__"})\n\n'
    'v38_compile = Path(__file__).with_name("apply_v38_compile_pacing.py")\n'
    'exec(compile(v38_compile.read_text(encoding="utf-8"), str(v38_compile), "exec"),\n'
    '    {"__file__": str(v38_compile), "__name__": "__main__"})\n\n'
    "''' + marker"
)
if text.count(needle) != 1:
    raise RuntimeError("Unable to insert complete V3.8 optimization stack after V3.7")
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
- 2 moderate: much stronger distant merge pressure, repeated-template preference, post-transform vertex-cache optimization,
  and post-batch shared-state compaction through SceneManager's existing SharedStateManager.
- 3 aggressive: force every eligible distant template into OpenMW's existing merge pipeline, allow repeated active-grid
  templates to merge where the existing refnum/optimizer safety rules permit it, run both post-transform and access-order
  mesh optimization, and compact duplicate post-merge render state.

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
32-GB host RAM budget while stale render graphs become reclaimable under pressure. V3.8 intentionally does not call
releaseGLObjects() on arbitrary expired scene graphs because their StateSets/textures may be shared with live objects.

Far-shadow modes:
- 0: proven V3.7 2-pixel far-cascade caster pruning.
- 1: 2.5-pixel conservative.
- 2: 3.5-pixel moderate.
- 3: 5-pixel aggressive.
Only the farthest cascade changes; near/mid cascades, map resolution, receiver shading and dynamic-caster semantics remain intact.

Incremental compile-pacing modes:
- 0: exact OpenMW/OSG behavior using the configured Cells target framerate.
- 1 conservative: retain target frame rate, cap compilation at 6 GL objects/frame and use a lower spare-time ratio.
- 2 balanced: compile target at most 45fps, cap 8 objects/frame, default 0.5 spare-time ratio.
- 3 aggressive preparation: compile target at most 36fps, cap 12 objects/frame, 0.6 spare-time ratio.
These modes aim to prepare newly paged VBO/state before first visibility without an unbounded compile burst.

Hardware object instancing QC note:
- Groundcover proves the renderer supports hardware instancing, but the project's Rafael PBR archive overlays the final
  object and shadow shaders during CMake. Stock-only instancing shader edits would therefore not be guaranteed to reach
  the actual runtime shader set. V3.8 does not ship unsafe partial object instancing; the stronger shader-independent
  merge path is used until the final PBR overlay can be transformed and shadow-tested as one atomic feature.

V3.8 launcher choices:
- 39 clean traversal baseline: proven V3.6/V3.7 stack, unvalidated companion keyframe preload and far stabilization off,
  all new V3.8 mechanisms off.
- 40/41/42 batching conservative/moderate/aggressive.
- 43/44/45 GPU residency conservative/moderate/aggressive.
- 46/47/48 combined conservative/moderate/aggressive (batching + residency + far-shadow + compile pacing).
- 49/50/51 isolated far-shadow conservative/moderate/aggressive.
- 52/53/54 isolated incremental compile pacing conservative/balanced/aggressive-preparation.

These are runtime modes inside one executable, not separate build cycles.
''')
