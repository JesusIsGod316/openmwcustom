# OptimizedMW GL-P8G2 — repaired groundcover shader optimization

Parent: `optimizedmw/gl-p8-hl1-probes@cdfe60d013f5be90767cbd9996872a4ced117951`

P8G1 SAFE-BASIS is rejected. It bound instance rotation columns to generic vertex attributes 8 and 9, which collide
with the OpenGL compatibility texture-coordinate attribute layout used by groundcover. Runtime screenshots showed
severe UV/alpha/appearance corruption. P8G1 performance data is invalid for promotion.

P8G2 deliberately keeps the established groundcover instance attributes unchanged:
- attribute 6: offset + scale
- attribute 7: Euler rotation
- no attributes 8 or 9 are added or rebound

`[Groundcover] optimizedmw gpu path`:
- 0: exact established shader path.
- 1: same-quality shader-only candidate: early squared fade-distance rejection; wind-only coefficients precomputed
  once per frame; squared stomp range rejection with sqrt only inside stomp range; exact z-only rotation fast path
  using two trig operations while non-z-only instances fall back to the established Euler matrix.
- 2: mode 1 plus a two-harmonic wind visual-risk ceiling probe.

`optimizedmw shadow receive=false` is a quality-risk probe that removes world-shadow receiving from groundcover only.

Launcher modes:
1. CONTROL
2. SAFE-SHADER
3. FAST-WIND
4. SAFE-NO-POINT
5. SAFE-NO-GRASS-SHADOWS

P8G2 does not include SHADOW-LITE because the user observed unacceptable shadow flicker in that HL1 configuration.

Promotion requires SAFE-SHADER visual parity first, then a material GPU/wall-time improvement. Quality-risk modes
remain diagnostic unless separately redesigned.
