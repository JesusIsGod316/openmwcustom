# OptimizedMW GL-P7 CPU preparation

Parent: optimizedmw/gl-p6-stutter-residency@4f1c1d1229a85d4ec0311af0f6d64a4dac26bead

P7 attacks CPU-side transition and demand-preparation cost without changing the OpenGL compatibility surface.

P7A exposes the accepted V3.25 actor controller-batch implementation through a default-off Cells setting. Eligible NIF controller clones remain unpublished until deterministic main-thread publication; generic/non-NIF paths stay serial.

P7B overlaps two independent read-heavy stages for a new non-composite terrain chunk: vertex/normal/color generation and blendmap/layer sampling. A queue-less PrepJobService gives cull-time demand misses a Critical helper and preload work a separate Background helper. Busy lanes fail open to the exact serial path. Texture realization and scene publication remain on the caller.

The existing CellPreloader already performs model/image/keyframe/collision preparation off-main, so P7 does not duplicate those worker systems without evidence of demand misses.

Matrix:
1. P6 quarantine control
2. actor batching
3. terrain CPU prep
4. actor + terrain CPU prep
5. mode 4 + P6R native terrain resource phases
6. mode 5 + split terrain VBO upload phases

Benchmark telemetry remains launcher-only.
