#version 120
uniform sampler2D temporalDepth;
uniform vec2 renderSize;
uniform bool resetHistory;
uniform float clearDepth;
varying vec4 previousClip;
void main()
{
    vec2 raster = gl_FragCoord.xy / renderSize;
    float depth = texture2D(temporalDepth, raster).r;
    // Exact opaque visible-surface overlay. Cutouts/transparency, custom
    // vertex effects and instancing are explicitly excluded by capture.
    if (depth == clearDepth || abs(depth - gl_FragCoord.z) > 0.000002) discard;
    vec2 motion = vec2(0.0);
    if (!resetHistory && previousClip.w > 1e-7 && previousClip.w < 1e20)
    {
        motion = (previousClip.xy / previousClip.w - (raster * 2.0 - 1.0))
            * vec2(0.5, -0.5) * renderSize;
        if (!(abs(motion.x) < 65504.0 && abs(motion.y) < 65504.0)) motion = vec2(0.0);
    }
    gl_FragColor = vec4(motion, 0.0, 1.0);
}
