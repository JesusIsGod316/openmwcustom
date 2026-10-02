# Focused renderer candidate, 2026-10-02

This is checkpoint one of the Revision 4 next-move audit. It targets stalled
background terrain preparation and generated composite-mask mip generation.
It preserves the retained renderer, original STEP, required bake, cleanup and
all earlier launcher selections as separate controls. It does not promote a
performance result or replace the Lua runtime.

## Selection

Run `C:\OptimizedMW_Test\START-OptimizedMW-Test.bat`, choose **F**, then:

| Choice | Mechanisms |
| --- | --- |
| 1 | Retained animation lifetime control (A1) |
| 2 | Control plus cleanup admission |
| 3 | Control plus the original STEP (A257) |
| 4 | Control plus early STEP and progress repair |
| 5 | Choice 4 plus supplied CPU mask mips |
| 6 | Choice 4 plus cleanup; mips selectable independently |

Normal and Deep select identical gameplay mechanisms. Normal starts no Windows
recorder. Deep defaults to engine diagnostics; Windows capture is optional.
Both new flags are explicitly zero on earlier launcher choices:
`OPENMW_HITCH_COMPOSITE_PROGRESS` and `OPENMW_HITCH_COMPOSITE_CPU_MIPS`.
Advanced Custom/CLI can select these independently; the progress mechanism
requires the existing STEP preparation path. No resource cap, pressure tier,
shader quality, save or mod change is part of this checkpoint.

## Terrain progress

The old selector repeatedly searched every queued map for the oldest unvisited
one and retried maps still waiting for their dependencies. The new path retains
canonical queue membership, uses a persistent fair cursor, and prioritizes the
bounded admitted pool. It examines at most 16 queue entries per owning frame,
including repeated traversals. Waiting maps retry on dependency, credit or
pressure generation changes; budget-deferred maps retry next frame. A bounded
32-frame maintenance interval preserves invalidation and weak-owner progress.

Full eight-map or unavailable metadata capacity is rejected before building
the 512-entry dependency descriptor. Final locked reservation and full resource
identity validation still govern publication. Completion wakes are weak,
generation qualified counters; no allocating notification queue or callback
into renderer locks is introduced. Required promotion sees parked membership
and the existing in-flight requiredness check.

Selection and non-drawing attempts use the shared ICO/bake account. The queue
maintenance sub-account is capped at 0.25 ms and a fraction of available budget;
one indivisible CPU operation can overrun that estimate and is charged in full.
Actual drawing uses the existing submission account. A repeat cannot replenish
budget or mint another progress token. Weak GL debt is pruned even without new
maps. Cancellation does not refund still-live unused GL allocations, and a
failed debt ledger strands conservative credit and disables new speculation.

An exact submitted texture can complete without GL on a separately count/time
bounded draw-thread lane, even with zero allocation budget. Programs and FBOs
do not receive this proof. Cancellation may retire bookkeeping; uncertain or
stale context/image/target identity never permits an upload on that lane.
No-op completions do not train allocation cost estimates.

A cold 1 MiB target can have a conservative prediction equal to the whole 2 ms
cap, leaving no fit after positive maintenance. A narrow target/FBO bootstrap
uses the existing single exceptional-service token. It rechecks exact identity,
valid Normal pressure, handoff suppression and conservative actual remaining
frame headroom immediately before dispatch. It charges all actual cost and
stops further optional service for the frame, including ordinary cleanup.
Pressure safety cleanup remains available. Arbitrary source uploads and
programs keep their existing budget/age rules; this does not guarantee every
source can prepare before demand or a driver call stays inside its prediction.

Required terrain keeps complete synchronous rendering. The 32 MiB reservation,
eight-map bound, original layer order, exact context/revision ownership, caller
GL restoration and complete visible texture publication remain required.

## Supplied mask mip chains

Only immutable generated ESM3 composite alpha-byte masks qualify. Direct terrain,
ESM4, diffuse/normal images, arbitrary mod images and RTT outputs use their
existing paths. Capability information is published from the existing realized
GL identification operation before terrain construction; producers perform no
GL capability queries or new worker waits.

The area-weighted linear NPOT chain preserves the base level and independent
opacity semantics. A 130-square mask supplies 130, 65, 32, 16, 8, 4, 2 and 1
levels, with 22,490 bytes of image data. Its original 16,900-byte image is also
retained for fallback, so reservation counts 39,390 payload bytes, plus normal
descriptor overhead. Filtering, wrapping, borders and anisotropy are preserved.
No sRGB conversion or cross-layer normalization is added.

The generated texture uses ordinary OSG uploads of all supplied levels. Unknown
or incompatible contexts, a released/recreated context, dirty source/image or
unsupported allocation use an explicit original-image control texture with its
own GL object. The outer texture cannot inherit a fallback preparation memo.
No shared image pointer is swapped during rendering and no GL object is aliased
between owners. Optional allocation failure keeps original rendering.
Queued optional dispatch rechecks original-source eligibility, exact retained
bytes and draw-owned mip-generation dirtiness before any upload. A stale source
retires through cancellation; it cannot upload a different fallback GL owner
under the old reservation. With progress enabled, this retirement also uses the
bounded no-GL bookkeeping lane when allocation headroom is zero.

Driver-generated mip values need not be bit-identical to the supplied chain.
The native fixture checks fixed pixel tolerances with the actual processed
terrain shader and checks every uploaded level and exact generation ownership.
The user's visual test remains necessary for real terrain edges and mod assets.

## Evidence for the next comparison

Normal exports bounded progress counters in the existing composite CSV/log:
descriptor builds, capacity declines, polls/skips, consumed layers, completed
maps and pressure yields. Existing ICO summaries expose no-GL completion and
bootstrap prediction/headroom overruns. Deep keeps exact preparation/wave/use
receipts, adds terminal reasons, and records first-demand lead in integer
microseconds from observed admission, first restored optional layer and private
optional completion. Unobserved stages use the unsigned metric sentinel
UINT64_MAX, and a raced admission snapshot reports unknown generation zero.
Layer outcomes separately identify required/cancelled/uncompleted fallback use;
a selected flag or an actual fallback draw is not proof of useful preparation.
Changed wait states emit a linked `composite.progress_park` receipt: reason 1 is
dependency, 2 capacity, 3 pressure and 4 frame budget. Repeated unchanged waits
are aggregated instead of emitting another event for every poll.

Use F/1, F/5, F/5, F/1 in Normal with the same declared save hash, first-person
view, cap and route. Hold the same heavy view for 45 seconds, then walk the same
route and quit normally. Keep loading, first visits and revisits separate. A
process restart is not proof of cold driver or disk caches. Compare whole-frame
distributions and tails, required bake exposure, layer consumption before
demand and queue/debt age. If consumption remains zero, inspect refusal and
terminal reasons before interpreting frame changes as a successful mechanism.
F/4 versus F/5 isolates mips; F/1 versus F/2 isolates cleanup.

Passing source/fixture/package checks establishes implementation and rendering
contracts. User hardware runs establish performance and runtime acceptance.
Paging optimizer and native Lua layout checkpoints remain separate later work.
