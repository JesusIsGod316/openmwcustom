#version 120

#if @useGPUShader4
    #extension GL_EXT_gpu_shader4: require
#endif

#include "lib/core/vertex.h.glsl"

#define GROUNDCOVER

attribute vec4 aOffset;
#if @optimizedmwGroundcoverLod
// Rank occupies the previously unused fourth component of slot 7, never a new UV-aliased slot.
attribute vec4 aRotation;
varying float p8g3Coverage;
uniform vec4 p8g3LodParams;
uniform mat4 projectionMatrix;
#include "groundcover_lod.glsl"
#else
attribute vec3 aRotation;
#endif

#if @diffuseMap
varying vec2 diffuseMapUV;
uniform mat4 texMat@diffuseMapUV;
#endif

#if @normalMap
varying vec2 normalMapUV;
uniform mat4 texMat@normalMapUV;
#endif

// Other shaders respect forcePPL, but legacy groundcover mods were designed to work with vertex lighting.
// They may do not look as intended with per-pixel lighting, so ignore this setting for now.
#define PER_PIXEL_LIGHTING @normalMap

varying float euclideanDepth;
varying float linearDepth;

#if PER_PIXEL_LIGHTING
varying vec3 passViewPos;
#else
centroid varying vec3 shadedLighting;
centroid varying vec3 passLighting;
#include "lib/light/clamp.glsl"
#endif

varying vec3 passNormal;

#include "shadows_vertex.glsl"
#include "compatibility/normals.glsl"
#include "lib/view/depth.glsl"

uniform float osg_SimulationTime;
uniform mat4 osg_ViewMatrixInverse;
uniform mat4 osg_ViewMatrix;
uniform float windSpeed;
uniform vec4 groundcoverWindCoefficients;
uniform vec3 playerPos;

centroid varying vec4 passColor;

#if @groundcoverStompMode == 0
#else
    #define STOMP 1
    #if @groundcoverStompMode == 2
        #define STOMP_HEIGHT_SENSITIVE 1
    #endif
    #define STOMP_INTENSITY_LEVEL @groundcoverStompIntensity
#endif

vec2 groundcoverDisplacement(in vec3 worldpos, float h)
{
    vec3 footPos = playerPos;
    vec2 harmonics = vec2(0.0);

#if @optimizedmwGroundcoverGpuPath >= 1
#if @optimizedmwGroundcoverFastWind
    // Deliberately visual-risk ceiling probe: keep the largest and finest wind terms.
    harmonics += vec2(groundcoverWindCoefficients.x
        * sin(1.0*osg_SimulationTime + worldpos.xy / 1100.0));
    harmonics += vec2(groundcoverWindCoefficients.w
        * sin(5.0*osg_SimulationTime + worldpos.xy / 200.0));
#else
    // Same algebra as the established path, with uniform wind-only terms
    // precomputed once per frame instead of once per vertex.
    harmonics += vec2(groundcoverWindCoefficients.x
        * sin(1.0*osg_SimulationTime + worldpos.xy / 1100.0));
    harmonics += vec2(groundcoverWindCoefficients.y
        * cos(2.0*osg_SimulationTime + worldpos.xy / 750.0));
    harmonics += vec2(groundcoverWindCoefficients.z
        * sin(3.0*osg_SimulationTime + worldpos.xy / 500.0));
    harmonics += vec2(groundcoverWindCoefficients.w
        * sin(5.0*osg_SimulationTime + worldpos.xy / 200.0));
#endif
#else
    vec2 windDirection = vec2(1.0);
    vec3 windVec = vec3(windSpeed * windDirection, 1.0);
    float v = length(windVec);
    vec2 displace = vec2(2.0 * windVec + 0.1);

    harmonics += vec2((1.0 - 0.10*v) * sin(1.0*osg_SimulationTime + worldpos.xy / 1100.0));
#if !@optimizedmwGroundcoverFastWind
    harmonics += vec2((1.0 - 0.04*v) * cos(2.0*osg_SimulationTime + worldpos.xy / 750.0));
    harmonics += vec2((1.0 + 0.14*v) * sin(3.0*osg_SimulationTime + worldpos.xy / 500.0));
#endif
    harmonics += vec2((1.0 + 0.28*v) * sin(5.0*osg_SimulationTime + worldpos.xy / 200.0));
    harmonics *= displace;
#endif

    vec2 stomp = vec2(0.0);
#if STOMP
#if STOMP_INTENSITY_LEVEL == 0
    // Gentle intensity
    const float STOMP_RANGE = 50.0; // maximum distance from player that grass is affected by stomping
    const float STOMP_DISTANCE = 20.0; // maximum distance stomping can move grass
#elif STOMP_INTENSITY_LEVEL == 1
    // Reduced intensity
    const float STOMP_RANGE = 80.0;
    const float STOMP_DISTANCE = 40.0;
#elif STOMP_INTENSITY_LEVEL == 2
    // MGE XE intensity
    const float STOMP_RANGE = 150.0;
    const float STOMP_DISTANCE = 60.0;
#endif

#if @optimizedmwGroundcoverGpuPath >= 1
    vec2 stompDelta = worldpos.xy - footPos.xy;
    float stompDistanceSquared = dot(stompDelta, stompDelta);
    if (stompDistanceSquared < STOMP_RANGE * STOMP_RANGE && stompDistanceSquared > 0.0)
    {
        float d = sqrt(stompDistanceSquared);
        stomp = (STOMP_DISTANCE / d - STOMP_DISTANCE / STOMP_RANGE) * stompDelta;
    }
#else
    float d = length(worldpos.xy - footPos.xy);
    if (d < STOMP_RANGE && d > 0.0)
        stomp = (STOMP_DISTANCE / d - STOMP_DISTANCE / STOMP_RANGE) * (worldpos.xy - footPos.xy);
#endif

#ifdef STOMP_HEIGHT_SENSITIVE
    stomp *= clamp((worldpos.z - footPos.z) / h, 0.0, 1.0);
#endif
#endif

    return clamp(0.02 * h, 0.0, 1.0) * (harmonics + stomp);
}

mat4 rotation(in vec3 angle)
{
    float sin_x = sin(angle.x);
    float cos_x = cos(angle.x);
    float sin_y = sin(angle.y);
    float cos_y = cos(angle.y);
    float sin_z = sin(angle.z);
    float cos_z = cos(angle.z);

    return mat4(
        cos_z*cos_y+sin_x*sin_y*sin_z, -sin_z*cos_x, cos_z*sin_y+sin_z*sin_x*cos_y, 0.0,
        sin_z*cos_y+cos_z*sin_x*sin_y, cos_z*cos_x, sin_z*sin_y-cos_z*sin_x*cos_y, 0.0,
        -sin_y*cos_x, sin_x, cos_x*cos_y, 0.0,
        0.0, 0.0, 0.0, 1.0);
}

mat3 rotation3(in mat4 rot4)
{
    return mat3(
        rot4[0].xyz,
        rot4[1].xyz,
        rot4[2].xyz);
}

void main(void)
{
    vec3 position = aOffset.xyz;
    float scale = aOffset.w;

#if @optimizedmwGroundcoverLod
    p8g3Coverage = 1.0;
    if (p8g3LodParams.x > 0.0)
    {
        vec4 baseView = gl_ModelViewMatrix * vec4(position, 1.0);
        float instanceDistance = length(baseView.xyz);
        float transformBound = p8g4TransformBound(gl_ModelViewMatrix);
        float projectedRadius = p8g3LodParams.w * transformBound * abs(projectionMatrix[1][1])
            / max(1.0, instanceDistance);
        float density = p8g3Density(instanceDistance, projectedRadius, p8g3LodParams.xyz);
        p8g3Coverage = clamp((density + 0.05 - aRotation.w) / 0.05, 0.0, 1.0);
        if (p8g3Coverage <= 0.0)
        {
            gl_ClipVertex = baseView;
            gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
            return;
        }
    }
#endif

#if @optimizedmwGroundcoverGpuPath >= 1
    vec4 instanceBaseViewPos = gl_ModelViewMatrix * vec4(position, 1.0);
    if (dot(instanceBaseViewPos.xyz, instanceBaseViewPos.xyz)
        > @groundcoverFadeEnd * @groundcoverFadeEnd)
    {
        // Same degenerate output used by the established path, but before
        // rotation, wind and lighting work for the rejected instance.
        gl_ClipVertex = instanceBaseViewPos;
        gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
#endif

    Material material = getMaterial();

#if @optimizedmwGroundcoverGpuPath >= 1
    mat4 instanceRotation;
    if (aRotation.x == 0.0 && aRotation.y == 0.0)
    {
        float sin_z = sin(aRotation.z);
        float cos_z = cos(aRotation.z);
        instanceRotation = mat4(
            cos_z, -sin_z, 0.0, 0.0,
            sin_z,  cos_z, 0.0, 0.0,
            0.0,    0.0,   1.0, 0.0,
            0.0,    0.0,   0.0, 1.0);
    }
    else
        instanceRotation = rotation(aRotation.xyz);
    mat4 rotation = instanceRotation;
#else
    mat4 rotation = rotation(aRotation.xyz);
#endif
    vec4 displacedVertex = rotation * scale * gl_Vertex;

    displacedVertex = vec4(displacedVertex.xyz + position, 1.0);

    vec4 worldPos = osg_ViewMatrixInverse * gl_ModelViewMatrix * displacedVertex;
    worldPos.xy += groundcoverDisplacement(worldPos.xyz, gl_Vertex.z);
    vec4 viewPos = osg_ViewMatrix * worldPos;

    gl_ClipVertex = viewPos;
    euclideanDepth = length(viewPos.xyz);

#if @optimizedmwGroundcoverGpuPath >= 1
    gl_Position = viewToClip(viewPos);
#else
    if (length(gl_ModelViewMatrix * vec4(position, 1.0)) > @groundcoverFadeEnd)
        gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
    else
        gl_Position = viewToClip(viewPos);
#endif

    linearDepth = getLinearDepth(gl_Position.z, viewPos.z);

    passNormal = rotation3(rotation) * gl_Normal.xyz;
    normalToViewMatrix = gl_NormalMatrix;
#if @normalMap
    normalToViewMatrix *= generateTangentSpace(gl_MultiTexCoord7.xyzw * rotation, passNormal);
#endif

#if (!PER_PIXEL_LIGHTING || (@shadows_enabled && @optimizedmwGroundcoverShadowReceive))
    vec3 viewNormal = normalize(gl_NormalMatrix * passNormal);
#endif

#if @diffuseMap
    diffuseMapUV = (texMat@diffuseMapUV * gl_MultiTexCoord@diffuseMapUV).xy;
#endif

#if @normalMap
    normalMapUV = (texMat@normalMapUV * gl_MultiTexCoord@normalMapUV).xy;
#endif

#if PER_PIXEL_LIGHTING
    passViewPos = viewPos.xyz;
#else
    float shininess = max(1e-4, material.shininess);
    vec3 viewDir = viewPos.xyz / euclideanDepth;

    vec3 sunDiffuse, sunAmbient, unusedSpecular1, pointDiffuse, pointAmbient, unusedSpecular2;
    directionalLighting(viewDir, viewNormal, shininess, sunDiffuse, sunAmbient, unusedSpecular1);
    pointLighting(clipToScreen(gl_Position), viewDir, viewPos.xyz, viewNormal, shininess, pointDiffuse, pointAmbient, unusedSpecular2);
    shadedLighting = pointDiffuse + pointAmbient + sunAmbient;
    passLighting = shadedLighting + sunDiffuse;
    clampLighting(shadedLighting);
    clampLighting(passLighting);
#endif

#if (@shadows_enabled && @optimizedmwGroundcoverShadowReceive)
    setupShadowCoords(viewPos, viewNormal);
#endif
}
