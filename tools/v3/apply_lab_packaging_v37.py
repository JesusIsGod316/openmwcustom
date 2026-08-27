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

# V3.7 residency originally used an exact-match launcher literal with one
# accidental trailing space. Normalize only those two source literals in memory
# before executing the generator so generated PowerShell remains whitespace-clean.
v37_residency = Path(__file__).with_name("apply_v37_residency.py")
v37_residency_text = v37_residency.read_text(encoding="utf-8")
v37_quote3 = "'" * 3
v37_dquote3 = chr(34) * 3
v37_bad_false = v37_quote3 + "    Set-IniValue $SettingsPath 'Cells' 'v3.2 gpu memory management' 'false' " + v37_quote3
v37_good_false = v37_dquote3 + "    Set-IniValue $SettingsPath 'Cells' 'v3.2 gpu memory management' 'false'" + v37_dquote3
v37_bad_value = v37_quote3 + "    Set-IniValue $SettingsPath 'Cells' 'v3.2 gpu memory management' $V37GpuMemoryManagement " + v37_quote3
v37_good_value = v37_dquote3 + "    Set-IniValue $SettingsPath 'Cells' 'v3.2 gpu memory management' $V37GpuMemoryManagement" + v37_dquote3
if v37_residency_text.count(v37_bad_false) != 1 or v37_residency_text.count(v37_bad_value) != 1:
    raise RuntimeError("V3.7 residency launcher-literal normalization no longer matches exactly once")
v37_residency_text = v37_residency_text.replace(v37_bad_false, v37_good_false, 1)
v37_residency_text = v37_residency_text.replace(v37_bad_value, v37_good_value, 1)
exec(compile(v37_residency_text, str(v37_residency), "exec"),
    {"__file__": str(v37_residency), "__name__": "__main__"})

v37_shadow = Path(__file__).with_name("apply_v37_shadow_stabilization.py")
exec(compile(v37_shadow.read_text(encoding="utf-8"), str(v37_shadow), "exec"),
    {"__file__": str(v37_shadow), "__name__": "__main__"})

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
- Adapter-aware residency admission: combines process-local DXGI pressure with adapter-wide NVML pressure. Soft pressure caps NEW predictive outer-ring cell preloads to one per frame; hard pressure admits none. Required/current cells and already-preloaded refreshes are untouched. No GL objects are destructively evicted.
- v3.7 stabilize far shadow cascade: default-off orthographic far-cascade texel-grid snap, limited to at most half a texel per axis. It is not whole-map reuse and does not freeze actor/player shadows.

V3.7 unified launcher choices:
- 33: normal V3.7 candidate = V3.6 profile + active-event fast path + companion-keyframe preload + relaxed resource sweep + adapter-aware non-destructive preload admission.
- 34: active-event fast path isolated.
- 35: companion-keyframe preload isolated + hitch attribution.
- 36: V3.7 hitch combined + deep attribution + adapter-aware preload admission.
- 37: adapter-aware speculative preload admission isolated against the normal V3.6 profile.
- 38: far-cascade texel stabilization isolated at 6144 shadow distance.

The historical V3.6 choices 24-32 intentionally leave every new V3.7 experimental switch off so they remain valid same-executable comparison points.

Shadow architecture note:
- A true static/dynamic far-shadow split is still deferred. The current receiver shader has one sampler/transform per cascade, while the installed Rafael PBR overlay can replace compatibility shader resources after the normal resource copy. Adding a second static depth layer therefore requires an explicitly audited overlay/shader composition path; V3.7 does not fake this with stale whole-map reuse.
''')
