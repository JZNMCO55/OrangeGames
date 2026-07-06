#version 450

// Tier 2 SDF metaball slime fragment shader — M0 pipeline-proof version.
//
// M0 only proves the whole RHI path (pipeline/descriptor/UBO/blend/layout) works:
// per-pixel density field of a single hardcoded point yields one smooth circle,
// alpha-blended onto the HDR scene. Not pretty, no real blob data yet.
//
// M0 works directly in NDC (no viewProj); uViewProj stays reserved for M1 when
// real world-space blob points get projected in.

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

layout(std140, set = 0, binding = 0) uniform SlimeUBO
{
    mat4 uViewProj;      // reserved for M1 (world -> clip)
    vec4 uParams0;       // x=count, y=isoLevel, z=aspect, w=time
    vec4 uPoints[16];    // xy=NDC pos, z=radius, w=weight (unused in M0)
} U;

void main()
{
    // Fullscreen NDC with aspect correction so the circle reads round on screen.
    vec2 p = vUv * 2.0 - 1.0;
    p.x *= U.uParams0.z;

    float field = 0.0;
    int   count = int(U.uParams0.x);
    for (int i = 0; i < count; ++i)
    {
        vec2  c = U.uPoints[i].xy;
        float r = length(p - c) / U.uPoints[i].z;
        if (r < 1.0)
        {
            float a = 1.0 - r;
            field += a * a;   // quadratic falloff, same core as particle_field
        }
    }

    float mask = smoothstep(U.uParams0.y - 0.05, U.uParams0.y, field);
    if (mask <= 0.001)
    {
        discard;
    }

    // M0: flat green just to prove the composite. Alpha-blended (SrcAlpha/1-SrcAlpha).
    outColor = vec4(0.2, 0.9, 0.35, mask);
}
