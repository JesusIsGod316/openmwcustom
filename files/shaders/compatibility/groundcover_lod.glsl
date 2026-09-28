// Conservative singular-value bound. Columns are OSG's row-vector basis after
// upload. A small guard covers shader floating point; the CPU uses a larger guard.
float p8g4TransformBound(mat4 m)
{
    vec3 a = m[0].xyz, b = m[1].xyz, c = m[2].xyz;
    float ab = abs(dot(a,b)), ac = abs(dot(a,c)), bc = abs(dot(b,c));
    float gram = max(dot(a,a)+ab+ac, max(dot(b,b)+ab+bc, dot(c,c)+ac+bc));
    float frobenius = dot(a,a) + dot(b,b) + dot(c,c);
    return sqrt(min(gram, frobenius)) * 1.0001;
}

// Same policy as SceneUtil::GroundcoverPolicy::density. Distances are in game units;
// projected radius is normalized to the vertical view extent (not output resolution).
float p8g3Density(float distanceToInstance, float projectedRadius, vec3 policy)
{
    float distanceDensity = 1.0 - (1.0 - policy.z) * smoothstep(policy.x, policy.y, distanceToInstance);
    float protectProminent = smoothstep(0.008, 0.025, projectedRadius);
    return mix(distanceDensity, 1.0, protectProminent);
}
