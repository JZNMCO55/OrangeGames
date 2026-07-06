#version 450

// Tier 2 SDF metaball slime — M2: 假法线 + gel 材质着色。
//
// M1 的纯绿 mask → 有 3D 圆润感的绿胶质 gel（迁移 Tier1 ShadeGel 到逐像素）。
// 剪影仍由 metaball 密度场决定（M1，随 blob 形变）；着色部分是 M2 新增。
//
// 假法线来源：**显式半球**（以 centroid 为心、按 blob 投影半径缩放），不用 field
// 梯度——因为 15 质点求和的 density field 非单调（在 perimeter 环处冲高、圆心反而
// 略低），梯度法线会在环处鼓包、读成"平盘带凸环"而非圆润穹顶。显式半球直接复刻
// Tier1 ShadeGel 的 dome 法线（径向方向 + 半球高度），静止圆读成圆润半球。
//
// 度量 / 朝向：像素与质点在原始投影 NDC 比较（viewProj 已含正交宽高比）；falloff 与
// 半球距离度量对差值 x 分量 ×aspect 做 screen-isotropic（见 M1）。Vulkan NDC y-down，
// 光照用 Tier1 的 y-up 约定，故构造法线时把 y 分量取负转到 y-up。
// 输出 linear HDR（不 tonemap、不上钳）：Fresnel/高光亮部超 bloom 阈值自然发光。

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

layout(std140, set = 0, binding = 0) uniform SlimeUBO
{
    mat4 uViewProj;   // world -> clip（主相机）
    vec4 uParams0;    // x=count, y=isoLevel, z=aspect, w=falloffRadius(ndc-y)
    vec4 uParams1;    // x=blobNdcRadius, y=domeScale, z=rimGain, w=specGain
    vec4 uParams2;    // x=ambient
    vec4 uPoints[16]; // xy=world pos（末尾一个是 centroid）
} U;

// gel 材质常量 —— 迁移自 main.cpp Tier1 SlimeParams（准确值）。
const vec3  kDeepColor = vec3(0.02, 0.16, 0.05);  // 厚处深绿（核心 / SSS 暗端）
const vec3  kBodyColor = vec3(0.11, 0.62, 0.20);  // 主体绿（薄处 / 边亮端）
const vec3  kRimColor  = vec3(0.55, 1.00, 0.55);  // Fresnel 亮边（黄绿）
const vec3  kLightDir  = vec3(-0.35, 0.85, 0.42); // 主光方向（左上偏前，y-up）
const float kRimPower  = 3.0;  // Fresnel 指数
const float kSpecPower = 32.0; // Blinn 高光锐度

vec2 ProjectNdc(vec2 world)
{
    vec4 clip = U.uViewProj * vec4(world, 0.0, 1.0);
    return clip.xy / clip.w;
}

void main()
{
    vec2  pix      = vUv * 2.0 - 1.0; // 像素 NDC（y-down）
    int   count    = int(U.uParams0.x);
    float iso      = U.uParams0.y;
    float aspect   = U.uParams0.z;
    float falloff  = max(U.uParams0.w, 1e-4);
    float blobR    = U.uParams1.x;
    float domeR    = max(U.uParams1.y * blobR, 1e-4); // 半球半径 = domeScale × blob 投影半径
    float rimGain  = U.uParams1.z;
    float specGain = U.uParams1.w;
    float ambient  = U.uParams2.x;

    // --- 剪影：metaball 密度场（M1，随 blob 形变）---
    float field = 0.0;
    for (int i = 0; i < count; ++i)
    {
        vec2  d = pix - ProjectNdc(U.uPoints[i].xy);
        d.x *= aspect;
        float r = length(d) / falloff;
        if (r < 1.0)
        {
            float a = 1.0 - r;
            field += a * a;
        }
    }
    float mask = smoothstep(iso - 0.05, iso, field);
    if (mask <= 0.001)
    {
        discard;
    }

    // --- 假法线：以 centroid（末位质点）为心的显式半球 ---
    vec2  centroidNdc = ProjectNdc(U.uPoints[count - 1].xy);
    vec2  dc          = pix - centroidNdc;
    dc.x *= aspect;                               // screen-isotropic metric（y-down）
    float dcLen   = length(dc);
    float rr      = clamp(dcLen / domeR, 0.0, 1.0); // 径向分数：0 圆心 → 1 边缘
    vec2  outward = (dcLen > 1e-6) ? normalize(vec2(dc.x, -dc.y)) : vec2(0.0); // 转 y-up
    vec3  N       = vec3(rr * outward, sqrt(max(0.0, 1.0 - rr * rr)));         // dome 法线

    // --- gel 着色（Tier1 ShadeGel 逐像素）---
    vec3  L    = normalize(kLightDir);
    vec3  V    = vec3(0.0, 0.0, 1.0);
    vec3  H    = normalize(L + V);
    float diff = dot(N, L) * 0.5 + 0.5; // 半 Lambert：次表面通透感，暗部不死黑（gel 是 SSS 主导非漫反主导）
    float spec = pow(clamp(dot(N, H), 0.0, 1.0), kSpecPower);
    float fres = pow(1.0 - clamp(N.z, 0.0, 1.0), kRimPower);

    // 厚度 SSS：中心厚=深绿、边薄=亮体色；rr² 让深绿只集中在核心、大片保持通透亮绿。
    vec3 col = mix(kDeepColor, kBodyColor, rr * rr);
    col *= (ambient + (1.0 - ambient) * diff); // dome 漫反（半 Lambert）
    col += kRimColor * (fres * rimGain);        // Fresnel 亮边（喂 bloom 发光）
    col += vec3(1.0) * (spec * specGain);       // 湿润高光
    col = max(col, vec3(0.0));                  // linear HDR，不上钳（喂 bloom）

    outColor = vec4(col, mask); // 核心实(mask≈1)、边缘透；alpha blend 已配好
}
