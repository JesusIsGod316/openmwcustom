# OptimizedMW GL-P8G — groundcover GPU path

Parent: `optimizedmw/gl-p8-hl1-probes@cdfe60d013f5be90767cbd9996872a4ced117951`

## Runtime motivation

The HL1 matched route showed that disabling groundcover reduced settled wall median by about 26% and GPU draw
median by about 22%, while graphics-thread OSG draw CPU changed only slightly. This identifies groundcover primarily
as a GPU/shader workload rather than a draw-call CPU bottleneck.

The existing renderer already uses hardware instancing for groundcover, so generic static instancing does not address
this measured hotspot.

## P8G modes

`[Groundcover] optimizedmw gpu path`:

- 0: exact established Euler/full-wind shader path.
- 1: same-quality candidate. Precompute the established GLSL Euler rotation matrix once per instance on the CPU,
  stream its three basis columns as instanced attributes, and perform the existing fade-distance rejection before
  rotation/wind/lighting work.
- 2: mode 1 plus a deliberately visual-risk two-harmonic wind ceiling probe.

`[Groundcover] optimizedmw shadow receive` defaults true. False is a quality-risk probe that removes world-shadow
coordinate work and sampling from groundcover only.

The existing `point lighting` setting is also exposed as a benchmark-only quality probe.

## Safety and control

Mode 0 retains the old Vec3 Euler attribute at location 7 and does not add attribute-divisor state for locations 8–9.
Modes 1–2 use three Vec3 instanced basis columns at locations 7–9. The CPU basis formula is a literal transcription
of the established groundcover vertex shader matrix columns rather than a quaternion reinterpretation.

No groundcover population, density, fade distance, texture, alpha, stomp, fog, normal-map, scene ownership, or
ObjectPaging behavior is changed by mode 1.

## Launcher

`START-OptimizedMW-GL-P8G-Test.bat` exposes:

1. CONTROL
2. SAFE-BASIS
3. FAST-WIND
4. SAFE-NO-POINT
5. SAFE-NO-GRASS-SHADOWS
6. SAFE-SHADOW-LITE

All modes retain the selected P7 FULL-STUTTER stack and OSG Automatic/DrawThreadPerContext behavior.

## Decision gate

SAFE-BASIS is promotable only if it preserves visual placement/orientation/wind/shadows and materially improves GPU
or wall frame time. The remaining modes are ceiling probes only unless separately redesigned for visual parity.
