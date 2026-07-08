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
    vec4 uParams3;      // x=glowGain, y=dropletCount, z=opacity, w=coreGain
    vec4 uBodyColor;    // rgb（颜色组顺序与 C++ SlimeUbo.uColors 对齐：body/deep/rim/eye/speck/glow）
    vec4 uDeepColor;
    vec4 uRimColor;
    vec4 uEyeColor;
    vec4 uSpeckColor;
    vec4 uGlowColor;
    vec4 uPoints[16];   // xy=world pos（末尾一个是 centroid）
    vec4 uDroplets[8];  // juice 额外 metaball 点：xy=world pos, z=falloff(ndc-y), w=intensity
} U;

// gel 颜色经 UBO 传入（SlimeTuningComponent 可调，默认 = 参考图校准值）；
// 宏别名保持下方使用点原样。
#define kBodyColor  (U.uBodyColor.rgb)
#define kDeepColor  (U.uDeepColor.rgb)
#define kRimColor   (U.uRimColor.rgb)
#define kEyeColor   (U.uEyeColor.rgb)
#define kSpeckColor (U.uSpeckColor.rgb)
#define kGlowColor  (U.uGlowColor.rgb)
const vec3  kLightDir   = vec3(-0.35, 0.85, 0.42); // 主光方向（左上偏前，y-up）
const float kRimPower   = 6.0;   // 高 power = 细玻璃亮边；3.0 会糊成肥晕带
const float kSpecPower  = 110.0; // 高 power = 锐小湿润高光；32 糊成一团

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

float Hash(vec2 p)
{
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

// 2D 值噪声（体内密度斑驳用；缓慢漂移让果冻"活"）。quintic 平滑；值噪声低频下
// 有轴对齐格子感（读成马赛克），使用处须配合八度间旋转（kOctRot）打破。
float Noise(vec2 p)
{
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    return mix(mix(Hash(i), Hash(i + vec2(1.0, 0.0)), u.x),
               mix(Hash(i + vec2(0.0, 1.0)), Hash(i + vec2(1.0, 1.0)), u.x), u.y);
}

const mat2 kOctRot = mat2(0.80, -0.60, 0.60, 0.80); // 八度间旋转 ~37°，破轴对齐格子

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
    float opacity  = clamp(U.uParams3.z, 0.0, 1.0);
    float coreGain = U.uParams3.w;

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

    // --- gel 着色（透光果冻模型：亮度由厚度决定 + 真透明，对齐参考图）---
    vec3  L    = normalize(kLightDir);
    vec3  V    = vec3(0.0, 0.0, 1.0);
    vec3  H    = normalize(L + V);
    float diff = dot(N, L) * 0.5 + 0.5; // 半 Lambert：SSS 通透感，暗部不死黑
    float spec = pow(clamp(dot(N, H), 0.0, 1.0), kSpecPower);

    // 场共形边缘度：0=剪影线，1=内部深处。边缘效果（边线/吸收带）必须用真实
    // metaball 场算——解析 dome 与形变剪影不重合时，超出 dome 的剪影区 rr 会被
    // clamp 在 1、Fresnel 整片全亮（侧面淡绿月牙的根因）。dome 只留给法线/高光。
    float edgeT = clamp((field - iso) / (0.9 * iso), 0.0, 1.0);
    float fres  = pow(1.0 - edgeT, kRimPower * 0.5); // 场梯度比 N.z 缓，power 减半保线宽

    // 透光梯度模型：参考图的光源在体内底部——亮度垂直梯度主导（底亮顶深），
    // 厚度只做次要调制。方向光只留微量残余：表面明暗一强就读成实心球。
    float thin       = smoothstep(0.10, 0.95, rr);   // 0 心（厚）→ 1 边（薄）
    // 深色窗口只开在顶部边缘附近：中上部的"暗"交给透明度（乘性深绿混浅背景会出浊带）。
    float topness    = smoothstep(0.35, 1.0, luv.y);
    float bottomness = smoothstep(0.2, -0.8, luv.y);
    float wDeep      = clamp(0.30 * (1.0 - thin) + 0.55 * topness, 0.0, 1.0);

    // 三段透射色相梯度（顶深绿→中鲜绿→底暖黄绿）：Beer-Lambert 的"穿透越浅越偏暖"。
    // 暖端写进乘性 base 保持饱和——靠 additive 提暖会被 tonemap 脱饱和成白（白洗根因）。
    vec3  warmCol = mix(kBodyColor, kSpeckColor, 0.55);
    float warmW   = bottomness * (0.35 + 0.65 * (1.0 - thin));
    vec3  base    = mix(mix(kBodyColor, kDeepColor, wDeep), warmCol, warmW);
    float shade   = mix(1.0, diff, N.z * 0.12); // 透明体几乎无表面明暗，只留一丝体积暗示
    vec3  body    = base * (ambient + (1.0 - ambient) * shade);

    // 体内密度斑驳：双频值噪声缓慢漂移（八度间旋转）；须极弱——过强读成表面纹理=变硬。
    float mottle = Noise(luv * 3.5 + vec2(0.0, time * 0.05)) * 0.6
                 + Noise(kOctRot * luv * 7.0) * 0.4;
    body *= 0.94 + 0.12 * mottle;

    // 边缘吸收只留极弱色相残留：体是全透的（用户拍板"整体透明状"），
    // 轮廓交给 Fresnel 亮线 + bloom 表达，不再用不透明深绿边带画形。场共形贴剪影。
    float absorb = 1.0 - smoothstep(0.05, 0.45, edgeT);
    body = mix(body, kDeepColor, absorb * 0.35);

    // --- 加性光（透射/反射光，不随体透明度衰减）---
    vec3 shine = vec3(0.0);

    // 介质自散射底色：透明内部再暗也带一点绿味（参考图暗部不是纯黑）。
    shine += kDeepColor * 0.12;

    // 内芯发光核：底部中心暖亮核向上/向边衰减——内部亮过外壳，"透亮"的主体。
    float core = (1.0 - thin) * pow(clamp(0.35 - luv.y * 0.9, 0.0, 1.0), 1.6);
    shine += mix(kGlowColor, kSpeckColor, 0.5) * (core * coreGain);

    // 底部光池：0 顶 → 1 底缘；调暖（掺金斑色）——参考图光池偏暖黄绿、横贯整个裙边。
    float pool = pow(clamp(-luv.y * 0.8 + 0.25, 0.0, 1.0), 2.2);
    shine += mix(kGlowColor, kSpeckColor, 0.60) * (pool * glowGain * 0.5);

    // （原"宽内透光带"已删：全透体方向下宽带加性光在浅背景上读成悬浮淡绿月牙。）

    // Fresnel 锐玻璃边线：全周可见，底部最亮（近内光源）。
    shine += kRimColor * (fres * rimGain * (0.75 + 0.25 * bottomness));
    shine += vec3(1.0) * (spec * specGain); // 湿润锐高光

    // --- M3 角色元素（emissive 累加，随 mask 收边）---
    vec3 emissive = vec3(0.0);

    // 接触辉光：blob 底部（centroid 下方约一个半径）压扁绿椭圆，additive。
    {
        float g = SoftEllipse(luv, vec2(0.0, -0.95), vec2(1.15, 0.22), 0.9);
        emissive += kGlowColor * (g * glowGain);
    }

    // 湿玻璃反射条：顶部左上两道细白条（参考图窗式反光；随 specGain 缩放）+
    // 内壁二次反射（主条下方错位一道弱条，玻璃"双层壁"读感 cue）。
    {
        float streak = SoftEllipse(luv, vec2(-0.30, 0.68), vec2(0.19, 0.05), 0.55)
                     + 0.6 * SoftEllipse(luv, vec2(0.10, 0.80), vec2(0.09, 0.035), 0.55);
        float inner  = SoftEllipse(luv, vec2(-0.24, 0.52), vec2(0.14, 0.04), 0.7);
        // （原大面积 sheen 已删：全透体上宽面加性光在浅背景读成顶部淡绿月牙。）
        emissive += vec3(1.0) * ((streak + inner * 0.25) * specGain * 0.5);
    }

    // 体内金斑：主层大小错落 + 明灭 + 每斑旁伴一颗错位子斑（成簇的碎金箔感，
    // 参考图不是均匀圆点）；次层同表变换再采一轮出微尘。散布放宽到两侧。
    for (int k = 0; k < 14; ++k)
    {
        float ph  = float(k) * 1.7;
        float rnd = fract(sin(float(k) * 12.9898) * 43758.5453); // 每斑固定伪随机
        float tw  = 0.65 + 0.35 * sin(time * 1.3 + ph);
        vec2  sp  = kSpeck[k] * 1.12 + vec2(0.0, sin(time * 0.6 + ph) * 0.03);
        float s   = SoftEllipse(luv, sp, vec2(0.055, 0.07) * mix(0.5, 2.0, rnd), 0.55);
        emissive += kSpeckColor * (s * tw * speckGain);

        vec2  off = (vec2(Hash(vec2(ph, 1.3)), Hash(vec2(1.7, ph))) - 0.5) * 0.17;
        float s3  = SoftEllipse(luv, sp + off, vec2(0.036, 0.046) * (0.5 + rnd), 0.6);
        emissive += kSpeckColor * (s3 * tw * speckGain * 0.7);

        vec2  sp2 = -kSpeck[k] * vec2(0.9, 0.5) + vec2(0.0, -0.38); // 中带微尘
        float s2  = SoftEllipse(luv, sp2, vec2(0.028, 0.034) * (0.7 + rnd), 0.6);
        emissive += kSpeckColor * (s2 * (0.5 + 0.5 * tw) * speckGain * 0.8);
    }

    // 两只发光眼：竖椭圆 + 白热芯 + 眼窝微暗一圈（参考图眼周深色让眼跳出来），轻微呼吸。
    {
        float pulse   = 1.0 + 0.05 * sin(time * 2.5);
        vec2  eyeHalf = vec2(0.132, 0.28 * pulse); // (rx,ry) 竖椭圆（对齐参考图大眼）
        vec2  up      = vec2(0.0, 0.32);
        for (int e = 0; e < 2; ++e)
        {
            vec2  ec     = up + vec2((e == 0 ? -0.28 : 0.28), 0.0);
            float socket = SoftEllipse(luv, ec, eyeHalf * 1.7, 0.7);
            body *= 1.0 - 0.24 * socket; // 眼窝微暗；重了像画上去的实心球贴纸
            float eyeBody = SoftEllipse(luv, ec, eyeHalf, 0.45);
            float core    = SoftEllipse(luv, ec, eyeHalf * vec2(0.38, 0.50), 0.5);
            emissive += kEyeColor * (eyeBody * eyeGain) + vec3(1.25, 1.12, 0.85) * (core * 0.85);
        }
    }

    // --- 预乘 alpha 合成（blend = One / OneMinusSrcAlpha）---
    // 体色乘 alpha：内部按 opacity 透背景、吸收带/Fresnel 边回实（掠角长光程更实）；
    // 低频噪声调制内部密度（稠处略实/稀处略透 = 参考图体内的"稠密果冻团"）；
    // 加性光 + emissive 不乘：眼/金斑/辉光在半透体上保持实亮。两项都随 mask 收边。
    // 整体统一低 alpha（全透水泡）：边缘不收实，只有密度团块微调制；
    // 轮廓读感 = Fresnel 亮线（additive，过 bloom 阈值自然出边缘光晕）。
    float density = Noise(luv * 2.1 + vec2(time * 0.03, 0.0)) * 0.65
                  + Noise(kOctRot * luv * 4.6 + vec2(0.0, time * 0.02)) * 0.35;
    float alpha   = mask * clamp(opacity * (0.74 + 0.52 * density), 0.0, 1.0);
    // 加性光乘 mask²：剪影渐隐带里把光压干净，避免浅背景下的边缘绿雾。
    vec3 rgb = body * alpha + (shine + emissive) * (mask * mask);
    rgb = max(rgb, vec3(0.0)); // linear HDR，不上钳（喂 bloom）

    outColor = vec4(rgb, alpha);
}
