# OptimizedMW Hitch Trial v2

User-authorized substantive implementation, with all four mechanisms independently
switchable and all 16 combinations available under Normal and Deep Trace.
Starting source: `codex/optimizedmw-phase9-audited@aa71f996556e6be6f1147490c79811a58452a00c`.
Normal uses the existing basic measurements. Deep uses the same production feature
vector plus the existing full engine attribution; Windows recording stays explicit.

Archive 00, 01 and 02 were refreshed before edits on 2026-10-01. Their latest
OptimizedMW checkpoint remains Event 161; the user's later resume and current
local implementation supersede that historical pause. Preserve its ownership,
complete-terrain, progress, source/control and validation lessons. Do not alter
the separate VulkanMW track, user mods, saves, shaders or rendering quality.

## Switch contracts

| Mask bit | Label | Exact opt-in environment switch |
| --- | --- | --- |
| 1 | UI | `OPENMW_HITCH_UI_REFLOW=1` |
| 2 | BAKE | `OPENMW_HITCH_COMPOSITE_BATCH=1` |
| 4 | ANIM | `OPENMW_HITCH_ANIMATION_PRELOAD=1` |
| 8 | TERRAIN | `OPENMW_HITCH_TERRAIN_CURRENT_FIRST=1` |

Only an exact value of `1` enables a mechanism. The launcher sets every switch,
including explicit zeroes, after clearing inherited experiments and restores the
original environment/settings after exit. Default and previous Phase 9 modes
retain control paths. I1/I2 and D1 remain separate, explicitly identified choices.

UI preserves MyGUI's own measurement cache by avoiding identical size/font/wrap
invalidations. It retains caption, edit history, selection, callback and layout
semantics. Mutable Lua table identity does not establish unchanged properties.
Production MyGUI geometry/text/edit equivalence is required.

BAKE confines deferred restoration to one map/FBO/compileUntil batch. Pass order
and inherited state remain exact. Programs/PCPs/geometry remain owned until caller
state restoration and temporary client-array retirement on every exit. Existing
P4/P9 scheduling, charge, progress and complete required publication remain intact.

ANIM closes additional-directory/base/custom/race-specific animation coverage.
It does not reenable the historically rejected same-name companion preloader or
unsafe actor GL prewarm. Shared exact source selection is the materially changed
mechanism; main-thread snapshots precede bounded worker cache preparation. Scene,
controller binding and gameplay activation stay on their original owner/thread.

TERRAIN uses explicit demanded target identity, not 'first predicted target' as a
proxy for demand. Correct complete targets publish individually with synchronization;
unprepared future targets are never marked loaded. Future demand, cancellation,
failure, target reuse/replacement and optional-tail view ownership need regression
coverage. The existing weak/strong optimization split is retained.

## Validation and delivery

Extend production-linked/native fixtures for each mechanism and run retained
engine/component tests. Exercise every launcher combination and both trace depths
on Windows PowerShell 5.1 and PowerShell 7, with environment restoration, selection
identity, independent feature hashes and truthful failed/incomplete captures.
Run one consolidated Windows MSVC game build after source integration and review.
Deliver to `C:\OptimizedMW_Test` with matching symbols and exact source/executable
identity; preserve previous captures and a runtime rollback checkpoint.

Source/test/build success is not performance promotion. The first user comparison
is Normal CONTROL versus Normal ALL under the same workload. Follow with targeted
single/pair comparisons and deep diagnosis when results require attribution.
No predicted millisecond or FPS gains are asserted.

## Focused attribution

At the user's request, extend the existing Deep HitchTrace with child spans for
UI property/layout/measurement work, optional animation discovery/preparation,
terrain readiness publication, and composite pass/restore operations. Reuse the
existing bounded recorder, thread/frame/parent/link identities, loss qualification
and exclusive-time calculation. Avoid forced per-widget/per-pass rows and new
high-frequency thread CPU queries. Child wall times are nested observations, not
independent numbers to sum; a readiness/preload receipt reports work rather than
proof of a measured gameplay benefit. Normal/Deep retain the same feature vector.
