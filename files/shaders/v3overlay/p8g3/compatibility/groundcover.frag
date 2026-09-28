#version 120

#if @useGPUShader4
    #extension GL_EXT_gpu_shader4: require
#endif

#include "lib/core/fragment.h.glsl"

#define GROUNDCOVER

#if @diffuseMap
uniform sampler2D diffuseMap;
#endif
varying vec2 diffuseMapUV;

#if @normalMap
uniform sampler2D normalMap;
varying vec2 normalMapUV;
#endif

#define PER_PIXEL_LIGHTING 1

#if @optimizedmwGroundcoverLod
varying float p8g3Coverage;
#endif
varying float euclideanDepth;
varying float linearDepth;
varying vec3 passViewPos;

uniform vec2 screenRes;
uniform float near;
uniform float far;
uniform float alphaRef;

#if PER_PIXEL_LIGHTING
#include "lib/light/clamp.glsl"
#else
centroid varying vec3 passLighting;
centroid varying vec3 sssLight;
centroid varying vec3 shadowDiffuseLighting;
#endif
varying float localHeight;

varying vec3 passNormal;

#define PIXEL_SHADER 1
#include "lighting.glsl"
#include "shadows_fragment.glsl"
#include "lib/material/alpha.glsl"
#include "fog.glsl"
#include "compatibility/normals.glsl"

void main()
{
    Material material = getMaterial();

    float ambientVisibility = 1.0;
    float sssIntensity = 1.0;
    vec3 sssColor = vec3(0.0);

#if @diffuseMap
    gl_FragData[0] = texture2D(diffuseMap, diffuseMapUV);
    #if PBR_ENABLED && GROUNDCOVER_SSS
    {
        #if GROUNDCOVER_AUTO_PBR
        float luminance = dot(gl_FragData[0].rgb, vec3(0.2126, 0.7152, 0.0722));
        sssIntensity = mix(1.0, smoothstep(-35.0, 5.0, localHeight), clamp(luminance * 4.0, 0.0, 1.0));
        ambientVisibility = sqrt(sssIntensity) * 0.5 + 0.5;
        // ambientVisibility = (smoothstep(-45.0, 35.0, localHeight));
        if (luminance > 0.0)
        {
            sssColor = gl_FragData[0].rgb / luminance * (1.0 - exp(-luminance)) * (1.2 - luminance * 0.3);
            // sssColor = gl_FragData[0].rgb / luminance * (1.0 - exp(-luminance)) * (1.5 - luminance);
            float minSSS = min(sssColor.r, min(sssColor.g, sssColor.b));
            float maxSSS = max(sssColor.r, max(sssColor.g, sssColor.b));
            sssColor *= smoothstep(0.07 - smoothstep(0.12, 0.0, luminance), 0.1, maxSSS - minSSS);
        }
        else
        #endif
            sssColor = gl_FragData[0].rgb;
    }
    #endif // PBR_ENABLED && GROUNDCOVER_SSS
#else
    gl_FragData[0] = vec4(1.0);
#endif

    float fade = 0.0;
    if (euclideanDepth > @groundcoverFadeStart)
    {
        #if PBR_ENABLED && GROUNDCOVER_FADE
            fade = clamp((euclideanDepth - @groundcoverFadeStart) / (@groundcoverFadeEnd - @groundcoverFadeStart), 0.0, 1.0);//smoothstep(500, 1000, euclideanDepth);
            ivec2 pixel = ivec2(gl_FragCoord.xy);
            int bayerIndex = (pixel.x % 4) + (pixel.y % 4) * 4;
            const float bayerMatrix[16] = float[16](
                0.0 / 16.0,  8.0 / 16.0,  2.0 / 16.0, 10.0 / 16.0,
                12.0 / 16.0,  4.0 / 16.0, 14.0 / 16.0,  6.0 / 16.0,
                3.0 / 16.0, 11.0 / 16.0,  1.0 / 16.0,  9.0 / 16.0,
                15.0 / 16.0,  7.0 / 16.0, 13.0 / 16.0,  5.0 / 16.0
            );
            if (bayerMatrix[bayerIndex] < fade)
                gl_FragData[0].a = 0.0;
        #else
            gl_FragData[0].a *= 1.0-smoothstep(@groundcoverFadeStart, @groundcoverFadeEnd, euclideanDepth);
        #endif // PBR_ENABLED && GROUNDCOVER_FADE
    }

    float alpha = gl_FragData[0].a;
    #if PBR_ENABLED && GROUNDCOVER_EDGE_REFINEMENT
    vec4 derivs = vec4(dFdx(diffuseMapUV), dFdy(diffuseMapUV));
    derivs.x = dot(derivs.xy, derivs.xy);
    derivs.y = dot(derivs.zw, derivs.zw);
    derivs.x = max(derivs.x, derivs.y);
    derivs.x = clamp(derivs.x * 200000.0, 0.0, 1.0);
    alpha = mix(alpha, 1.0, derivs.x * 0.35);
    #endif

#if @optimizedmwGroundcoverLod
    // Apply after PBR edge refinement: that filter must not resurrect a
    // fully removed instance. All normal PBR lighting/SSS/fog is retained.
    alpha *= p8g3Coverage;
#endif
    gl_FragData[0].a = alphaTest(alpha, alphaRef);

#if @normalMap
    vec4 normalTex = texture2D(normalMap, normalMapUV);
    vec3 normal = normalTex.xyz * 2.0 - 1.0;
#if @reconstructNormalZ
    normal.z = sqrt(1.0 - dot(normal.xy, normal.xy));
#endif
    #if PBR_ENABLED
        ambientVisibility = min(ambientVisibility, normalTex.a);
    #endif // PBR_ENABLED
    vec3 viewNormal = normalToView(normal);
#else
    vec3 viewNormal = normalToView(normalize(passNormal));
    #if PBR_ENABLED
        if (!gl_FrontFacing) // some groundcover mods don't even respect this, they just have 100% flipped normals, or just weirdly pointing up
            viewNormal = -viewNormal;
    #endif // PBR_ENABLED
#endif // @normalMap

    float shadowing = unshadowedLightRatio(euclideanDepth
        #ifdef ADAPTIVE_RADIUS
        , CalculateSunStrength()
        #endif
    );

    vec3 lighting;
#if !PER_PIXEL_LIGHTING
    lighting = ambientVisibility * passLighting + shadowDiffuseLighting * shadowing;
#else
    vec3 diffuseLight, ambientLight, specularLight, sssLight;
    ProcessLighting(passViewPos, viewNormal, material.shininess, shadowing, false, gl_FragData[0].rgb, vec4(-1.0, -1.0, ambientVisibility, 1.0), vec3(1.0), diffuseLight, ambientLight, specularLight, sssLight);
    lighting = diffuseLight + ambientLight;
    clampLighting(lighting);
#endif

    gl_FragData[0].xyz *= lighting;
#if PBR_ENABLED && GROUNDCOVER_SSS
    gl_FragData[0].rgb += shadowing * sssLight * sssColor * sssIntensity;
#endif // PBR_ENABLED && GROUNDCOVER_SSS

    // gl_FragData[0].xyz *= mix(clamp(gl_FragData[0].a + vec3(0.25), vec3(0.0), vec3(1.0)), vec3(1.0), sqrt(fade));
    // gl_FragData[0].rgb *= smoothstep(alphaRef - 0.2, alphaRef + 0.1, gl_FragData[0].a);
    gl_FragData[0] = applyFogAtDist(gl_FragData[0], euclideanDepth, linearDepth, near, far);

#if !@disableNormals
    gl_FragData[1].xyz = viewNormal * 0.5 + 0.5;
#endif

#if PBR_ENABLED
#if LIGHTING_DEBUG_MODE == 1
    gl_FragData[0].rgb = vec3(shadowing);
#elif LIGHTING_DEBUG_MODE == 2
    gl_FragData[0].rgb = vec3(ambientVisibility);
#elif LIGHTING_DEBUG_MODE == 3
    gl_FragData[0].rgb = sssLight * sssColor * sssIntensity;
#elif LIGHTING_DEBUG_MODE == 4
    gl_FragData[0].rgb = vec3(diffuseMapUV.xy, 0.0);
#endif // LIGHTING_DEBUG_MODE
#endif // PBR_ENABLED

    applyShadowDebugOverlay();
}
// v2.0e
