# OpimizedMW GL-P2: initial optimizer/cancellation slice

This is the FIRST slice of P2, not completion of the required-readiness versus
optional-optimization split. Based on P1B 2d493b6e78a25e395895daad0298c85a947d28dd.
P1C is deferred by user direction. Vulkan remains parked.

## Implemented behavior

- Independent startup-only `[Cells] opimizedmw paging optimizer = false` control.
- Only `TerrainPreloadItem` on the legacy/OpenGL terrain route enables the scope.
  Other optimizer callers and disabled runs retain osgUtil::VertexCacheVisitor.
- Preserves OSG 3.6.5 Forsyth scores, triangle tie ordering, polygon conversion,
  degeneracy behavior, original vertex arrays and index-width selection.
- Replaces repeated full-triangle restart scans with a bounded-storage indexed
  max heap. This improves the disconnected-component worst case; adjacency
  scoring can STILL be superlinear for high-valence geometry.
- Checks cancellation during mesh enumeration, score updates, adjacency
  removal, input/output processing and between expensive chunk operations.
  Allocator calls and individual graph-visitor operations are not preemptible.
- Builds a private replacement index list before installing it in an as-yet
  unpublished paging geometry. Never edits live scene arrays or drops content.
- Releases a V3.13 strong-upgrade in-flight marker when construction throws.
- Opt-in terrain jobs catch cancellation/failure, complete their loading
  reporter, and do not advertise partial work as a prepared terrain target.
  Required waiters receive a preserved failure, not an empty success result.

## Remaining P2 work

`syncTerrainLoad` STILL waits for required terrain preparation; this first slice
makes the implicated optimizer cancellable and removes its whole-mesh restart
scan pathology. It DOES NOT yet let gameplay run while optional optimization of
the same target continues. Do not present this as full two-phase readiness,
GPU-ready publication, a paging-freeze cure, or gameplay/performance acceptance.

Separate immutable optimized replacements, generation-safe publication,
optional-job lifetime/admission and live-view refresh without traversal spikes
need their own implementation and validation. Do not remove waits before those
ownership boundaries exist. Keep efficient final batch topology, required
terrain/collision/scripts and P1 memory behavior intact.

## Validation gates

Pure CPU tests cover triangle/winding preservation, determinism, invalid input,
disconnected meshes, non-quadratic restart selection, cancellation and TLS
isolation. Real OSG tests compare exact triangle ordering against the inherited
visitor on random, disconnected, high-valence, strip/fan/quad/polygon,
degenerate and 32-bit-index fixtures, and verify transactional cancellation.

GCC and sanitized Clang lanes use real OSG. MSVC pure-policy tests do not imply
Windows OSG or full engine validation. The consolidated Windows gate compiles
and tests the engine after native tests pass. Runtime gate: previous P1 OFF
versus new P2 OFF, then same binary OFF versus ON with P1 settings fixed. Record
terrain/optimizer waits, median/p95/p99, cancellation latency and memory peaks.
No synthetic optimizer result is a measured game FPS gain.

## Provenance and recovery

The scoring/cache algorithm derives from OpenSceneGraph 3.6.5
`src/osgUtil/MeshOptimizers.cpp` (blob f337fc436befdd4a8f3270bd395b0321bbfac4b2),
under OSGPL; the copyright/license notice is retained in pagingvertexcache.hpp.
Changes to existing files are covered by the previously verified P0 source
bundle plus P1A/P1B source archives. No prior branch or legacy archive is deleted.
Production source is materialized into its own commit only after native tests;
normal engine builds use that committed C++, NOT the transport patch.

The exact upstream OSGPL license is retained in `OSGPL-LICENSE.txt` (upstream blob `793879943406cd853fa9c48397746af8c3f525ac`). Ordinary connected meshes do not maintain the restart heap; it is created lazily after two searches. No global linear-time claim: high-valence adjacency updates remain, with cooperative cancellation.
