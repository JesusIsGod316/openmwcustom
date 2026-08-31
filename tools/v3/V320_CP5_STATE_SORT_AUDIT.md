# V3.20 CP5 render-state sorting audit

## Disposition

CP5 does not add a second render-state sorter. The candidate is rejected for the
initial V3.20 build because OpenSceneGraph already owns opaque submission grouping
through its `StateGraph`/`RenderBin` path, while OpenMW assigns explicit ordering
contracts to bins whose order is semantically visible.

## Audited ownership

- The default opaque bin is OpenMW render bin 0 and is submitted through OSG's
  existing state graph. A parallel OpenMW sorter would duplicate the established
  state owner without eliminating traversal or drawables.
- Transparent geometry uses the depth-sorted bin, including back-to-front ordering
  and the transparent depth post-pass. State-first reordering is not equivalent.
- Shadows use a custom protected render bin and traverse render-bin, state-graph,
  and leaf structures directly. Reordering outside that owner can change shadow
  caster behavior.
- Sky, water, occlusion queries, first-person depth clearing, sun glare, and
  distortion have explicit bin numbers or callbacks. Their order is part of the
  rendering contract rather than an optimization hint.
- V3.15 already tested exact shared-state canonicalization immediately before the
  existing geometry merger. That mechanism improves state identity at the one
  narrow point with an established static-geometry owner; it does not authorize
  arbitrary cross-drawable or cross-bin equivalence.

## Rejected approximations

- Sorting all opaque drawables by a newly derived StateSet hash or comparator.
- Moving transparent or alpha-tested content into state-first order.
- Reusing material, PBR, lighting, shader, or shadow-state similarity as identity.
- Reassigning render bins or bypassing custom bin callbacks.
- Treating pointer equality after cull as a new stable ordering contract without
  proving that OSG's current state graph failed to perform the same grouping.

## Revisit gate

Revisit only with measured evidence that an identified opaque bin owner leaves a
large, stable population of identical-state leaves unsorted. Any implementation
must operate inside that owner, preserve stable order for equal keys, exclude every
custom/transparent bin, keep shaders and StateSets byte-identical, and expose
submitted-leaf and state-transition counters. Visual parity must include shadows,
PBR, alpha, water, sky, first-person rendering, and post-processing.

## Checkpoint result

The safe V3.20 tree remains CP1 focus refinement, CP2 engine/Lua and conversion
fast paths, and CP3 same-frame sound-query coalescing. CP5 changes no generated
runtime source and preserves exact P0 fallback. This rejection prevents a
duplicate or semantically unsafe ordering layer from entering the gameplay build.
