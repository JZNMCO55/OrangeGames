// spike-01-blob —— 流体史莱姆 context-sensitive 手感 Spike 1 · Loop A
//
// 设计规格：../spike-01-blob-feel.md（执行规格）+ ../phase0-pillars.md（宪法）。
//
// 本文件实现 spike 的 **Loop A + Loop B**：
//   * Loop A —— gameplay 真相层：隐藏 control point（Box2D dynamic 刚体）承载
//     落点 / 碰撞 / 输入响应，调成标准平台跳跃手感（已落地，本次不动其手感逻辑）。
//   * Loop B —— 视觉表现层：可见软体 blob（自写 Verlet/PBD，见 Blob.h）用弹簧
//     软软追 control point，是主可见物。仍只用 debug 线框 + 纯色，不碰 metaball /
//     折射 / 任何好看渲染（spec 硬纪律：feel 在前视觉在后）。
//   * blob 接在 FixedStep 同一固定子步内推进；apex 分叉（视觉质心 ↔ control point
//     距离）实时画在屏幕 + ImGui 读数，是本 spike 的核心待测产出（"果冻预算"）。
//
// 核心架构（spec "果冻感和精确落点不在同一层"）：
//   * gameplay 真相 = control point（这里实现）：天生精确、可预测；
//   * 视觉表现 = 软体 blob（Loop B）：弹簧软软追 control point，随便晃。
// 二者解耦，所以本文件只管把刚体手感调跟手。
//
// 已落地：
//   1. Box2D 静态世界（平地 + 左/右平台 + 右边沿 ledge + 中间窄缝两壁）；
//   2. 固定步长物理（60Hz accumulator，与渲染帧率解耦）；
//   3. 引擎 Input（Action/ActionMap/InputContext）绑左右移动 + 跳跃；
//   4. control point 平台跳跃手感（加/减速 + 空中控制 + 可变跳跃高度 +
//      apex 重力衰减 + coyote time + jump buffer + corner correction）；
//   5. ImGui 调参面板（全部旋钮 slider，边玩边拧不重编）；
//   6. 判据 debug overlay（输入瞬间标记 + control point 速度 + 10 次落点散布）。
//
// 留给 Loop B：control point 的位置 / 速度经 SpikeLayer::GetControlPoint() 暴露，
// 将来的软体 blob 用弹簧吸附它（见文件末尾 ControlPointState 注释）。
//
// 注意：运行需要引擎 builtin shader 在 .exe 旁（shaders/orange_engine/*.spv），
// 由 CMakeLists.txt 的 ORANGE_ENGINE_SHADER_DIR workaround 在 build 时 copy。

// 引擎公共 API 经便利头收敛引入（prelude 精选核心 + 各子系统聚合头），
// 替代逐个 <orange/engine/<mod>/*> 罗列。prelude 已含 World/Entity/Transform/
// Name + FrameContext/Layer + Log/Time + glm 核心；其余按用到的子系统取。
#include <orange/engine/prelude.h>
#include <orange/engine/app.h>
#include <orange/engine/asset.h>
#include <orange/engine/input.h>
#include <orange/engine/physics.h>
#include <orange/engine/platform.h>
#include <orange/engine/render.h>

// 消费者自行 #include <imgui.h>（引擎把 imgui include 目录 PUBLIC 暴露，
// 公共头本身零 imgui 类型；GAP-2026-05-27-consumer-imgui-tuning-hook 已补完）。
#include <imgui.h>

// Loop B —— 软体 blob 仿真（自写 Verlet/PBD，纯游戏代码；见 Blob.h 头注释）。
#include "Blob.h"

// Tier 2 M0 —— 自定义 fullscreen SDF metaball pass（下沉到 RHI，证明管线打通）。
#include "SlimeMetaballPass.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <utility>
#include <vector>

using namespace Orange::Engine;
using Orange::Engine::Asset::AssetRegistry;
using Orange::Engine::Asset::ShaderAsset;
using Orange::Engine::Asset::ShaderLoader;
using Orange::Engine::Render::Camera;
using Orange::Engine::Render::DebugDrawScene;
using Orange::Engine::Render::MaterialSystem;
using Orange::Engine::Render::Pipeline;
using Orange::Engine::Render::BloomPass;
using Orange::Engine::Render::PostProcessChain;
namespace In   = Orange::Engine::Input;
namespace Phys = Orange::Engine::Physics;

namespace
{

    // debug draw 颜色：ABGR packed（低 8 位 R，高 8 位 A，0xAA_BB_GG_RR）。
    constexpr std::uint32_t kGreen   = 0xFF00FF00u; // 平地
    constexpr std::uint32_t kCyan    = 0xFFFFFF00u; // 平台
    constexpr std::uint32_t kRed     = 0xFF0000FFu; // 窄缝两壁
    constexpr std::uint32_t kAmber   = 0xFF1A78FFu; // 落点打点
    constexpr std::uint32_t kYellow  = 0xFF00FFFFu; // control point 标记
    constexpr std::uint32_t kMagenta = 0xFFFF00FFu; // 速度向量
    constexpr std::uint32_t kWhite   = 0xFFFFFFFFu; // 输入瞬间标记
    constexpr std::uint32_t kGrey    = 0xFF808080u; // 接地指示
    // Loop B 配色（ABGR：低 8 位 R）。
    constexpr std::uint32_t kBlob     = 0xFF66FF66u; // 软体 blob perimeter loop（主可见物）
    constexpr std::uint32_t kSpoke    = 0xFF338833u; // blob center 辐条（暗绿）
    constexpr std::uint32_t kDiverge  = 0xFF00A5FFu; // apex 分叉连线（橙：cp ↔ 视觉质心）
    constexpr std::uint32_t kCentroid = 0xFF00D7FFu; // blob 视觉质心十字标记（金）

    // 固定物理步长：60Hz（spec 地基 #2，与渲染帧率解耦）。
    constexpr float kFixedDt = 1.0f / 60.0f;
    // 单帧累加器上限：避免窗口切回前台 / 断点后的"长帧"一次推进过多子步。
    constexpr float kMaxFrameDt = 0.10f;

    // control point 出生点（左侧地面上方一点，落下后稳定在平地上）。
    constexpr glm::vec2 kSpawn{-5.0f, -2.5f};

    // ---------------------------------------------------------------------------
    // 碰撞过滤位（category/mask）——引擎新落地的 QueryFilter 用它做“只命中关卡几何、
    // 自动排除 control point 自身”的干净分类（见 ProbeGrounded / HeadClearAt）。
    //   * kCatPlayer = control point（动态刚体）；
    //   * kCatGround = isGround 的平地；
    //   * kCatWall   = 其余静态盒（左右平台 + 窄缝两壁）。
    // 所有 body 的 maskBits 仍保持 0xFFFFFFFF（与一切碰撞）→ 真实 Box2D 碰撞行为
    // 与迁移前逐字不变；category 位仅供空间查询按类过滤。
    // 查询命中规则（对齐 Box2D）：
    //   (filter.maskBits & collider.categoryBits) && (collider.maskBits & filter.categoryBits)
    // 故查询用 QueryFilter{categoryBits=all, maskBits=kCatGround|kCatWall} 即可只打
    // 关卡几何、天然排除 kCatPlayer（player 不在 mask 里）——无需手动剔除 self。
    constexpr std::uint32_t kCatPlayer = 0x1u; // = ColliderComponent 默认 categoryBits
    constexpr std::uint32_t kCatGround = 0x2u;
    constexpr std::uint32_t kCatWall   = 0x4u;
    // 关卡实心几何的查询 mask（平地 + 平台 + 墙，即原 analytic 探针遍历的全部盒）。
    constexpr std::uint32_t kSolidMask = kCatGround | kCatWall;

    // ---------------------------------------------------------------------------
    // 手感旋钮（全部走 ImGui slider，不 hardcode 数值）。
    // baseline 抄 Celeste 经典起点（coyote ~5 帧 / buffer ~6 帧 / 跑速 ~9 u/s），
    // 供用户第二天真机微调；详见 spec "循环 A 调参顺序" + "别从零发明手感"。
    // ---------------------------------------------------------------------------
    struct FeelParams
    {
        // —— 地面移动（先让走/跑跟手，判据 #1 延迟在这关）——
        float maxRunSpeed = 9.0f;   // 最大跑速 u/s（Celeste ~9 量级）
        float groundAccel = 90.0f;  // 地面加速度 u/s^2（~0.1s 到顶速）
        float groundDecel = 120.0f; // 地面减速度 u/s^2（停得比加速快 → 更跟手）

        // —— 空中控制（单独减弱，空中没地面那么灵）——
        float airAccel = 60.0f; // 空中加速度 u/s^2
        float airDecel = 60.0f; // 空中减速度 u/s^2

        // —— 跳跃（依赖链：先调这组再调 juice）——
        float jumpSpeed       = 16.0f; // 起跳初速度 u/s
        float riseGravity     = 45.0f; // 上升段重力 u/s^2
        float fallGravity     = 70.0f; // 下落段重力（比上升大 → 下落更跟手）
        float apexThreshold   = 3.0f;  // |vy| 低于此值视为 apex（顶点）
        float apexGravityMult = 0.55f; // apex 段重力衰减系数（顶点悬停手感）
        float lowJumpMult     = 2.5f;  // 上升中松手 → 重力放大倍率（可变跳跃高度）
        float maxFallSpeed    = 25.0f; // 终端下落速度上限 u/s

        // —— juice 辅助（建立在跳跃调好之上）——
        float coyoteTime           = 0.08f; // 离开平台后仍可起跳的宽限 s（~5 帧）
        float jumpBufferTime       = 0.10f; // 落地前预输入跳跃的缓冲 s（~6 帧）
        float cornerCorrectionDist = 0.25f; // 顶到角落时的最大水平推移 u（0 = 关）

        // —— 爬墙 wall-climb（本轮新增；dogfood-pending：手感未真机验证，靠 slider 现场调）——
        // 分层同 Loop A：爬墙只作用在 control point（gameplay 真相），blob 照旧软软追随。
        bool  wallEnabled      = true;  // 总开关（关掉退回纯 Loop A 手感）
        float wallSlideSpeed   = 3.5f;  // 按住朝墙 + 空中下落时的下滑限速 u/s（须 < maxFallSpeed 才有"抓住"感）
        float wallJumpSpeedX   = 11.0f; // 墙跳离墙水平初速 u/s
        float wallJumpSpeedY   = 15.0f; // 墙跳竖直初速 u/s
        float wallJumpLockTime = 0.12f; // 墙跳后抑制"回墙"输入的时长 s（防立刻粘回刚跳离的墙）
        float wallCoyoteTime   = 0.08f; // 离墙后仍可墙跳的宽限 s
        float wallClimbSpeed   = 4.0f;  // 抓墙（LeftShift）时 ↑/↓ 攀爬速度 u/s
        float wallStamina      = 0.0f;  // 抓墙耐力上限 s（0 = 无限，先不加压力，dogfood 再定）
        float wallDetectDist   = 0.06f; // 侧向探墙距离 u（cpHalfWidth 之外多远算贴墙）

        // —— control point 尺寸（小框 / 小胶囊代理）——
        float cpHalfWidth  = 0.18f; // 半宽（须 < 窄缝半宽 0.4 才钻得过）
        float cpHalfHeight = 0.28f; // 半高
    };

    // 一块静态碰撞盒。单一几何真相：既喂 Box2D 静态 body（碰撞解算），又供 debug
    // 线框可视，还作为 Blob 软体碰撞的 AABB 源（见 Blob::Step）。接地 / 角落 / 天花板
    // 探针已迁移到引擎空间查询（不再读本结构），但 isGround 仍用于 body 建 collider 时
    // 分类 kCatGround / kCatWall（供查询按类过滤），color 供 debug 上色。
    struct LevelBox
    {
        glm::vec2     min;
        glm::vec2     max;
        std::uint32_t color;
        bool          isGround; // true=平地(kCatGround)；false=平台/墙(kCatWall)
    };

    // Spike 1 规格那一屏：平地 + 左/右平台 + 右边沿 ledge + 中间窄缝两壁。
    // 单一数据源：同一份 box 列表既建 Box2D 静态体，又喂 debug 线框，又作 Blob 碰撞 AABB。
    std::vector<LevelBox> MakeLevel()
    {
        return {
            // 平地：top y=-3，横跨视野（厚 0.5）。
            {{-9.0f, -3.5f}, {9.0f, -3.0f}, kGreen, true},
            // 左平台：base 平台跳跃 / 落点 / coyote 测试（右沿 x=-2.5 可蹬空起跳）。
            {{-6.0f, -0.7f}, {-2.5f, -0.5f}, kCyan, false},
            // 右平台：右端 x=6 竖面 = 单壁挂墙 / 抓墙测试面。
            {{2.5f, 0.3f}, {6.0f, 0.5f}, kCyan, false},

            // —— 墙跳 chimney（专测两壁交替墙跳）——
            // 缝宽 1.4 ≈ wallJumpX(11)×lockTime(0.12)=1.32：起跳后恰在锁失效时够到对面壁。
            // 墙高 y=-3→4（7u）给真实攀爬。左壁底浮到 y=-1.3 留地面入口（从左侧走进缝底再往上跳）。
            {{-0.85f, -1.3f}, {-0.7f, 4.0f}, kRed, false}, // 左壁（底部浮起留入口）
            {{0.7f, -3.0f}, {0.85f, 4.0f}, kRed, false},   // 右壁（落到地面）
            {{0.85f, 3.8f}, {3.0f, 4.0f}, kCyan, false},   // 顶部 mantle 平台（爬到顶翻上去）

            // —— squeeze 隧道（专测穿墙隙）——
            // 缝宽 0.7 > control point 宽 0.36（现在能钻过）、< blob 直径 1.0（须形变）。
            // blob ooze-through 视觉判据待 stranding 修复（见 NOTES-dogfood-2026-07-05.md）；
            // 此刻只 control point 可过。顶盖封成"孔"，才像真 squeeze 而非开口槽。
            {{6.5f, -3.0f}, {6.6f, -1.0f}, kRed, false},   // 隧道左壁
            {{7.3f, -3.0f}, {7.4f, -1.0f}, kRed, false},   // 隧道右壁
            {{6.5f, -1.0f}, {7.4f, -0.85f}, kCyan, false}, // 隧道顶盖
        };
    }

    // control point 对外快照（留给 Loop B 的软体 blob 弹簧吸附用）。
    struct ControlPointState
    {
        glm::vec2 position{0.0f, 0.0f};
        glm::vec2 velocity{0.0f, 0.0f};
        bool      grounded{false};
    };

    // cur 朝 tgt 逼近：加速（朝同向提速）用 accel，减速 / 反向用 decel。
    float Approach(float cur, float tgt, float accel, float decel, float dt)
    {
        const bool speedingUp = (tgt != 0.0f) &&
                                (std::abs(tgt) >= std::abs(cur)) &&
                                (cur == 0.0f || ((cur > 0.0f) == (tgt > 0.0f)));
        const float maxDelta = (speedingUp ? accel : decel) * dt;
        if (cur < tgt)
        {
            return std::min(cur + maxDelta, tgt);
        }
        return std::max(cur - maxDelta, tgt);
    }

    // glm 颜色 (0..1 RGB) → DebugDrawScene 的 packed ABGR uint32（低 8 位 = R）。
    inline std::uint32_t PackColorABGR(const glm::vec3& c, float a = 1.0f)
    {
        auto q = [](float v)
        { return static_cast<std::uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
        return (q(a) << 24) | (q(c.z) << 16) | (q(c.y) << 8) | q(c.x);
    }

    // 闭合 Catmull-Rom 平滑：把 N 点环每段插成 sub 段，让 blob 轮廓不那么 faceted。
    // sub<=1 原样返回。
    inline std::vector<glm::vec2> SmoothClosedCatmullRom(const std::vector<glm::vec2>& pts, int sub)
    {
        const int n = static_cast<int>(pts.size());
        if (n < 3 || sub <= 1)
        {
            return pts;
        }
        std::vector<glm::vec2> out;
        out.reserve(pts.size() * static_cast<std::size_t>(sub));
        for (int i = 0; i < n; ++i)
        {
            const glm::vec2& p0 = pts[(i - 1 + n) % n];
            const glm::vec2& p1 = pts[i];
            const glm::vec2& p2 = pts[(i + 1) % n];
            const glm::vec2& p3 = pts[(i + 2) % n];
            for (int s = 0; s < sub; ++s)
            {
                const float t  = static_cast<float>(s) / static_cast<float>(sub);
                const float t2 = t * t;
                const float t3 = t2 * t;
                out.push_back(0.5f * ((2.0f * p1) + (-p0 + p2) * t + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2 + (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3));
            }
        }
        return out;
    }

    // 实心史莱姆外观（全 live slider；debug 三角填充 + bloom 发光）。
    //
    // Tier 1 —— CPU 逐三角伪着色 gel：把 blob 内部细分成 rings×segments 个小四边形，
    // 每个小面在 CPU 上算一遍"半球假法线 + dome 漫反 + Fresnel 亮边 + Blinn 高光 +
    // 厚度驱动 SSS"，再平涂进 debug 三角（每三角单色，靠密度 + bloom 糊成渐变）。
    // 逐像素平滑留给 Tier 2 自定义 fragment shader。参考图配色：深绿厚体 + 黄绿亮边 +
    // 暖黄发光眼 + 体内黄绿光斑 + 落地绿辉光。
    struct SlimeParams
    {
        bool fill      = true;
        bool wireframe = false; // 保留 debug 线框作对照
        bool beauty    = true;  // 展示模式：只画史莱姆(黑底)，隐藏关卡线框 + gameplay overlay

        // —— 胶体分层色（厚→薄）——
        glm::vec3 deepColor = {0.02f, 0.16f, 0.05f}; // 厚处深绿（核心底色，SSS 暗端 → 通透深度）
        glm::vec3 bodyColor = {0.11f, 0.62f, 0.20f}; // 主体绿（略压亮度，让眼/边对比更强）
        glm::vec3 rimColor  = {0.55f, 1.00f, 0.55f}; // Fresnel 亮边（黄绿）
        glm::vec3 eyeColor  = {1.00f, 0.92f, 0.45f}; // 发光眼（暖黄，超 bloom 阈值强发光）
        glm::vec3 speckColor = {0.85f, 1.00f, 0.42f}; // 体内漂浮光斑
        glm::vec3 glowColor  = {0.18f, 0.85f, 0.28f}; // 落地接触辉光

        // —— 伪光照 ——
        glm::vec3 lightDir  = {-0.35f, 0.85f, 0.42f}; // 主光方向（左上偏前，对齐参考图高光）
        float     specPower = 32.0f;                  // Blinn 高光锐度
        float     specGain  = 1.15f;                  // 高光强度（脆亮湿润感）
        float     rimPower  = 3.0f;                   // Fresnel 指数（越大边越窄）
        float     rimGain   = 0.85f;                  // Fresnel 亮边强度
        float     ambient   = 0.30f;                  // 环境光基线（暗部不全黑）

        // —— 网格密度 / 细节 ——
        int   rings         = 12; // 径向环数（越多 banding 越少）
        int   silhouetteSub = 3;  // 轮廓 Catmull-Rom 段数（受 DebugDraw 4096 顶点上限约束）
        bool  drawEyes      = true;
        bool  drawSpecks    = true;
        bool  drawContactGlow = true;
        float eyeSize       = 0.21f; // 眼高（相对 blob 半径；参考图眼是竖高亮条）
    };

    // 固定体内光斑局部偏移（单位圆内、偏下半区）——确定性布点，无运行时随机。
    constexpr int kSpeckCount = 14;
    constexpr glm::vec2 kSpeckLocal[kSpeckCount] = {
        {-0.35f, -0.30f}, {0.20f, -0.45f}, {-0.10f, -0.55f}, {0.42f, -0.20f},
        {-0.50f, -0.05f}, {0.05f, -0.28f}, {0.30f, -0.60f}, {-0.25f, -0.62f},
        {0.52f, -0.36f}, {-0.55f, -0.40f}, {0.15f, -0.72f}, {-0.05f, -0.44f},
        {0.36f, -0.50f}, {-0.40f, -0.56f}};

    // 单个小面（径向位置 tm∈[0,1]、方向 dir）的伪 gel 着色：半球假法线 →
    // dome 漫反 + Blinn 高光 + Fresnel 亮边 + 厚度 SSS。返回 clamp 到 [0,1] 的线性 RGB。
    inline glm::vec3 ShadeGel(glm::vec2 dir, float tm, const SlimeParams& s,
                              const glm::vec3& L, const glm::vec3& H)
    {
        const float nz   = std::sqrt(std::max(0.0f, 1.0f - tm * tm)); // 半球高度：中心 1 → 边 0
        const glm::vec3 n = glm::normalize(glm::vec3(dir.x * tm, dir.y * tm, nz));
        const float diff = std::clamp(glm::dot(n, L), 0.0f, 1.0f);
        const float spec = std::pow(std::clamp(glm::dot(n, H), 0.0f, 1.0f), s.specPower);
        const float fres = std::pow(1.0f - std::clamp(nz, 0.0f, 1.0f), s.rimPower);
        // 厚度 SSS：厚(center)取深绿、薄(edge)取亮体色。
        glm::vec3 col = glm::mix(s.deepColor, s.bodyColor, tm);
        col *= (s.ambient + (1.0f - s.ambient) * diff);   // dome 漫反
        col += s.rimColor * (fres * s.rimGain);           // Fresnel 亮边
        col += glm::vec3(1.0f) * (spec * s.specGain);     // 湿润高光
        return glm::clamp(col, glm::vec3(0.0f), glm::vec3(1.0f));
    }

    // 实心椭圆（三角扇），单色。用于发光眼 / 光斑 / 接触辉光。
    inline void FillEllipse(DebugDrawScene* dbg, glm::vec2 c, float rx, float ry,
                            std::uint32_t color, int seg = 16)
    {
        const float kTwoPi = 6.28318530718f;
        glm::vec3   prev(c.x + rx, c.y, 0.0f);
        for (int i = 1; i <= seg; ++i)
        {
            const float a = kTwoPi * static_cast<float>(i) / static_cast<float>(seg);
            glm::vec3   cur(c.x + std::cos(a) * rx, c.y + std::sin(a) * ry, 0.0f);
            dbg->AddTriangle(glm::vec3(c, 0.0f), prev, cur, color);
            prev = cur;
        }
    }

    // 把软体 blob 渲成"史莱姆 gel"：接触辉光 → 内部 rings×segments 逐面着色 →
    // 体内光斑 → 发光眼。全部平涂进 debug 三角（HDR + bloom 前），靠密度 + bloom 糊成渐变。
    void EmitSlimeGel(DebugDrawScene* dbg, const std::vector<glm::vec2>& ring,
                      glm::vec2 center, const SlimeParams& s, float t)
    {
        const int m = static_cast<int>(ring.size());
        if (m < 3)
        {
            return;
        }
        // 平均半径 + 最低点（接触辉光锚点）。
        float avgR = 0.0f, bottomY = ring[0].y;
        for (const auto& p : ring)
        {
            avgR += glm::length(p - center);
            bottomY = std::min(bottomY, p.y);
        }
        avgR /= static_cast<float>(m);
        if (avgR < 1e-4f)
        {
            return;
        }

        const glm::vec3 L = glm::normalize(s.lightDir);
        const glm::vec3 V(0.0f, 0.0f, 1.0f);
        const glm::vec3 H = glm::normalize(L + V);

        // —— 1. 接触辉光（body 之前画，作底部光晕）——
        if (s.drawContactGlow)
        {
            FillEllipse(dbg, glm::vec2(center.x, bottomY + avgR * 0.05f),
                        avgR * 1.15f, avgR * 0.22f, PackColorABGR(s.glowColor, 1.0f), 20);
        }

        // —— 2. 内部 rings×segments 逐面着色 ——
        const int R = std::max(2, s.rings);
        for (int i = 0; i < m; ++i)
        {
            const glm::vec2 da  = ring[i] - center;
            const glm::vec2 db  = ring[(i + 1) % m] - center;
            const glm::vec2 dir = glm::normalize(0.5f * (da + db));
            for (int r = 0; r < R; ++r)
            {
                const float t0 = static_cast<float>(r) / static_cast<float>(R);
                const float t1 = static_cast<float>(r + 1) / static_cast<float>(R);
                const float tm = 0.5f * (t0 + t1);
                const glm::vec3 col = ShadeGel(dir, tm, s, L, H);
                const std::uint32_t c = PackColorABGR(col, 1.0f);
                const glm::vec3 a0(center + da * t0, 0.0f);
                const glm::vec3 a1(center + da * t1, 0.0f);
                const glm::vec3 b0(center + db * t0, 0.0f);
                const glm::vec3 b1(center + db * t1, 0.0f);
                dbg->AddTriangle(a0, a1, b1, c);
                dbg->AddTriangle(a0, b1, b0, c);
            }
        }

        // —— 3. 体内漂浮光斑（缓慢上浮 + 明灭）——
        if (s.drawSpecks)
        {
            for (int k = 0; k < kSpeckCount; ++k)
            {
                const float     ph   = static_cast<float>(k) * 1.7f;
                const glm::vec2 off  = kSpeckLocal[k] * avgR * 0.9f +
                                      glm::vec2(0.0f, std::sin(t * 0.6f + ph) * avgR * 0.03f);
                const float     tw   = 0.6f + 0.4f * std::sin(t * 1.3f + ph); // 明灭
                FillEllipse(dbg, center + off, avgR * 0.045f, avgR * 0.06f,
                            PackColorABGR(s.speckColor * tw, 1.0f), 8);
            }
        }

        // —— 4. 发光眼（最上层；轻微呼吸缩放 → bloom 强发光）——
        if (s.drawEyes)
        {
            const float     pulse = 1.0f + 0.05f * std::sin(t * 2.5f);
            const float     rx    = s.eyeSize * 0.45f * avgR;
            const float     ry    = s.eyeSize * pulse * avgR;
            const glm::vec2 up    = glm::vec2(0.0f, avgR * 0.30f);
            const std::uint32_t eyeC = PackColorABGR(s.eyeColor, 1.0f);
            FillEllipse(dbg, center + up + glm::vec2(-avgR * 0.26f, 0.0f), rx, ry, eyeC, 14);
            FillEllipse(dbg, center + up + glm::vec2(avgR * 0.26f, 0.0f), rx, ry, eyeC, 14);
        }
    }

    // ===========================================================================
    // SpikeLayer —— Loop A 全部逻辑（输入 + 固定步长物理 + 手感 + overlay + 调参面板）。
    // ===========================================================================
    class SpikeLayer : public Layer
    {
    public:
        SpikeLayer(Pipeline& pipeline, World& world, Phys::PhysicsWorld& phys,
                   Phys::BodyHandle cp, std::vector<LevelBox> level, BloomPass* bloom = nullptr,
                   spike01::SlimeMetaballPass* sdfPass = nullptr)
            : Layer("SpikeLayer"), mPipeline(pipeline), mWorld(world), mPhys(phys), mCp(cp), mLevel(std::move(level)), mpBloom(bloom), mpSdfPass(sdfPass)
        {
            BuildActionMap();
            mVxHistory.fill(0.0f);

            // Loop B：blob 软体碰撞直接复用单一 mLevel（不再维护第二份几何副本，
            // 消除“两份 AABB 可能漂移”风险）；Blob::Step 只读 LevelBox 的 min/max。
        }

        // 留给 Loop B：软体 blob 每帧读这个快照，用弹簧吸附 control point。
        const ControlPointState& GetControlPoint() const noexcept
        {
            return mCpState;
        }

        // 平台事件：分辨率变化转给 pipeline；key 事件喂给 InputContext。
        bool OnEvent(const Platform::WindowEvent& event) override
        {
            if (auto* resize = std::get_if<Platform::WindowResizeEvent>(&event))
            {
                mPipeline.OnResize(resize->width, resize->height);
                return false;
            }
            if (auto* key = std::get_if<Platform::KeyEvent>(&event))
            {
                // Repeat 不喂（ActionMap 自己的状态机维护 Held）。
                if (key->action == Platform::KeyAction::Press)
                {
                    mInput.PostKeyEvent(static_cast<In::KeyCode>(key->key), true);
                }
                else if (key->action == Platform::KeyAction::Release)
                {
                    mInput.PostKeyEvent(static_cast<In::KeyCode>(key->key), false);
                }
            }
            return false;
        }

        void OnUpdate(const FrameContext& frame) override
        {
            // 主循环顺序：PollEvents → OnEvent（本帧 key 已 Post）→ OnUpdate（此处读状态）。
            // ActionMap.BeginFrame 放在本函数末尾推进状态，所以这里读到的是本帧新鲜的边沿。
            SampleInput();

            // —— 固定步长物理（accumulator，与渲染帧率解耦）——
            float dt = frame.time.deltaSeconds;
            dt       = std::clamp(dt, 0.0f, kMaxFrameDt);
            mRenderTime += dt; // gel 光斑漂移 / 眼呼吸 / 高光脉动（视觉动画，与物理步无关）
            mAccumulator += dt;
            mStepsThisFrame = 0;
            while (mAccumulator >= kFixedDt)
            {
                FixedStep(kFixedDt);
                mAccumulator -= kFixedDt;
                ++mStepsThisFrame;
            }

            // —— 拉取 control point 最新状态供 overlay / Loop B ——
            const auto bxf    = mPhys.GetBodyTransform(mCp);
            const auto bvel   = mPhys.GetLinearVelocity(mCp);
            mCpState.position = bxf.position;
            mCpState.velocity = bvel;
            mCpState.grounded = mGrounded;

            // 速度历史（判据 #1 延迟曲线）。
            mVxHistory[mVxHead] = bvel.x;
            mVxHead             = (mVxHead + 1) % static_cast<int>(mVxHistory.size());

            // —— Loop B：apex 分叉测量（spec 核心产出 = "果冻预算"）——
            // 分叉 = blob 视觉质心与 control point 的距离。读三个数：
            //   * 当前分叉 mDivergence；
            //   * 跳跃顶点那一刻的分叉 mApexDivergence（vy 由 + 翻 - 的瞬间采）；
            //   * 运行期峰值 mPeakDivergence（可重置）。
            if (mBlob.Initialized())
            {
                mBlobCentroid   = mBlob.Centroid();
                mDivergence     = glm::length(mBlobCentroid - mCpState.position);
                mPeakDivergence = std::max(mPeakDivergence, mDivergence);
                const float vy  = mCpState.velocity.y;
                if (!mGrounded && mPrevVy > 0.0f && vy <= 0.0f)
                {
                    mApexDivergence = mDivergence; // 跳跃顶点 = 玩家读图决定落点的瞬间
                }
                mPrevVy = vy;
            }

            if (mInputFlashTimer > 0.0f)
            {
                mInputFlashTimer = std::max(0.0f, mInputFlashTimer - dt);
            }
            if (mWallJumpFlash > 0.0f)
            {
                mWallJumpFlash = std::max(0.0f, mWallJumpFlash - dt);
            }

            // Tier 2 M1：把最新 blob 软体喂给 SDF pass（perimeter 世界坐标 + centroid +
            // radius）。Execute 在下面 Render 里用缓存写 UBO 逐像素出剪影。
            if (mpSdfPass && mBlob.Initialized())
            {
                const auto& bp = mBlob.Positions();
                mpSdfPass->SetBlob(bp.data(), mBlob.PerimeterCount(), mBlob.Centroid(),
                                   mBlobParams.blobRadius);
                mpSdfPass->SetTime(mRenderTime); // M3：眼呼吸 / 光斑明灭（视觉时钟，与物理步解耦）
            }

            DrawDebug();
            mPipeline.Render(mWorld);

            // 帧末推进输入状态机：Pressed→Held、Released→Idle。
            mInput.BeginFrame();
        }

        void OnImGui() override
        {
            // 面板右置（首次），把左侧 / 中央 viewport 让给 control point + blob
            // （出生点在左侧，避免被面板遮挡）；ImGuiCond_FirstUseEver 保留用户拖动。
            ImGui::SetNextWindowPos(ImVec2(944.0f, 8.0f), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(328.0f, 360.0f), ImGuiCond_FirstUseEver);
            ImGui::Begin("Loop A — control point 手感调参 (live, no recompile)");

            ImGui::TextUnformatted("操作: A/D 移动 | Space/W 跳跃 | 贴墙:按住朝墙下滑 · 贴墙跳=墙跳 · Shift 抓墙+↑↓攀爬");
            ImGui::TextUnformatted("一次只动一个旋钮，改完跑同一套测试路线对比。");
            ImGui::Separator();

            if (ImGui::CollapsingHeader("地面移动", ImGuiTreeNodeFlags_DefaultOpen))
            {
                ImGui::SliderFloat("max run speed (u/s)", &mParams.maxRunSpeed, 1.0f, 20.0f);
                ImGui::SliderFloat("ground accel", &mParams.groundAccel, 10.0f, 300.0f);
                ImGui::SliderFloat("ground decel", &mParams.groundDecel, 10.0f, 400.0f);
                ImGui::SliderFloat("air accel", &mParams.airAccel, 10.0f, 300.0f);
                ImGui::SliderFloat("air decel", &mParams.airDecel, 10.0f, 400.0f);
            }
            if (ImGui::CollapsingHeader("跳跃", ImGuiTreeNodeFlags_DefaultOpen))
            {
                ImGui::SliderFloat("jump speed (u/s)", &mParams.jumpSpeed, 5.0f, 30.0f);
                ImGui::SliderFloat("rise gravity", &mParams.riseGravity, 10.0f, 200.0f);
                ImGui::SliderFloat("fall gravity", &mParams.fallGravity, 10.0f, 200.0f);
                ImGui::SliderFloat("apex threshold (u/s)", &mParams.apexThreshold, 0.0f, 10.0f);
                ImGui::SliderFloat("apex gravity mult", &mParams.apexGravityMult, 0.1f, 1.0f);
                ImGui::SliderFloat("low-jump mult (松手切断)", &mParams.lowJumpMult, 1.0f, 6.0f);
                ImGui::SliderFloat("max fall speed (u/s)", &mParams.maxFallSpeed, 5.0f, 60.0f);
            }
            if (ImGui::CollapsingHeader("juice 辅助", ImGuiTreeNodeFlags_DefaultOpen))
            {
                ImGui::SliderFloat("coyote time (s)", &mParams.coyoteTime, 0.0f, 0.3f);
                ImGui::SliderFloat("jump buffer (s)", &mParams.jumpBufferTime, 0.0f, 0.3f);
                ImGui::SliderFloat("corner correction (u)", &mParams.cornerCorrectionDist, 0.0f, 0.6f);
            }
            if (ImGui::CollapsingHeader("爬墙 wall-climb (dogfood-pending)", ImGuiTreeNodeFlags_DefaultOpen))
            {
                ImGui::TextUnformatted("按住朝墙=下滑 | Shift=抓墙+↑↓攀爬 | 贴墙 Space/W=墙跳");
                ImGui::Checkbox("wall enabled", &mParams.wallEnabled);
                ImGui::SliderFloat("wall slide speed (u/s)", &mParams.wallSlideSpeed, 0.5f, 15.0f);
                ImGui::SliderFloat("wall jump speed X (u/s)", &mParams.wallJumpSpeedX, 2.0f, 25.0f);
                ImGui::SliderFloat("wall jump speed Y (u/s)", &mParams.wallJumpSpeedY, 5.0f, 30.0f);
                ImGui::SliderFloat("wall jump lock (s)", &mParams.wallJumpLockTime, 0.0f, 0.4f);
                ImGui::SliderFloat("wall coyote (s)", &mParams.wallCoyoteTime, 0.0f, 0.3f);
                ImGui::SliderFloat("wall climb speed (u/s)", &mParams.wallClimbSpeed, 1.0f, 12.0f);
                ImGui::SliderFloat("wall stamina (s, 0=inf)", &mParams.wallStamina, 0.0f, 10.0f);
                ImGui::SliderFloat("wall detect dist (u)", &mParams.wallDetectDist, 0.01f, 0.3f);
            }
            if (ImGui::CollapsingHeader("control point 尺寸"))
            {
                // 改尺寸经 ReplaceFixture 即时生效（见 SyncColliderSize）。
                ImGui::SliderFloat("cp half width", &mParams.cpHalfWidth, 0.05f, 0.39f);
                ImGui::SliderFloat("cp half height", &mParams.cpHalfHeight, 0.05f, 0.8f);
            }

            ImGui::Separator();
            ImGui::Text("FPS %.0f | 本帧固定步 %d", static_cast<double>(ImGui::GetIO().Framerate),
                        mStepsThisFrame);
            ImGui::Text("pos (%.2f, %.2f)  vel (%.2f, %.2f)",
                        mCpState.position.x, mCpState.position.y,
                        mCpState.velocity.x, mCpState.velocity.y);
            ImGui::Text("grounded %s | coyote %.3f | buffer %.3f",
                        mGrounded ? "YES" : "no ", mCoyoteTimer, mJumpBufferTimer);
            ImGui::Text("wall %s | %s%s | wallCoyote %.3f | lock %.3f",
                        mWallDir < 0 ? "LEFT " : (mWallDir > 0 ? "RIGHT" : "none "),
                        mSliding ? "SLIDE " : "", mGrabbing ? "GRAB" : "",
                        mWallCoyoteTimer, mWallJumpLockTimer);

            // 判据 #1：control point 水平速度曲线（看跟手延迟）。
            ImGui::PlotLines("vx history", mVxHistory.data(),
                             static_cast<int>(mVxHistory.size()), mVxHead, nullptr,
                             -mParams.maxRunSpeed * 1.2f, mParams.maxRunSpeed * 1.2f,
                             ImVec2(0.0f, 60.0f));

            // 判据 #2：连续 10 次起跳落点 x 散布。
            ImGui::Separator();
            ImGui::Text("落点采样 %zu / 10", mLandings.size());
            if (!mLandings.empty())
            {
                float lo = mLandings.front().x, hi = mLandings.front().x;
                for (const auto& p : mLandings)
                {
                    lo = std::min(lo, p.x);
                    hi = std::max(hi, p.x);
                }
                ImGui::Text("落点 x 散布 (max-min) = %.3f u", hi - lo);
            }
            if (ImGui::Button("清空落点"))
            {
                mLandings.clear();
            }
            ImGui::SameLine();
            if (ImGui::Button("重置 control point"))
            {
                ResetControlPoint();
            }

            ImGui::End();

            // ===================================================================
            // Loop B —— 软体 blob 视觉耦合调参（不碰 gameplay 数值，只管果冻跟得对不对）。
            // baseline = 单档软弹簧（spec：先 baseline 后加档；状态相关 stiffness
            // = 机制 1 是 Step B，本次只暴露单档 slider，不预先写状态机）。
            // ===================================================================
            ImGui::SetNextWindowPos(ImVec2(944.0f, 376.0f), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(328.0f, 336.0f), ImGuiCond_FirstUseEver);
            ImGui::Begin("Loop B — 软体 blob 视觉耦合 (live, no recompile)");

            ImGui::TextUnformatted("blob 用弹簧软软追 control point；纯位移分叉 = juice，");
            ImGui::TextUnformatted("只在读图决策瞬间（顶点/待机/对缝）才需要收敛回真相。");
            ImGui::Separator();

            if (ImGui::CollapsingHeader("弹簧吸附 (核心)", ImGuiTreeNodeFlags_DefaultOpen))
            {
                ImGui::SliderFloat("stiffness (吸附硬度)", &mBlobParams.stiffness, 20.0f, 1500.0f);
                ImGui::SliderFloat("damping (弹簧阻尼)", &mBlobParams.damping, 0.0f, 50.0f);
                ImGui::TextDisabled("提示: 临界阻尼 c=2*sqrt(k); 现 c<临界 = 欠阻尼(弹/晃)");
            }
            if (ImGui::CollapsingHeader("PBD 约束 (sim 先稳)", ImGuiTreeNodeFlags_DefaultOpen))
            {
                ImGui::SliderInt("约束迭代次数", &mBlobParams.constraintIters, 1, 20);
                ImGui::SliderFloat("edge stiffness (距离约束)", &mBlobParams.edgeStiffness, 0.0f, 1.0f);
                ImGui::SliderFloat("area stiffness (面积/保体积)", &mBlobParams.areaStiffness, 0.0f, 1.0f);
            }
            if (ImGui::CollapsingHeader("形状 / 渲染", ImGuiTreeNodeFlags_DefaultOpen))
            {
                ImGui::SliderFloat("blob radius (u)", &mBlobParams.blobRadius, 0.2f, 1.0f);
                ImGui::SliderFloat("collision skin (u)", &mBlobParams.skin, 0.0f, 0.1f);
                ImGui::Checkbox("画辐条", &mBlobParams.drawSpokes);
                ImGui::SameLine();
                ImGui::Checkbox("画 control point 标记", &mBlobParams.drawCpMarker);
                ImGui::Text("质点数: %d perimeter + 1 center (固定, 改在 Blob 构造)",
                            mBlob.PerimeterCount());
            }
            if (ImGui::CollapsingHeader("史莱姆 gel 外观 (Tier 1 CPU 着色)", ImGuiTreeNodeFlags_DefaultOpen))
            {
                ImGui::Checkbox("实心填充", &mSlime.fill);
                ImGui::SameLine();
                ImGui::Checkbox("线框对照", &mSlime.wireframe);
                ImGui::SameLine();
                ImGui::Checkbox("beauty(纯史莱姆黑底)", &mSlime.beauty);

                ImGui::SeparatorText("胶体分层色");
                ImGui::ColorEdit3("深绿(厚)", &mSlime.deepColor.x);
                ImGui::ColorEdit3("主体绿", &mSlime.bodyColor.x);
                ImGui::ColorEdit3("Fresnel 亮边", &mSlime.rimColor.x);
                ImGui::ColorEdit3("发光眼", &mSlime.eyeColor.x);
                ImGui::ColorEdit3("体内光斑", &mSlime.speckColor.x);
                ImGui::ColorEdit3("接触辉光", &mSlime.glowColor.x);

                ImGui::SeparatorText("伪光照 / 细节");
                ImGui::SliderFloat3("主光方向", &mSlime.lightDir.x, -1.0f, 1.0f);
                ImGui::SliderFloat("环境基线", &mSlime.ambient, 0.0f, 1.0f);
                ImGui::SliderFloat("高光锐度", &mSlime.specPower, 4.0f, 80.0f);
                ImGui::SliderFloat("高光强度", &mSlime.specGain, 0.0f, 2.0f);
                ImGui::SliderFloat("Fresnel 指数", &mSlime.rimPower, 1.0f, 8.0f);
                ImGui::SliderFloat("Fresnel 强度", &mSlime.rimGain, 0.0f, 2.0f);
                ImGui::SliderInt("径向环数", &mSlime.rings, 2, 24);
                ImGui::SliderInt("轮廓平滑段数", &mSlime.silhouetteSub, 1, 8);
                ImGui::SliderFloat("眼大小", &mSlime.eyeSize, 0.05f, 0.4f);
                ImGui::Checkbox("眼", &mSlime.drawEyes);
                ImGui::SameLine();
                ImGui::Checkbox("光斑", &mSlime.drawSpecks);
                ImGui::SameLine();
                ImGui::Checkbox("接触辉光", &mSlime.drawContactGlow);

                if (mpBloom)
                {
                    ImGui::SeparatorText("发光 (bloom 亮部晕开)");
                    ImGui::SliderFloat("bloom 阈值 (越低越易发光)", &mpBloom->threshold, 0.0f, 1.5f);
                    ImGui::SliderFloat("bloom 强度", &mpBloom->intensity, 0.0f, 2.0f);
                    ImGui::TextDisabled("8-bit 色上限=白; 靠调低阈值让亮色进 bloom。");
                }
                else
                {
                    ImGui::TextDisabled("(bloom pass 未接入)");
                }

                // Tier 2 M1：逐像素 SDF metaball pass 接管史莱姆渲染（跟随真 blob 出连续
                // 平滑剪影）。开启时默认隐藏 Tier 1 CPU gel；关掉退回 Tier 1 便于对比。
                if (mpSdfPass)
                {
                    ImGui::SeparatorText("Tier 2 SDF pass (M3 角色 gel, 接管渲染)");
                    bool sdfOn = mpSdfPass->IsEnabled();
                    if (ImGui::Checkbox("SDF metaball pass (逐像素 gel)", &sdfOn))
                    {
                        mpSdfPass->SetEnabled(sdfOn);
                    }
                    auto& t = mpSdfPass->GetTunables();
                    ImGui::SliderFloat("falloff scale (融合半径倍数)", &t.falloffScale, 0.4f, 2.5f);
                    ImGui::SliderFloat("iso level (剪影阈值)", &t.isoLevel, 0.1f, 2.0f);
                    ImGui::SliderFloat("dome scale (半球半径倍数)", &t.domeScale, 0.8f, 2.5f);
                    ImGui::SliderFloat("rim gain (Fresnel 亮边)", &t.rimGain, 0.0f, 3.0f);
                    ImGui::SliderFloat("spec gain (湿润高光)", &t.specGain, 0.0f, 3.0f);
                    ImGui::SliderFloat("ambient (环境基线)", &t.ambient, 0.0f, 1.0f);
                    ImGui::SliderFloat("eye gain (发光眼)", &t.eyeGain, 0.0f, 4.0f);
                    ImGui::SliderFloat("speck gain (体内光斑)", &t.speckGain, 0.0f, 3.0f);
                    ImGui::SliderFloat("glow gain (接触辉光)", &t.glowGain, 0.0f, 3.0f);
                    ImGui::TextDisabled("dome≈剪影出亮边; rim/spec 质感; eye/speck/glow 角色元素喂 bloom。");
                }
            }

            // —— apex 分叉读数（spec 核心产出 = "果冻预算"）——
            ImGui::Separator();
            ImGui::TextUnformatted("apex 分叉 = blob 视觉质心与 control point 的距离:");
            ImGui::Text("  当前分叉      : %.3f u", static_cast<double>(mDivergence));
            ImGui::Text("  最近顶点分叉  : %.3f u", static_cast<double>(mApexDivergence));
            ImGui::Text("  运行期峰值    : %.3f u", static_cast<double>(mPeakDivergence));
            ImGui::TextDisabled("用法: 跳到顶点冷眼看连线长短 + 读「最近顶点分叉」;");
            ImGui::TextDisabled("超过「开始觉得瞄不准」的值 = 果冻预算临界, 那时才上机制1。");
            if (ImGui::Button("重置峰值"))
            {
                mPeakDivergence = 0.0f;
                mApexDivergence = 0.0f;
            }
            ImGui::SameLine();
            if (ImGui::Button("重置 blob"))
            {
                mBlob.Reset(mCpState.position, mBlobParams.blobRadius);
            }

            ImGui::End();
        }

    private:
        // —— 输入：A/D 或方向键移动 + Space 跳跃 ——
        void BuildActionMap()
        {
            In::ActionMap map;
            auto          add = [&map](const char* name, std::initializer_list<In::KeyCode> keys)
            {
                In::Action a;
                a.name = name;
                for (auto k : keys)
                {
                    a.bindings.push_back({In::ActionBinding::Source::Key,
                                          static_cast<std::int32_t>(k)});
                }
                map.AddAction(std::move(a));
            };
            add("move_left", {In::KeyCode::A, In::KeyCode::Left});
            add("move_right", {In::KeyCode::D, In::KeyCode::Right});
            add("jump", {In::KeyCode::Space, In::KeyCode::W}); // Up 让给爬墙 climb_up
            // —— 爬墙输入 ——
            add("grab", {In::KeyCode::LeftShift, In::KeyCode::RightShift}); // hold 抓墙
            add("climb_up", {In::KeyCode::Up});
            add("climb_down", {In::KeyCode::Down});
            mInput.Push(std::move(map));
        }

        void SampleInput()
        {
            const In::ActionMap* top = mInput.Top();
            if (!top)
            {
                return;
            }
            const bool left  = In::IsHeld(top->GetState("move_left"));
            const bool right = In::IsHeld(top->GetState("move_right"));
            mMoveAxis        = (right ? 1.0f : 0.0f) - (left ? 1.0f : 0.0f);

            const In::ActionState js = top->GetState("jump");
            mJumpHeld                = In::IsHeld(js);
            const bool jumpPressed   = In::IsTriggered(js);
            if (jumpPressed)
            {
                mJumpBufferTimer = mParams.jumpBufferTime; // 预输入缓冲
            }

            // —— 爬墙输入：抓墙 hold + 上下攀爬 ——
            mGrabHeld      = In::IsHeld(top->GetState("grab"));
            mClimbUpHeld   = In::IsHeld(top->GetState("climb_up"));
            mClimbDownHeld = In::IsHeld(top->GetState("climb_down"));

            // 输入瞬间标记（判据 #1）：移动 / 跳跃任一刚按下就闪一下。
            const bool moveEdge = In::IsTriggered(top->GetState("move_left")) ||
                                  In::IsTriggered(top->GetState("move_right"));
            if (moveEdge || jumpPressed)
            {
                mInputFlashTimer = 0.15f;
            }
        }

        // —— 一个固定物理子步：手感逻辑 → Step → 回读对齐碰撞结果 ——
        void FixedStep(float dt)
        {
            SyncColliderSize(); // cp 尺寸 slider 改动经 ReplaceFixture 生效（帧外/Step 外）

            glm::vec2 pos = mPhys.GetBodyTransform(mCp).position;

            // 接地探测（引擎 RaycastClosest 向下投射，按 category/mask 只打关卡实心几何、
            // 自动排除 control point 自身；窗口/容差与原 analytic 逐字等价，见 ProbeGrounded）。
            const bool grounded = ProbeGrounded(pos);

            // coyote：在地面时刷满，离地后随时间衰减。
            if (grounded)
            {
                mCoyoteTimer = mParams.coyoteTime;
            }
            else
            {
                mCoyoteTimer = std::max(0.0f, mCoyoteTimer - dt);
            }

            // 落点采样（判据 #2）：空中→落地的跳变那一刻记录 x。
            if (grounded && !mWasGrounded && mVel.y <= 0.0f)
            {
                RecordLanding(pos);
            }
            mWasGrounded = grounded;
            mGrounded    = grounded;

            // —— 爬墙：侧向探墙 + coyote + 锁定计时 + 抓墙判定（在水平/跳跃/重力之前定状态）——
            // 窄缝里两壁都在探测范围内：**优先取玩家正按向的那面墙**（按意图选），否则任一侧有墙
            // 就取。这样在窄缝双壁间才能按方向交替墙跳往上爬（挤缝是 spike 核心卖点）——否则恒选
            // 固定一侧会导致墙跳方向永远朝同一边、爬不上去。
            int wallDir = 0;
            if (mParams.wallEnabled && !grounded)
            {
                const bool wallR = ProbeWall(pos, +1);
                const bool wallL = ProbeWall(pos, -1);
                if (mMoveAxis > 0.0f && wallR)
                    wallDir = +1; // 按右 + 右有墙
                else if (mMoveAxis < 0.0f && wallL)
                    wallDir = -1; // 按左 + 左有墙
                else if (wallR)
                    wallDir = +1; // 无方向意图：任一侧
                else if (wallL)
                    wallDir = -1;
            }
            mWallDir = wallDir;
            if (wallDir != 0)
            {
                mLastWallDir     = wallDir;
                mWallCoyoteTimer = mParams.wallCoyoteTime; // 贴墙时刷满
            }
            else
            {
                mWallCoyoteTimer = std::max(0.0f, mWallCoyoteTimer - dt);
            }
            if (mWallJumpLockTimer > 0.0f)
            {
                mWallJumpLockTimer = std::max(0.0f, mWallJumpLockTimer - dt);
            }
            // 接地 / 离墙回满耐力；抓墙判定（hold grab + 贴墙 + 有耐力）。
            if (grounded || wallDir == 0)
            {
                mStamina = mParams.wallStamina;
            }
            bool grabbing = mParams.wallEnabled && wallDir != 0 && !grounded && mGrabHeld &&
                            (mParams.wallStamina <= 0.0f || mStamina > 0.0f);

            // —— 水平：朝目标速度加/减速（空中控制单独减弱）。抓墙时不横向漂；墙跳锁定期禁"回墙"——
            float moveAxis = mMoveAxis;
            if (mWallJumpLockTimer > 0.0f && mWallJumpAwayDir != 0 && moveAxis != 0.0f &&
                (moveAxis > 0.0f) != (mWallJumpAwayDir > 0))
            {
                moveAxis = 0.0f; // 抑制朝刚跳离的墙加速
            }
            const float target = (grabbing ? 0.0f : moveAxis * mParams.maxRunSpeed);
            const float accel  = grounded ? mParams.groundAccel : mParams.airAccel;
            const float decel  = grounded ? mParams.groundDecel : mParams.airDecel;
            mVel.x             = Approach(mVel.x, target, accel, decel, dt);

            // 贴地：站立时不让向下速度累积（保持接触、便于 analytic 探测）。
            if (grounded && mVel.y <= 0.0f)
            {
                mVel.y = 0.0f;
            }

            // —— 跳跃：接地跳（buffer + coyote）优先；否则墙跳（buffer + 墙 coyote，非接地）——
            bool airborne = !grounded;
            if (mJumpBufferTimer > 0.0f && mCoyoteTimer > 0.0f)
            {
                mVel.y           = mParams.jumpSpeed;
                mJumpBufferTimer = 0.0f;
                mCoyoteTimer     = 0.0f;
                airborne         = true;
            }
            else if (mParams.wallEnabled && mJumpBufferTimer > 0.0f &&
                     mWallCoyoteTimer > 0.0f && mCoyoteTimer <= 0.0f)
            {
                // 墙跳：离墙方向 = 当前墙反向（coyote 窗口内已离墙则用最近墙 mLastWallDir 反向）。
                const int away     = (wallDir != 0) ? -wallDir : -mLastWallDir;
                mVel.y             = mParams.wallJumpSpeedY;
                mVel.x             = static_cast<float>(away) * mParams.wallJumpSpeedX;
                mWallJumpAwayDir   = away;
                mWallJumpLockTimer = mParams.wallJumpLockTime;
                mWallJumpFlash     = 0.15f;
                mJumpBufferTimer   = 0.0f;
                mWallCoyoteTimer   = 0.0f;
                airborne           = true;
                grabbing           = false; // 墙跳抵消本步抓墙（否则下面重力段会把 vy 覆盖回攀爬速度）
            }
            if (mJumpBufferTimer > 0.0f)
            {
                mJumpBufferTimer = std::max(0.0f, mJumpBufferTimer - dt);
            }

            // —— 抓墙：重力挂起，竖直由 ↑/↓ 攀爬输入控制（分层：只动 control point）——
            mGrabbing = grabbing;
            mSliding  = false;
            if (grabbing)
            {
                const float climb = (mClimbUpHeld ? 1.0f : 0.0f) - (mClimbDownHeld ? 1.0f : 0.0f);
                mVel.y            = climb * mParams.wallClimbSpeed;
                if (mParams.wallStamina > 0.0f)
                {
                    mStamina = std::max(0.0f, mStamina - dt);
                }
            }
            // —— 重力：上升 / 下落分档 + apex 衰减 + 松手切断上升（可变跳跃高度）——
            else if (airborne)
            {
                float g;
                if (mVel.y > 0.0f)
                {
                    if (std::abs(mVel.y) < mParams.apexThreshold)
                    {
                        g = mParams.riseGravity * mParams.apexGravityMult; // apex 悬停
                    }
                    else
                    {
                        // 上升中松手 → 重力放大，迅速收顶（可变跳跃高度）。
                        g = mParams.riseGravity * (mJumpHeld ? 1.0f : mParams.lowJumpMult);
                    }
                }
                else
                {
                    g = (std::abs(mVel.y) < mParams.apexThreshold)
                            ? mParams.fallGravity * mParams.apexGravityMult // apex 悬停（下落侧）
                            : mParams.fallGravity;
                }
                mVel.y -= g * dt;
                mVel.y = std::max(mVel.y, -mParams.maxFallSpeed);

                // —— 贴墙下滑：按住朝墙 + 下落 → 限速（比自由落体慢 = "抓住墙"手感）——
                const bool pressingIntoWall = wallDir != 0 && mMoveAxis != 0.0f &&
                                              (mMoveAxis > 0.0f) == (wallDir > 0);
                if (mParams.wallEnabled && pressingIntoWall && mVel.y < 0.0f)
                {
                    mVel.y   = std::max(mVel.y, -mParams.wallSlideSpeed);
                    mSliding = true;
                }
            }

            // —— 提交速度 + 步进物理（Box2D 解碰撞：撞墙/天花板会修正速度）——
            const float intendedVy = mVel.y;
            mPhys.SetLinearVelocity(mCp, mVel);
            mPhys.Step(dt);

            // 回读：让自管积分器与 Box2D 解出的碰撞结果对齐（撞墙 vx 归零等）。
            glm::vec2 postVel = mPhys.GetLinearVelocity(mCp);
            glm::vec2 postPos = mPhys.GetBodyTransform(mCp).position;

            // —— corner correction：上升中顶到角落（vy 被 Box2D 打没）时，
            //    尝试小幅水平推移让头部让开角落，恢复上升（平台跳跃老把戏）——
            if (mParams.cornerCorrectionDist > 0.0f && intendedVy > 0.1f &&
                postVel.y <= 0.05f && !ProbeGrounded(postPos))
            {
                if (TryCornerCorrect(postPos, intendedVy))
                {
                    postVel = mPhys.GetLinearVelocity(mCp);
                    postPos = mPhys.GetBodyTransform(mCp).position;
                }
            }

            mVel = postVel;

            // —— Loop B：在同一固定子步内推进软体 blob（spec 地基 #2：与 control point
            //    同步、固定步长；变步长会让 blob 易炸）。用本子步已解算的 control point
            //    位置当弹簧 target。**只读 postPos，不改 control point 手感逻辑。** ——
            //    碰撞几何直接用单一 mLevel（Blob::Step 只读其 min/max）。
            mBlob.Step(dt, postPos, mLevel, mBlobParams);
        }

        // 接地：脚底是否落在某关卡盒顶面附近（且未在上升）。
        // 迁移：原 analytic「脚底 y 落进某盒顶面窗口 [top-0.06, top+0.10] 且水平交叠」
        // 换成引擎 RaycastClosest 向下投射。为逐字保住原窗口：从脚底上方 kLandEps 处起
        // 射，maxDist = kLandEps + kProbeDist，则命中距离 d = (feetY+kLandEps) - top，
        // d ∈ [0, kLandEps+kProbeDist] ⇔ feetY - top ∈ [-kLandEps, +kProbeDist]，与原一致。
        // 起点抬高 kLandEps 也保证脚底略嵌入（≤kLandEps）时 origin 仍在顶面之上、射线
        // 从盒外向下干净命中顶面（raycast 不报 origin 处的 initial-overlap）。
        // 宽度：原 xOverlap 是整宽区间交叠；这里用左沿/中心/右沿三点采样近似——本关卡
        // 平台都远宽于 control point，边沿站立（中心悬出）能被对应边沿射线接住。
        bool ProbeGrounded(glm::vec2 pos) const
        {
            if (mVel.y > 0.05f)
            {
                return false; // 正在上升不算接地
            }
            const float     feetY      = pos.y - mParams.cpHalfHeight;
            constexpr float kLandEps   = 0.06f; // 容许略微嵌入 / 浮空
            constexpr float kProbeDist = 0.10f; // 脚下探测距离
            const float     originY    = feetY + kLandEps;
            const float     maxDist    = kLandEps + kProbeDist;
            // categoryBits=all（本查询代表“任何类”），maskBits=kSolidMask（只命中关卡实心
            // 几何；player 不在 mask 里 → 自动排除自身）。
            const Phys::QueryFilter filter{0xFFFFFFFFu, kSolidMask};
            for (const float sx : {pos.x - mParams.cpHalfWidth, pos.x, pos.x + mParams.cpHalfWidth})
            {
                const Phys::RaycastHit hit =
                    mPhys.RaycastClosest({sx, originY}, {0.0f, -1.0f}, maxDist, filter);
                if (hit.hit)
                {
                    return true;
                }
            }
            return false;
        }

        // 头部在给定 x 是否与任一 solid box 的顶角交叠（corner correction 探测）。
        // 迁移：原 analytic「头顶带盒 vs 每个非平地盒 AABB 交叠（isGround 跳过）」换成引擎
        // OverlapAABB（同一头顶带盒坐标）。filter = kCatWall（只墙 / 平台类）——直接对应原
        // HeadClearAt 跳过 ground 的语义（平地是脚下的，不参与顶角修正），不依赖"头顶盒几何上
        // 够不到平地"的关卡假设；且 kCatPlayer 天然不在 mask 里 → 排除自身无需手动剔除。
        // 返回列表非空 = 头没让开。
        // 注意：OverlapAABB 是 broad-phase（fat-AABB），可能过报（盒真实形状略在查询框外
        // 也报命中）→ corner correction 触发可能比原 analytic 略更积极；见交付报告 dogfood 项。
        bool HeadClearAt(float x, float y) const
        {
            const float                         headLo = y + mParams.cpHalfHeight - 0.05f;
            const float                         headHi = y + mParams.cpHalfHeight + 0.05f;
            const Phys::QueryFilter             filter{0xFFFFFFFFu, kCatWall};
            const std::vector<Phys::BodyHandle> hits = mPhys.OverlapAABB(
                {x - mParams.cpHalfWidth, headLo}, {x + mParams.cpHalfWidth, headHi}, filter);
            return hits.empty(); // 无实心几何交叠 = 头部让开
        }

        // 侧向探墙：从 control point 中心朝 dir(±1) 水平投三点（上/中/下）ray，命中 kCatWall =
        // 该侧有墙。origin 取 cp 中心（Box2D 保证 cp 不嵌入墙 → origin 恒在墙外，无 initial-overlap
        // 漏报）；maxDist = 半宽 + wallDetectDist，故贴墙 / 近墙 wallDetectDist 内都算贴。filter =
        // kCatWall（只竖面墙 / 平台，不含脚下平地 kCatGround），与 HeadClearAt 同源过滤。
        bool ProbeWall(glm::vec2 pos, int dir) const
        {
            if (dir == 0)
            {
                return false;
            }
            const float             maxDist = mParams.cpHalfWidth + mParams.wallDetectDist;
            const glm::vec2         rayDir{static_cast<float>(dir), 0.0f};
            const Phys::QueryFilter filter{0xFFFFFFFFu, kCatWall};
            for (const float sy : {pos.y - mParams.cpHalfHeight * 0.6f, pos.y,
                                   pos.y + mParams.cpHalfHeight * 0.6f})
            {
                const Phys::RaycastHit hit =
                    mPhys.RaycastClosest({pos.x, sy}, rayDir, maxDist, filter);
                if (hit.hit)
                {
                    return true;
                }
            }
            return false;
        }

        // 顶到角落时从最小推移开始两侧扫，找到第一处头部让开的 x 就吸附过去。
        bool TryCornerCorrect(glm::vec2 pos, float restoreVy)
        {
            for (float d = 0.05f; d <= mParams.cornerCorrectionDist + 1e-4f; d += 0.05f)
            {
                for (float dir : {1.0f, -1.0f})
                {
                    const float nx = pos.x + dir * d;
                    if (HeadClearAt(nx, pos.y))
                    {
                        Phys::BodyTransform t{};
                        t.position = {nx, pos.y};
                        mPhys.SetBodyTransform(mCp, t);
                        mVel.y = restoreVy;
                        mPhys.SetLinearVelocity(mCp, mVel);
                        return true;
                    }
                }
            }
            return false;
        }

        // cp 尺寸 slider 改动 → ReplaceFixture 重建 collider（必须在 Step 之外调）。
        void SyncColliderSize()
        {
            if (mParams.cpHalfWidth == mLastHalfW && mParams.cpHalfHeight == mLastHalfH)
            {
                return;
            }
            Phys::ColliderComponent col;
            col.shape        = Phys::BoxDesc{{mParams.cpHalfWidth, mParams.cpHalfHeight}, {0.0f, 0.0f}};
            col.density      = 1.0f;
            col.friction     = 0.0f; // 水平由手感逻辑全权控制，不让地面摩擦掺和
            col.restitution  = 0.0f;
            col.categoryBits = kCatPlayer;  // ReplaceFixture 会重置过滤位，须显式重设
            col.maskBits     = 0xFFFFFFFFu; // 仍与一切碰撞（行为不变）
            mPhys.ReplaceFixture(mCp, col);
            mLastHalfW = mParams.cpHalfWidth;
            mLastHalfH = mParams.cpHalfHeight;
        }

        void RecordLanding(glm::vec2 pos)
        {
            const float feetY = pos.y - mParams.cpHalfHeight;
            mLandings.push_back({pos.x, feetY});
            if (mLandings.size() > 10)
            {
                mLandings.erase(mLandings.begin());
            }
        }

        void ResetControlPoint()
        {
            Phys::BodyTransform t{};
            t.position = kSpawn;
            mPhys.SetBodyTransform(mCp, t);
            mPhys.SetLinearVelocity(mCp, {0.0f, 0.0f});
            mVel             = {0.0f, 0.0f};
            mCoyoteTimer     = 0.0f;
            mJumpBufferTimer = 0.0f;

            // Loop B：blob 跟着回到出生点静止起步（避免重置后弹簧从远处猛甩）。
            mBlob.Reset(kSpawn, mBlobParams.blobRadius);
            mApexDivergence = 0.0f;
            mPeakDivergence = 0.0f;
            mPrevVy         = 0.0f;
        }

        // —— overlay：场景碰撞盒 + control point 标记 + 速度向量 + 输入标记 + 落点 ——
        void DrawDebug()
        {
            auto* dbg = mPipeline.GetDebugDrawScene();
            if (!dbg)
            {
                return;
            }

            // beauty 展示模式：隐藏关卡线框 + 所有 gameplay overlay，只留史莱姆(黑底)对齐参考图。
            // 关掉即恢复 Loop A/B 全部调试可视（cp 框 / 速度 / 落点 / apex 分叉线）。
            if (!mSlime.beauty)
            {
            // 静态世界（debug 线保留作可视参照；同时就是真 Box2D 碰撞体的轮廓）。
            for (const auto& b : mLevel)
            {
                dbg->AddAabb(glm::vec3(b.min, 0.0f), glm::vec3(b.max, 0.0f), b.color);
            }

            const glm::vec2 p = mCpState.position;
            const glm::vec3 c3{p, 0.0f};

            // control point 小框（gameplay 真相层）。Loop B 起 blob 是主可见物，
            // 这里只作叠加小标记对比"软体 vs 隐藏真相"，可经 slider 关。
            if (mBlobParams.drawCpMarker)
            {
                const glm::vec3 half{mParams.cpHalfWidth, mParams.cpHalfHeight, 0.0f};
                dbg->AddAabb(c3 - half, c3 + half, kYellow);
            }

            // 接地指示：脚底一条短横线（grey=空中, yellow=接地）。
            const float feetY = p.y - mParams.cpHalfHeight;
            dbg->AddLine(glm::vec3(p.x - mParams.cpHalfWidth, feetY, 0.0f),
                         glm::vec3(p.x + mParams.cpHalfWidth, feetY, 0.0f),
                         mGrounded ? kYellow : kGrey);

            // —— 爬墙 overlay：贴墙侧竖线（grey=触墙 / cyan=下滑 / yellow=抓墙）——
            if (mWallDir != 0)
            {
                const float         wx = p.x + static_cast<float>(mWallDir) * mParams.cpHalfWidth;
                const std::uint32_t wc = mGrabbing ? kYellow : (mSliding ? kCyan : kGrey);
                dbg->AddLine(glm::vec3(wx, p.y - mParams.cpHalfHeight, 0.0f),
                             glm::vec3(wx, p.y + mParams.cpHalfHeight, 0.0f), wc);
            }
            // 墙跳闪：从 cp 指向离墙方向的短线（离墙起跳瞬间）。
            if (mWallJumpFlash > 0.0f)
            {
                dbg->AddLine(glm::vec3(p, 0.0f),
                             glm::vec3(p.x + static_cast<float>(mWallJumpAwayDir) * 0.5f,
                                       p.y + 0.2f, 0.0f),
                             kWhite);
            }

            // 速度向量（判据 #1：跟手延迟肉眼可读）。
            const glm::vec2 v = mCpState.velocity;
            dbg->AddLine(c3, glm::vec3(p + v * 0.1f, 0.0f), kMagenta);

            // 输入瞬间标记（判据 #1）：刚按下时在 cp 上方闪一个白三角 + 竖线。
            if (mInputFlashTimer > 0.0f)
            {
                const float y0 = p.y + mParams.cpHalfHeight + 0.15f;
                dbg->AddTriangle(glm::vec3(p.x - 0.15f, y0, 0.0f),
                                 glm::vec3(p.x + 0.15f, y0, 0.0f),
                                 glm::vec3(p.x, y0 + 0.3f, 0.0f), kWhite);
                dbg->AddLine(glm::vec3(p.x, p.y, 0.0f), glm::vec3(p.x, y0, 0.0f), kWhite);
            }

            // 落点散布打点（判据 #2）：每次落地的 x 处一根橙色短竖线。
            for (const auto& lp : mLandings)
            {
                dbg->AddLine(glm::vec3(lp.x, lp.y, 0.0f),
                             glm::vec3(lp.x, lp.y + 0.35f, 0.0f), kAmber);
            }
            } // if (!mSlime.beauty)

            // —— Loop B：软体 blob（主可见物 = 史莱姆 gel 渲染）——
            if (mBlob.Initialized())
            {
                const auto& bp = mBlob.Positions();
                const int   n  = mBlob.PerimeterCount();

                // —— Tier 0 实心史莱姆：中心扇形填充 + 2 环径向渐变（core→edge）——
                // AddTriangle 输出到 HDR target、在 bloom 之前 → 亮色自动晕开发光
                // （见 DebugDrawScene 头 18-23 行）。轮廓经 Catmull-Rom 平滑去 faceted。
                // Tier 2 SDF pass 开启时接管史莱姆渲染，跳过 Tier 1 CPU gel（关掉 SDF 恢复）。
                const bool sdfActive = mpSdfPass && mpSdfPass->IsEnabled();
                if (mSlime.fill && !sdfActive)
                {
                    std::vector<glm::vec2>       peri(bp.begin(), bp.begin() + n);
                    const std::vector<glm::vec2> ring = SmoothClosedCatmullRom(peri, mSlime.silhouetteSub);
                    const glm::vec2              ctr  = bp[mBlob.CenterIndex()];
                    EmitSlimeGel(dbg, ring, ctr, mSlime, mRenderTime);
                }

                // debug 线框（perimeter loop + 辐条）：填充上线后默认关，作对照可开。
                if (mSlime.wireframe)
                {
                    for (int i = 0; i < n; ++i)
                    {
                        const int j = (i + 1) % n;
                        dbg->AddLine(glm::vec3(bp[i], 0.0f), glm::vec3(bp[j], 0.0f), kBlob);
                    }
                    if (mBlobParams.drawSpokes)
                    {
                        const glm::vec3 ctr(bp[mBlob.CenterIndex()], 0.0f);
                        for (int i = 0; i < n; ++i)
                        {
                            dbg->AddLine(ctr, glm::vec3(bp[i], 0.0f), kSpoke);
                        }
                    }
                }

                // apex 分叉 overlay（spec 核心产出）：control point ↔ blob 视觉质心
                // 连线 + 质心十字标记。连线越长 = 果冻越"骗人"，这就是要量的预算。beauty 模式隐藏。
                if (!mSlime.beauty)
                {
                    const glm::vec3 cpw(mCpState.position, 0.0f);
                    const glm::vec3 ctw(mBlobCentroid, 0.0f);
                    dbg->AddLine(cpw, ctw, kDiverge);
                    constexpr float km = 0.08f;
                    dbg->AddLine(ctw - glm::vec3(km, 0.0f, 0.0f), ctw + glm::vec3(km, 0.0f, 0.0f),
                                 kCentroid);
                    dbg->AddLine(ctw - glm::vec3(0.0f, km, 0.0f), ctw + glm::vec3(0.0f, km, 0.0f),
                                 kCentroid);
                }
            }
        }

        // 引擎引用 / 物理
        Pipeline&             mPipeline;
        World&                mWorld;
        Phys::PhysicsWorld&   mPhys;
        Phys::BodyHandle      mCp;
        std::vector<LevelBox> mLevel;
        BloomPass*            mpBloom = nullptr; // Tier 0 slime glow：live 调 bloom 阈值/强度
        SlimeParams           mSlime;            // Tier 0 实心史莱姆外观
        spike01::SlimeMetaballPass* mpSdfPass = nullptr; // Tier 2 M0：SDF pass 运行时开关

        // 输入
        In::InputContext mInput;
        float            mMoveAxis      = 0.0f;
        bool             mJumpHeld      = false;
        bool             mGrabHeld      = false; // 抓墙 hold
        bool             mClimbUpHeld   = false; // 攀爬 ↑
        bool             mClimbDownHeld = false; // 攀爬 ↓

        // 手感状态
        FeelParams mParams;
        glm::vec2  mVel{0.0f, 0.0f};
        bool       mGrounded        = false;
        bool       mWasGrounded     = false;
        float      mCoyoteTimer     = 0.0f;
        float      mJumpBufferTimer = 0.0f;

        // —— 爬墙状态 ——
        int   mWallDir           = 0;     // 当前贴墙方向：-1 左, +1 右, 0 无（供 overlay）
        int   mLastWallDir       = 0;     // 最近一次贴墙方向（wall coyote 窗口内墙跳用）
        bool  mSliding           = false; // 贴墙下滑中（overlay）
        bool  mGrabbing          = false; // 抓墙中（overlay）
        float mWallCoyoteTimer   = 0.0f;  // 离墙后墙跳宽限
        float mWallJumpLockTimer = 0.0f;  // 墙跳后抑制回墙输入
        int   mWallJumpAwayDir   = 0;     // 墙跳离墙方向（锁定期抑制回墙加速）
        float mStamina           = 0.0f;  // 剩余抓墙耐力 s
        float mWallJumpFlash     = 0.0f;  // overlay：墙跳闪计时

        float mLastHalfW = -1.0f; // 触发首帧 ReplaceFixture 同步
        float mLastHalfH = -1.0f;

        // 物理累加器
        float mAccumulator    = 0.0f;
        int   mStepsThisFrame = 0;
        float mRenderTime     = 0.0f; // 视觉动画时钟（gel 光斑/眼/高光；与固定物理步解耦）

        // 对外快照（Loop B）
        ControlPointState mCpState;

        // —— Loop B：软体 blob 视觉表现层 ——
        spike01::Blob       mBlob{14}; // 14 perimeter + 1 center 质点
        spike01::BlobParams mBlobParams;
        glm::vec2           mBlobCentroid{0.0f, 0.0f};
        float               mDivergence     = 0.0f; // 当前 |质心 - cp|
        float               mApexDivergence = 0.0f; // 最近跳跃顶点的分叉
        float               mPeakDivergence = 0.0f; // 运行期峰值（可重置）
        float               mPrevVy         = 0.0f; // 上帧 cp 垂直速度（apex 检测）

        // overlay / 判据
        float                  mInputFlashTimer = 0.0f;
        std::array<float, 180> mVxHistory{};
        int                    mVxHead = 0;
        std::vector<glm::vec2> mLandings;
    };

} // namespace

int main()
{
    AppConfig cfg{};
    cfg.window.title  = "OrangeGames - spike-01-blob (Loop A control point + Loop B soft blob)";
    cfg.window.width  = 1280;
    cfg.window.height = 720;

    auto hostResult = AppHost::Create(cfg);
    if (hostResult.IsErr())
    {
        std::fprintf(stderr, "AppHost::Create failed (code=%u)\n",
                     static_cast<unsigned>(hostResult.Error()));
        return 1;
    }
    auto host = std::move(hostResult).Value();

    AssetRegistry assets;
    if (auto reg = assets.RegisterLoader<ShaderAsset>(std::make_unique<ShaderLoader>());
        reg.IsErr())
    {
        std::fprintf(stderr, "RegisterLoader<ShaderAsset> failed\n");
        return 1;
    }

    MaterialSystem materials(assets);
    if (auto rb = materials.RegisterBuiltins(); rb.IsErr())
    {
        std::fprintf(stderr, "RegisterBuiltins failed\n");
        return 1;
    }

    World world;

    // 正交 2D 相机：一屏覆盖约 16×9 世界单位，正对 XY 平面（沿 -Z 看）。
    Entity camEntity = world.CreateEntity();
    {
        // 拉近对准史莱姆静止区（出生点 (-5,-2.5) 落到平地顶 y=-3 上，静止约 (-5,-2.45)）——
        // 本阶段是"实体渲染"showcase，相机聚焦主角看清 gel 外观；后续加 follow-cam 另说。
        const float halfH  = 1.9f;
        const float aspect = static_cast<float>(cfg.window.width) / static_cast<float>(cfg.window.height);
        const float halfW  = halfH * aspect;
        const glm::vec3 focus(-5.0f, -2.15f, 0.0f);
        Camera      cam    = Camera::Orthographic(-halfW, halfW, -halfH, halfH, 0.1f, 100.0f);
        cam.view           = glm::lookAt(focus + glm::vec3(0.0f, 0.0f, 10.0f), focus,
                                         glm::vec3(0.0f, 1.0f, 0.0f));
        world.AddComponent(camEntity, cam);
    }

    // ---------- 物理：世界重力归零（我们在 control point 上自管重力，
    //            以便上升 / 下落 / apex 分档），Box2D 只负责碰撞解算 ----------
    Phys::PhysicsWorldDesc pdesc{};
    pdesc.gravity = {0.0f, 0.0f};
    Phys::PhysicsWorld physWorld(pdesc);

    const std::vector<LevelBox> level = MakeLevel();

    // 静态世界：每个 LevelBox 建一个 Box2D 静态 body。
    for (const auto& b : level)
    {
        const glm::vec2          center = 0.5f * (b.min + b.max);
        const glm::vec2          half   = 0.5f * (b.max - b.min);
        Phys::RigidBodyComponent rb;
        rb.type            = Phys::BodyType::Static;
        rb.initialPosition = center;
        Phys::ColliderComponent col;
        col.shape    = Phys::BoxDesc{half, {0.0f, 0.0f}};
        col.friction = 0.0f;
        // 分类：平地 kCatGround，平台 / 墙 kCatWall；mask 全通 → 碰撞行为不变，
        // 仅供空间查询按类过滤（接地 / 头顶探针只打 kSolidMask，排除 control point）。
        col.categoryBits = b.isGround ? kCatGround : kCatWall;
        col.maskBits     = 0xFFFFFFFFu;
        if (!physWorld.AddBody(rb, col).IsValid())
        {
            std::fprintf(stderr, "AddBody(static level box) failed\n");
            return 1;
        }
    }

    // control point：dynamic 刚体（velocity 驱动 + 自管重力），fixedRotation 锁转，
    // gravityScale=0（重力我们自己加）。小 box collider，无摩擦无弹性。
    // 选 Dynamic 而非 Kinematic：Box2D 中 kinematic 体不与 static 体产生碰撞响应，
    // 而 control point 必须撞墙 / 踩平台（见 NOTES-loopA.md）。
    Phys::BodyHandle cpBody;
    {
        Phys::RigidBodyComponent rb;
        rb.type            = Phys::BodyType::Dynamic;
        rb.initialPosition = kSpawn;
        rb.fixedRotation   = true;
        rb.gravityScale    = 0.0f;
        Phys::ColliderComponent col;
        col.shape        = Phys::BoxDesc{{0.18f, 0.28f}, {0.0f, 0.0f}};
        col.density      = 1.0f;
        col.friction     = 0.0f;
        col.restitution  = 0.0f;
        col.categoryBits = kCatPlayer;  // control point 类；查询用 mask 排除自身
        col.maskBits     = 0xFFFFFFFFu; // 仍与一切碰撞（撞墙 / 踩平台，行为不变）
        cpBody           = physWorld.AddBody(rb, col);
        if (!cpBody.IsValid())
        {
            std::fprintf(stderr, "AddBody(control point) failed\n");
            return 1;
        }
    }

    Pipeline pipeline;
    if (auto r = pipeline.Initialize(host->GetWindow(), assets); r.IsErr())
    {
        std::fprintf(stderr, "Pipeline::Initialize failed (code=%u)\n",
                     static_cast<unsigned>(r.Error()));
        return 1;
    }
    pipeline.SetMaterialSystem(&materials);

    // 史莱姆 showcase 背景：关程序化天空 + 场景清到近黑(略带暗绿)，让 gel 在暗底上发光，
    // 对齐参考图纯黑森林调。运行时 beauty 关掉仍是这背景（关卡线框自会画回来）。
    pipeline.SetSkyEnabled(false);
    pipeline.SetSceneClearColor(0.012f, 0.020f, 0.016f);

    // 接通引擎托管 ImGui overlay + 把 LayerStack 的 OnImGui 派发接进来（调参面板）。
    if (auto im = pipeline.EnableImGui(); im.IsErr())
    {
        std::fprintf(stderr, "Pipeline::EnableImGui failed (code=%u)\n",
                     static_cast<unsigned>(im.Error()));
        return 1;
    }
    pipeline.SetImGuiSubmit([h = host.get()]()
                            { h->DispatchImGui(); });

    // —— Tier 0 slime glow：加 bloom 后处理链（消费者侧，与 sample 08 同路）——
    // 史莱姆填充色走 HDR target，bloom 把亮部晕开成发光。chain 须活到 Run() 结束。
    PostProcessChain slimeChain = Orange::Engine::Render::BuiltinPostProcessChain::CreateDefault();
    BloomPass*       slimeBloom = dynamic_cast<BloomPass*>(slimeChain.FindByName("bloom"));
    if (slimeBloom)
    {
        // 阈值偏高：只让 Fresnel 亮边 / 眼 / 高光进 bloom，body 主体保留 dome 渐变不被洗白
        //（对齐参考图"深绿通透 body + 眼强对比跳出"的层次）。
        slimeBloom->threshold = 0.72f;
        slimeBloom->intensity = 0.85f;
    }
    pipeline.SetPostProcessChain(&slimeChain);

    // —— Tier 2：注入自定义 fullscreen SDF metaball pass（M1 接真 blob 出剪影）——
    // InsertPass(AfterMainPass) —— 主 pass + 粒子已写完 HDR、bloom 还没跑的 hook 点，
    // pass 自己 transition + BeginRendering + Draw + EndRendering（见 SlimeMetaballPass）。
    // 输出叠在 HDR 上 → 自动喂现有 bloom 链。Pipeline 立即调一次 Setup 建 GPU 资源。
    auto                        slimeSdfPass = std::make_unique<spike01::SlimeMetaballPass>();
    spike01::SlimeMetaballPass* pSlimeSdfPass = slimeSdfPass.get();
    pipeline.InsertPass(Orange::Engine::Render::PipelineStage::AfterMainPass, std::move(slimeSdfPass));

    host->PushLayer(std::make_unique<SpikeLayer>(pipeline, world, physWorld, cpBody, level, slimeBloom, pSlimeSdfPass));

    const int rc = host->Run();
    pipeline.Shutdown();
    return rc;
}
