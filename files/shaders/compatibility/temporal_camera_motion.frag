#version 120
// Phase 9 opt-in camera/static motion stage. This is not a dense-motion
// or DLSS implementation; actors, moving objects and grass need overlays.
uniform sampler2D temporalDepth;
uniform mat4 clipToPreviousClip;
uniform vec2 renderSize;
uniform vec2 jitterPixels;
uniform bool depthZeroToOne;
uniform bool resetHistory;
uniform float clearDepth = -1.0;

void main()
{
    if (resetHistory)
    {
        gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    vec2 raster = gl_FragCoord.xy / renderSize;
    float depth = texture2D(temporalDepth, raster).r;
    if (depth == clearDepth) { gl_FragColor = vec4(0.0); return; }
    vec2 jitterClip = vec2(2.0, -2.0) * jitterPixels / renderSize;
    vec2 currentClip = raster * 2.0 - 1.0 - jitterClip;
    float clipZ = depthZeroToOne ? depth : depth * 2.0 - 1.0;
    vec4 previous = clipToPreviousClip * vec4(currentClip, clipZ, 1.0);
    vec2 motion = vec2(0.0);
    // Disoccluded/invalid projections receive zero motion; the temporal backend
    // still needs its own depth/disocclusion handling. No dense-dynamic claim.
    if (depth >= 0.0 && depth <= 1.0 && previous.w > 1e-7 && previous.w < 1e20)
    {
        vec2 delta = previous.xy / previous.w - currentClip;
        motion = delta * vec2(0.5, -0.5) * renderSize;
        if (!(abs(motion.x) < 65504.0 && abs(motion.y) < 65504.0))
            motion = vec2(0.0);
    }
    gl_FragColor = vec4(motion, 0.0, 1.0);
}
