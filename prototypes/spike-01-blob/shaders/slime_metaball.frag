#version 450

// Tier 2 SDF metaball slime — M3: 角色元素 + Fresnel/通透 polish（逼近参考图）。
//
// M2 的圆润绿 gel 半球 → 有生命的史莱姆：两只发光暖黄眼 + 体内密集金光斑 + 底部绿
// 接触辉光 + 明显黄绿 Fresnel 亮边。剪影(M1) + 显式半球法线(M2, 半 Lambert + rr² SSS)
// 不动，本文件只加着色元素。
//
// 眼/光斑/辉光在 blob 局部帧摆位（原点 = centroid，单位 = blobNdcRadius，屏幕 up 朝向，
// 不随体旋转）；几何/颜色抄 main.cpp Tier1 EmitSlimeGel（准确值）。全部 emissive(值可 >1)，
// 亮部超 bloom 阈值(0.72)自然发光。输出 linear HDR，不 tonemap、不上钳。

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

layout(std140, set = 0, binding = 0) uniform SlimeUBO
{
    mat4 uViewProj;   // world -> clip（主相机）
    vec4 uParams0;    // x=count, y=isoLevel, z=aspect, w=falloffRadius(ndc-y)
    vec4 uParams1;    // x=blobNdcRadius, y=domeScale, z=rimGain, w=specGain
    vec4 uParams2;      // x=ambient, y=time, z=eyeGain, w=speckGain
    vec4 uParams3;      // x=glowGain, y=dropletCount
    vec4 uPoints[16];   // xy=world pos（末尾一个是 centroid）
    vec4 uDroplets[8];  // juice 额外 metaball 点：xy=world pos, z=falloff(ndc-y), w=intensity
} U;

// gel 材质常量 —— 迁移自 main.cpp Tier1 SlimeParams（准确值）。
const vec3  kDeepColor  = vec3(0.02, 0.16, 0.05);  // 厚处深绿（核心 / SSS 暗端）
const vec3  kBodyColor  = vec3(0.11, 0.62, 0.20);  // 主体绿（薄处 / 边亮端）
const vec3  kRimColor   = vec3(0.55, 1.00, 0.55);  // Fresnel 亮边（黄绿）
const vec3  kEyeColor   = vec3(1.00, 0.92, 0.45);  // 发光眼（暖黄）
const vec3  kSpeckColor = vec3(0.85, 1.00, 0.42);  // 体内光斑
const vec3  kGlowColor  = vec3(0.18, 0.85, 0.28);  // 底部接触辉光
const vec3  kLightDir   = vec3(-0.35, 0.85, 0.42); // 主光方向（左上偏前，y-up）
const float kRimPower   = 3.0;
const float kSpecPower  = 32.0;

// 体内光斑局部布点（y-up，偏下半区 = 负 y）——抄 Tier1 kSpeckLocal 14 点。
const vec2 kSpeck[14] = vec2[14](
    vec2(-0.35, -0.30), vec2(0.20, -0.45), vec2(-0.10, -0.55), vec2(0.42, -0.20),
    vec2(-0.50, -0.05), vec2(0.05, -0.28), vec2(0.30, -0.60), vec2(-0.25, -0.62),
    vec2(0.52, -0.36), vec2(-0.55, -0.40), vec2(0.15, -0.72), vec2(-0.05, -0.44),
    vec2(0.36, -0.50), vec2(-0.40, -0.56));

vec2 ProjectNdc(vec2 world)
{
    vec4 clip = U.uViewProj * vec4(world, 0.0, 1.0);
    return clip.xy / clip.w;
}

// 软椭圆遮罩：q = (uv - center)/halfAxes；内部 ~1，边缘平滑到 0。
float SoftEllipse(vec2 uv, vec2 center, vec2 halfAxes, float softness)
{
    float e = length((uv - center) / max(halfAxes, vec2(1e-4)));
    return 1.0 - smoothstep(1.0 - softness, 1.0, e);
}

void main()
{
    vec2  pix      = vUv * 2.0 - 1.0; // 像素 NDC（y-down）
    int   count    = int(U.uParams0.x);
    float iso      = U.uParams0.y;
    float aspect   = U.uParams0.z;
    float falloff  = max(U.uParams0.w, 1e-4);
    float blobR    = max(U.uParams1.x, 1e-4);
    float domeR    = max(U.uParams1.y * blobR, 1e-4); // 半球半径 = domeScale × blob 投影半径
    float rimGain  = U.uParams1.z;
    float specGain = U.uParams1.w;
    float ambient  = U.uParams2.x;
    float time     = U.uParams2.y;
    float eyeGain  = U.uParams2.z;
    float speckGain = U.uParams2.w;
    float glowGain = U.uParams3.x;

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

    // --- juice droplet 通道（落地溅射 / 分裂 / 合并）：额外 metaball 点累加进同一 field，
    //     与主体 metaball 天然 smin 融合 / 断开 → 剪影自动连 / 断。各自半径（世界半径 CPU 已转 ndc-y）。
    int dropCount = int(U.uParams3.y);
    for (int i = 0; i < dropCount; ++i)
    {
        vec4  dp  = U.uDroplets[i];
        float rad = max(dp.z, 1e-4);
        vec2  d   = pix - ProjectNdc(dp.xy);
        d.x *= aspect;
        float r = length(d) / rad;
        if (r < 1.0)
        {
            float a = 1.0 - r;
            field += dp.w * a * a; // w = intensity
        }
    }

    float mask = smoothstep(iso - 0.05, iso, field);
    if (mask <= 0.001)
    {
        discard;
    }

    // --- 假法线：以 centroid（末位质点）为心的显式半球（M2）---
    vec2  centroidNdc = ProjectNdc(U.uPoints[count - 1].xy);
    vec2  dc          = pix - centroidNdc;
    dc.x *= aspect;                               // screen-isotropic metric（y-down）
    float dcLen   = length(dc);
    float rr      = clamp(dcLen / domeR, 0.0, 1.0); // 径向分数：0 圆心 → 1 边缘
    vec2  outward = (dcLen > 1e-6) ? normalize(vec2(dc.x, -dc.y)) : vec2(0.0); // 转 y-up
    vec3  N       = vec3(rr * outward, sqrt(max(0.0, 1.0 - rr * rr)));         // dome 法线

    // blob 局部帧（y-up，单位 = blobNdcRadius）供眼/光斑/辉光摆位。
    vec2 luv = vec2(dc.x, -dc.y) / blobR;

    // --- gel 着色（Tier1 ShadeGel 逐像素；半 Lambert + rr² SSS 由 M2 定稿）---
    vec3  L    = normalize(kLightDir);
    vec3  V    = vec3(0.0, 0.0, 1.0);
    vec3  H    = normalize(L + V);
    float diff = dot(N, L) * 0.5 + 0.5; // 半 Lambert：SSS 通透感，暗部不死黑
    float spec = pow(clamp(dot(N, H), 0.0, 1.0), kSpecPower);
    float fres = pow(1.0 - clamp(N.z, 0.0, 1.0), kRimPower);

    // 厚度 SSS：中心厚=深绿、边薄=亮体色；rr² 让深绿只集中在核心、大片保持通透亮绿。
    vec3 col = mix(kDeepColor, kBodyColor, rr * rr);
    col *= (ambient + (1.0 - ambient) * diff); // dome 漫反（半 Lambert）
    col += kRimColor * (fres * rimGain);        // Fresnel 亮边（M3 调强，喂 bloom）
    col += vec3(1.0) * (spec * specGain);       // 湿润高光

    // --- M3 角色元素（emissive 累加，随 mask 收边）---
    vec3 emissive = vec3(0.0);

    // 接触辉光：blob 底部（centroid 下方约一个半径）压扁绿椭圆，additive。
    {
        float g = SoftEllipse(luv, vec2(0.0, -0.95), vec2(1.15, 0.22), 0.9);
        emissive += kGlowColor * (g * glowGain);
    }

    // 体内光斑：局部布点、明灭（sin(time)）——偏下半区密集。
    for (int k = 0; k < 14; ++k)
    {
        float ph = float(k) * 1.7;
        vec2  sp = kSpeck[k] * 0.9 + vec2(0.0, sin(time * 0.6 + ph) * 0.03);
        float tw = 0.6 + 0.4 * sin(time * 1.3 + ph); // 明灭
        float s  = SoftEllipse(luv, sp, vec2(0.045, 0.06), 0.6);
        emissive += kSpeckColor * (s * tw * speckGain);
    }

    // 两只发光眼：centroid 上方偏移、左右各一竖椭圆 + 更亮高光芯，轻微呼吸。
    {
        float pulse   = 1.0 + 0.05 * sin(time * 2.5);
        vec2  eyeHalf = vec2(0.21 * 0.45, 0.21 * pulse); // (rx,ry) 竖椭圆
        vec2  up      = vec2(0.0, 0.30);
        for (int e = 0; e < 2; ++e)
        {
            vec2  ec   = up + vec2((e == 0 ? -0.26 : 0.26), 0.0);
            float body = SoftEllipse(luv, ec, eyeHalf, 0.5);
            float core = SoftEllipse(luv, ec, eyeHalf * 0.5, 0.4); // 眼内高光芯
            emissive += kEyeColor * (body * eyeGain) + vec3(1.0) * (core * 0.6);
        }
    }

    col += emissive * mask;    // 随剪影收边，避免边界硬 emissive
    col = max(col, vec3(0.0)); // linear HDR，不上钳（喂 bloom）

    outColor = vec4(col, mask); // 核心实(mask≈1)、边缘透；alpha blend 已配好
}
