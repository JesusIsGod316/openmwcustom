#version 120

#if @useGPUShader4
    #extension GL_EXT_gpu_shader4: require
#endif

#include "lib/core/vertex.h.glsl"

attribute vec4 aOffset;
#if @optimizedmwGroundcoverLod
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

#define PER_PIXEL_LIGHTING 1

varying float euclideanDepth;
varying float linearDepth;
varying vec3 passViewPos;

#if !PER_PIXEL_LIGHTING
centroid varying vec3 passLighting;
centroid varying vec3 sssLight;
centroid varying vec3 shadowDiffuseLighting;
#include "lib/light/clamp.glsl"
#endif
varying float localHeight;

varying vec3 passNormal;

#define GROUNDCOVER 1
#define VERTEX_SHADER 1
#include "lighting.glsl"
#include "shadows_vertex.glsl"
#include "compatibility/normals.glsl"
#include "lib/view/depth.glsl"

uniform float osg_SimulationTime;
uniform mat4 osg_ViewMatrix;
uniform float windSpeed;
uniform vec3 playerPos;

#if @groundcoverStompMode == 0
#else
    #define STOMP 1
    #if @groundcoverStompMode == 2
        #define STOMP_HEIGHT_SENSITIVE 1
    #endif
    #define STOMP_INTENSITY_LEVEL @groundcoverStompIntensity
#endif

vec2 Gust(vec2 x)
{
    vec2 ccx = max(vec2(0.0), cos(x));
    return ccx * ccx * 0.8 + sin(x) * 0.4;
}

vec2 groundcoverDisplacement(in vec3 worldPos, float h)
{
    vec2 windDirection = vec2(1.0);
    vec3 footPos = playerPos;
    vec3 windVec = vec3((windSpeed + 0.3) * windDirection, 1.0);

    float v = length(windVec);
    vec2 displace = vec2(2.0 * windVec + 0.1);
    vec2 harmonics = vec2(0.0);

    float t = osg_SimulationTime - dot(worldPos.xy, normalize(windDirection)) * 0.005;
    // return vec2((1.0 + 0.53 * v) * vec2(Gust(vec2(3.7 * t))));

    harmonics += vec2((1.0 - 0.13 * v) * sin(2.3 * t + worldPos.xy / 990.0));
#if !@optimizedmwGroundcoverFastWind
    harmonics += vec2((1.0 - 0.17 * v) * cos(4.4 * t + worldPos.xy / 740.0));
#endif
    harmonics += vec2((1.0 + 0.41 * v) * Gust(3.7 * t + worldPos.xy / 49.0));
    // harmonics += vec2((1.0 + 0.28 * v) * sin(5.1 * t + worldPos.xy / 200.0));
    // harmonics += vec2(cos(harmonics.x * harmonics.y));

    vec2 stomp = vec2(0.0);
#if STOMP
    float d = length(worldPos.xy - footPos.xy);
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
    if (d < STOMP_RANGE && d > 0.0)
        stomp = (STOMP_DISTANCE / d - STOMP_DISTANCE / STOMP_RANGE) * (worldPos.xy - footPos.xy);

#ifdef STOMP_HEIGHT_SENSITIVE
    stomp *= clamp((worldPos.z - footPos.z) / h, 0.0, 1.0);
#endif
#endif

    displace = harmonics * displace + stomp;
    displace = displace * smoothstep(2500.0f, 500.0f, distance(worldPos, footPos)) + windDirection * 40.0 * windSpeed;
    return clamp(0.02 * h, 0.0, 1.0) * displace;
}

mat4 rotation(in vec3 angle)
{
    float sinX = sin(angle.x);
    float cosX = cos(angle.x);
    float sinY = sin(angle.y);
    float cosy = cos(angle.y);
    float sinZ = sin(angle.z);
    float cosZ = cos(angle.z);

    return mat4(
         cosZ * cosy + sinX * sinY * sinZ, -sinZ * cosX, cosZ * sinY + sinZ * sinX * cosy, 0.0,
         sinZ * cosy + cosZ * sinX * sinY,  cosZ * cosX, sinZ * sinY - cosZ * sinX * cosy, 0.0,
        -sinY * cosX,                       sinX,        cosX * cosy,                      0.0,
         0.0,                               0.0,         0.0,                              1.0);
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
    Material material = getMaterial();

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

#if @optimizedmwGroundcoverLod
    mat4 rotation = rotation(aRotation.xyz);
#else
    mat4 rotation = rotation(aRotation);
#endif
    vec4 displacedVertex = rotation * scale * gl_Vertex;

    displacedVertex = vec4(displacedVertex.xyz + position, 1.0);

    vec4 worldPos = osg_ViewMatrixInverse * gl_ModelViewMatrix * displacedVertex;
    worldPos.xy += groundcoverDisplacement(worldPos.xyz, gl_Vertex.z);
    vec4 viewPos = osg_ViewMatrix * worldPos;

    gl_ClipVertex = viewPos;
    euclideanDepth = length(viewPos.xyz);

    if (length(gl_ModelViewMatrix * vec4(position, 1.0)) > @groundcoverFadeEnd)
        gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
    else
        gl_Position = viewToClip(viewPos);

    linearDepth = getLinearDepth(gl_Position.z, viewPos.z);

    passNormal = rotation3(rotation) * gl_Normal.xyz;
    normalToViewMatrix = gl_NormalMatrix;
#if @normalMap
    normalToViewMatrix *= generateTangentSpace(gl_MultiTexCoord7.xyzw * rotation, passNormal);
#endif

#if (!PER_PIXEL_LIGHTING || @shadows_enabled)
    vec3 viewNormal = normalize(gl_NormalMatrix * passNormal);
#endif

#if @diffuseMap
    diffuseMapUV = (texMat@diffuseMapUV * gl_MultiTexCoord@diffuseMapUV).xy;
#endif

#if @normalMap
    normalMapUV = (texMat@normalMapUV * gl_MultiTexCoord@normalMapUV).xy;
#endif

    localHeight = gl_Vertex.z;
    passViewPos = viewPos.xyz;
#if !PER_PIXEL_LIGHTING
    vec3 diffuseLight, ambientLight, specularLight;
    vec3 unusedShadowSpecular;
    ProcessLighting(viewPos.xyz, viewNormal, material.shininess, diffuseLight, ambientLight, specularLight, sssLight, shadowDiffuseLighting, unusedShadowSpecular);
    passLighting = diffuseLight + ambientLight;
    clampLighting(passLighting);
    // clampLighting(sssLight);
#endif

#if (@shadows_enabled)
    setupShadowCoords(viewPos, viewNormal);
#endif
}
// v2.0e
