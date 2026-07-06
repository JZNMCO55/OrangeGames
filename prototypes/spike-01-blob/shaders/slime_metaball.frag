#version 450

// Tier 2 SDF metaball slime — M1: 多质点密度场出连续平滑剪影（跟随真 blob）。
// 逐像素对 count 个质点（14 perimeter + 1 centroid，世界坐标）投影到 NDC 求 falloff
// 累加，smoothstep 阈值出剪影。M1 只出纯绿 mask，着色留 M2。
//
// 度量说明：质点位置在原始投影 NDC 比较（viewProj 已含正交宽高比，位置正确）；但
// falloff 距离度量须 screen-isotropic —— 正交把世界圆投成 NDC 椭圆（y 比 x 高 aspect
// 倍），若用各向同性 NDC 距离，融合半径在屏幕上成椭圆 → blob 变形。故对差值的 x 分量
// ×aspect（换算成屏幕高度比例的度量），使剪影在屏幕上为正圆。falloff 半径由 CPU 按
// blob 投影尺寸算好传入（uParams0.w），随缩放 / blobRadius 自适应。

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

layout(std140, set = 0, binding = 0) uniform SlimeUBO
{
    mat4 uViewProj;   // world -> clip（主相机）
    vec4 uParams0;    // x=count, y=isoLevel, z=aspect, w=falloffRadius(ndc-y 单位)
    vec4 uPoints[16]; // xy=world pos（末尾一个是 centroid，用于填实内部）
} U;

void main()
{
    vec2  pix     = vUv * 2.0 - 1.0; // 像素 NDC
    int   count   = int(U.uParams0.x);
    float iso     = U.uParams0.y;
    float aspect  = U.uParams0.z;
    float falloff = max(U.uParams0.w, 1e-4);

    float field = 0.0;
    for (int i = 0; i < count; ++i)
    {
        vec4 clip = U.uViewProj * vec4(U.uPoints[i].xy, 0.0, 1.0);
        vec2 pndc = clip.xy / clip.w;
        vec2 d    = pix - pndc;
        d.x *= aspect; // screen-isotropic 度量（见顶注）
        float r = length(d) / falloff;
        if (r < 1.0)
        {
            float a = 1.0 - r;
            field += a * a; // 二次衰减，同 particle_field / M0
        }
    }

    float mask = smoothstep(iso - 0.05, iso, field);
    if (mask <= 0.001)
    {
        discard;
    }

    outColor = vec4(0.2, 0.9, 0.35, mask); // M1 纯绿 mask；着色留 M2
}
