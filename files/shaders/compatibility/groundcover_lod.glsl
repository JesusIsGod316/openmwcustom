// Same policy as SceneUtil::GroundcoverPolicy::density. Distances are in game units;
// projected radius is normalized to the vertical view extent (not output resolution).
float p8g3Density(float distanceToInstance, float projectedRadius, vec3 policy)
{
    float distanceDensity = 1.0 - (1.0 - policy.z) * smoothstep(policy.x, policy.y, distanceToInstance);
    float protectProminent = smoothstep(0.008, 0.025, projectedRadius);
    return mix(distanceDensity, 1.0, protectProminent);
}
