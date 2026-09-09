# V4.0 CP3D dynamic actor slice

CP3D extends the neutral renderer boundary from CP3C static interiors to
deformable actor content. It does not expose OSG nodes, callbacks, controllers,
or mutable bone objects to the Vulkan backend.

## Implemented compatibility slice

- A mesh may publish an immutable source skin palette, inverse-bind matrices,
  mesh-to-skeleton transform, and per-vertex influences.
- Morph targets publish canonical position offsets from the neutral base mesh.
- Each frame publishes immutable current/previous local bone transforms and
  current/previous morph weights keyed to a generation-safe instance.
- `SingleViewFrameProducer` advances actor history only after a presented frame.
  Skipped acquisition, rejected input, resize, camera cut, and world reset cannot
  leak an unpresented pose into temporal history.
- Pose and morph inputs are checked against the current `RenderWorld`: skeleton
  identity, bone count, mesh ownership, model-node ownership, and morph-target
  count all fail closed.

The authoritative NIF adapter publishes legacy and Bethesda skin bindings,
active legacy morph controllers, canonical skeleton hierarchy, and explicit
dynamic-feature requirements. NPC body parts are composed into one neutral
actor model without exposing OSG ownership to the backend.

The Vulkan compatibility path resolves the compact skin palette against the
actor skeleton, applies morphs and CPU skinning in OpenMW order, and replaces
each actor's immutable frame graph through the normal retirement queue. This is
the correctness fallback and the contract for later GPU skinning and motion
vectors.

Unsupported particles, node effects, sequence playback, dynamic vertex data,
unreproduced controllers, live effects, dynamic lights, and actor fades fail
closed with a diagnostic. They must not disappear while the Vulkan route still
reports itself healthy.
