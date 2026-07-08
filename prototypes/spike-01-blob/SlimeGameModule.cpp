// spike-01-blob —— SlimeGameModule 实现（原 main.cpp SpikeLayer 全部玩法逻辑）。
// 引擎引用经 GameModuleContext 传入（mpPhysics / mpPipeline）；宿主管渲染 / resize /
// bloom；Play 期 2D 相机与暗场背景归模块（见头文件分工注释）。Blob.h +
// SlimeMetaballPass.h 原地复用。

#include "SlimeGameModule.h"
#include "SlimeMetaballPass.h"
#include "SlimeTuningComponent.h"

// consumer 侧经便利聚合头引入引擎公共 API（与 main.cpp 同路）。
#include <orange/engine/input.h>
#include <orange/engine/physics.h>
#include <orange/engine/platform.h>
#include <orange/engine/render.h>
#include <orange/engine/scene/NameComponent.h>      // Play 期相机实体在 Entity Tree 可辨识
#include <orange/engine/scene/TransformComponent.h>
#include <orange/engine/scene/World.h> // ApplyTuningOverrides 遍历 entt view

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <span>
#include <utility>
#include <variant>
#include <vector>

using namespace Orange::Engine;
using Orange::Engine::Render::DebugDrawScene;
namespace In   = Orange::Engine::Input;
namespace Phys = Orange::Engine::Physics;

namespace spike01
{
    namespace
    {

        // debug draw 颜色：ABGR packed（低 8 位 R，0xAA_BB_GG_RR）。
        constexpr std::uint32_t kGreen    = 0xFF00FF00u; // 平地
        constexpr std::uint32_t kCyan     = 0xFFFFFF00u; // 平台
        constexpr std::uint32_t kRed      = 0xFF0000FFu; // 窄缝两壁
        constexpr std::uint32_t kAmber    = 0xFF1A78FFu; // 落点打点
        constexpr std::uint32_t kYellow   = 0xFF00FFFFu; // control point 标记
        constexpr std::uint32_t kMagenta  = 0xFFFF00FFu; // 速度向量
        constexpr std::uint32_t kWhite    = 0xFFFFFFFFu; // 输入瞬间标记
        constexpr std::uint32_t kGrey     = 0xFF808080u; // 接地指示
        constexpr std::uint32_t kBlob     = 0xFF66FF66u; // blob perimeter loop
        constexpr std::uint32_t kSpoke    = 0xFF338833u; // blob center 辐条
        constexpr std::uint32_t kDiverge  = 0xFF00A5FFu; // apex 分叉连线
        constexpr std::uint32_t kCentroid = 0xFF00D7FFu; // blob 视觉质心十字

        // 固定物理步长 60Hz（与渲染帧率解耦）；单帧累加器上限防长帧一次推进过多子步。
        constexpr float     kFixedDt    = 1.0f / 60.0f;
        constexpr float     kMaxFrameDt = 0.10f;
        constexpr glm::vec2 kSpawn{-5.0f, -2.5f}; // control point 出生点

        // 碰撞过滤位：kCatPlayer=control point，kCatGround=平地，kCatWall=平台/墙。
        // maskBits 恒全通 → 碰撞行为不变，category 仅供空间查询按类过滤（排除自身）。
        constexpr std::uint32_t kCatPlayer = 0x1u;
        constexpr std::uint32_t kCatGround = 0x2u;
        constexpr std::uint32_t kCatWall   = 0x4u;
        constexpr std::uint32_t kSolidMask = kCatGround | kCatWall;

        // 状态检测阈值（D1 形变 §2）。
        constexpr float kLandingImpactThresh = 5.0f;
        constexpr float kLandingDuration     = 0.12f;
        constexpr float kLaunchDuration      = 0.10f;
        constexpr float kSlideVxThresh       = 6.0f;

        // 固定体内光斑局部偏移（单位圆内偏下半区，确定性布点无运行时随机）。
        constexpr int       kSpeckCount               = 14;
        constexpr glm::vec2 kSpeckLocal[kSpeckCount] = {
            {-0.35f, -0.30f}, {0.20f, -0.45f}, {-0.10f, -0.55f}, {0.42f, -0.20f},
            {-0.50f, -0.05f}, {0.05f, -0.28f}, {0.30f, -0.60f}, {-0.25f, -0.62f},
            {0.52f, -0.36f}, {-0.55f, -0.40f}, {0.15f, -0.72f}, {-0.05f, -0.44f},
            {0.36f, -0.50f}, {-0.40f, -0.56f}};

        // 逐状态形变目标（sx 横向 / sy 纵向缩放相对基础圆；stiffness 当帧弹簧硬度）。
        struct MotionShape
        {
            float sx;
            float sy;
            float stiffness;
        };

        MotionShape MotionShapeTarget(SlimeMotionState s)
        {
            switch (s)
            {
                case SlimeMotionState::Idle:     return {1.15f, 0.80f, 200.0f}; // 矮 dome
                case SlimeMotionState::Crouch:   return {1.42f, 0.60f, 420.0f}; // 蓄力宽扁
                case SlimeMotionState::Launch:   return {0.70f, 1.55f, 120.0f}; // 高瘦柱
                case SlimeMotionState::Rising:   return {0.95f, 1.10f, 200.0f}; // 饱满略纵长
                case SlimeMotionState::Falling:  return {0.82f, 1.28f, 180.0f}; // 纵长水滴倾向
                case SlimeMotionState::Landing:  return {1.60f, 0.42f, 350.0f}; // 极扁冲击
                case SlimeMotionState::Sliding:  return {1.50f, 0.72f, 200.0f}; // 横躺拉长
                case SlimeMotionState::Rolling:  return {1.24f, 0.82f, 230.0f}; // 椭圆（旋转在 UpdateDeform 叠加）
                case SlimeMotionState::Squeezed: return {1.34f, 0.66f, 320.0f}; // 窄缝压扁
            }
            return {1.15f, 0.80f, 200.0f};
        }

        // Spike 1 关卡：平地 + 左/右平台 + 墙跳 chimney + squeeze 隧道。单一数据源：
        // 同一 box 列表既建静态体、又喂 debug 线框、又作 Blob 碰撞 AABB。
        std::vector<LevelBox> MakeLevel()
        {
            return {
                {{-9.0f, -3.5f}, {9.0f, -3.0f}, kGreen, true},      // 平地
                {{-6.0f, -0.7f}, {-2.5f, -0.5f}, kCyan, false},     // 左平台
                {{2.5f, 0.3f}, {6.0f, 0.5f}, kCyan, false},         // 右平台

                {{-0.85f, -1.3f}, {-0.7f, 4.0f}, kRed, false},      // chimney 左壁（底部浮起留入口）
                {{0.7f, -3.0f}, {0.85f, 4.0f}, kRed, false},        // chimney 右壁
                {{0.85f, 3.8f}, {3.0f, 4.0f}, kCyan, false},        // 顶部 mantle 平台

                {{6.5f, -3.0f}, {6.6f, -1.0f}, kRed, false},        // squeeze 隧道左壁
                {{7.3f, -3.0f}, {7.4f, -1.0f}, kRed, false},        // squeeze 隧道右壁
                {{6.5f, -1.0f}, {7.4f, -0.85f}, kCyan, false},      // 隧道顶盖
            };
        }

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

        // glm 颜色 (0..1 RGB) → DebugDrawScene 的 packed ABGR uint32（低 8 位 R）。
        inline std::uint32_t PackColorABGR(const glm::vec3& c, float a = 1.0f)
        {
            auto q = [](float v)
            { return static_cast<std::uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
            return (q(a) << 24) | (q(c.z) << 16) | (q(c.y) << 8) | q(c.x);
        }

        // 闭合 Catmull-Rom 平滑：把 N 点环每段插成 sub 段，让 blob 轮廓不那么 faceted。
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

        // 单个小面（径向位置 tm∈[0,1]、方向 dir）的伪 gel 着色：半球假法线 →
        // dome 漫反 + Blinn 高光 + Fresnel 亮边 + 厚度 SSS。返回 clamp 到 [0,1] 线性 RGB。
        inline glm::vec3 ShadeGel(glm::vec2 dir, float tm, const SlimeParams& s,
                                  const glm::vec3& L, const glm::vec3& H)
        {
            const float     nz = std::sqrt(std::max(0.0f, 1.0f - tm * tm)); // 半球高度：中心 1 → 边 0
            const glm::vec3 n  = glm::normalize(glm::vec3(dir.x * tm, dir.y * tm, nz));
            const float     diff = std::clamp(glm::dot(n, L), 0.0f, 1.0f);
            const float     spec = std::pow(std::clamp(glm::dot(n, H), 0.0f, 1.0f), s.specPower);
            const float     fres = std::pow(1.0f - std::clamp(nz, 0.0f, 1.0f), s.rimPower);
            glm::vec3       col  = glm::mix(s.deepColor, s.bodyColor, tm); // 厚度 SSS：厚深绿→薄亮体
            col *= (s.ambient + (1.0f - s.ambient) * diff);                // dome 漫反
            col += s.rimColor * (fres * s.rimGain);                        // Fresnel 亮边
            col += glm::vec3(1.0f) * (spec * s.specGain);                  // 湿润高光
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
            float avgR = 0.0f, bottomY = ring[0].y; // 平均半径 + 最低点（接触辉光锚点）
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

            // 1. 接触辉光（body 之前画，作底部光晕）。
            if (s.drawContactGlow)
            {
                FillEllipse(dbg, glm::vec2(center.x, bottomY + avgR * 0.05f),
                            avgR * 1.15f, avgR * 0.22f, PackColorABGR(s.glowColor, 1.0f), 20);
            }

            // 2. 内部 rings×segments 逐面着色。
            const int R = std::max(2, s.rings);
            for (int i = 0; i < m; ++i)
            {
                const glm::vec2 da  = ring[i] - center;
                const glm::vec2 db  = ring[(i + 1) % m] - center;
                const glm::vec2 dir = glm::normalize(0.5f * (da + db));
                for (int r = 0; r < R; ++r)
                {
                    const float         t0  = static_cast<float>(r) / static_cast<float>(R);
                    const float         t1  = static_cast<float>(r + 1) / static_cast<float>(R);
                    const float         tm  = 0.5f * (t0 + t1);
                    const glm::vec3     col = ShadeGel(dir, tm, s, L, H);
                    const std::uint32_t c   = PackColorABGR(col, 1.0f);
                    const glm::vec3     a0(center + da * t0, 0.0f);
                    const glm::vec3     a1(center + da * t1, 0.0f);
                    const glm::vec3     b0(center + db * t0, 0.0f);
                    const glm::vec3     b1(center + db * t1, 0.0f);
                    dbg->AddTriangle(a0, a1, b1, c);
                    dbg->AddTriangle(a0, b1, b0, c);
                }
            }

            // 3. 体内漂浮光斑（缓慢上浮 + 明灭）。
            if (s.drawSpecks)
            {
                for (int k = 0; k < kSpeckCount; ++k)
                {
                    const float     ph  = static_cast<float>(k) * 1.7f;
                    const glm::vec2 off = kSpeckLocal[k] * avgR * 0.9f +
                                          glm::vec2(0.0f, std::sin(t * 0.6f + ph) * avgR * 0.03f);
                    const float tw = 0.6f + 0.4f * std::sin(t * 1.3f + ph); // 明灭
                    FillEllipse(dbg, center + off, avgR * 0.045f, avgR * 0.06f,
                                PackColorABGR(s.speckColor * tw, 1.0f), 8);
                }
            }

            // 4. 发光眼（最上层；轻微呼吸缩放 → bloom 强发光）。
            if (s.drawEyes)
            {
                const float         pulse = 1.0f + 0.05f * std::sin(t * 2.5f);
                const float         rx    = s.eyeSize * 0.45f * avgR;
                const float         ry    = s.eyeSize * pulse * avgR;
                const glm::vec2     up    = glm::vec2(0.0f, avgR * 0.30f);
                const std::uint32_t eyeC  = PackColorABGR(s.eyeColor, 1.0f);
                FillEllipse(dbg, center + up + glm::vec2(-avgR * 0.26f, 0.0f), rx, ry, eyeC, 14);
                FillEllipse(dbg, center + up + glm::vec2(avgR * 0.26f, 0.0f), rx, ry, eyeC, 14);
            }
        }

    } // namespace

    // ===========================================================================
    // 生命周期
    // ===========================================================================

    SlimeGameModule::SlimeGameModule()
    {
        BuildActionMap();
        mVxHistory.fill(0.0f);
    }

    void SlimeGameModule::RegisterRenderPasses(Render::Pipeline& pipeline)
    {
        // InsertPass(AfterMainPass)：主 pass + 粒子写完 HDR、bloom 未跑的 hook。
        // 裸指针按 pipeline 记账供 Tick 喂数据（所有权移交 Pipeline，Edit 态常驻）。
        // 编辑器双视口对 Scene / Game 两条 pipeline 各调一次本函数。
        auto pass = std::make_unique<SlimeMetaballPass>();
        mSdfPasses.push_back({&pipeline, pass.get()});
        pipeline.InsertPass(Render::PipelineStage::AfterMainPass, std::move(pass));
    }

    void SlimeGameModule::UnregisterRenderPasses(Render::Pipeline& pipeline)
    {
        // RegisterRenderPasses 的对偶。DLL 热重载 FreeLibrary 前必须调——SlimeMetaballPass
        // 的 vtable/代码在 slime.dll 内，卸载后 Pipeline 持悬垂 pass 会在 Execute/析构崩。
        // AfterMainPass 上只有本模块 SDF pass（内置 post 走独立机制），清整 stage 安全。
        pipeline.RemovePassesAt(Render::PipelineStage::AfterMainPass);
        std::erase_if(mSdfPasses,
                      [&pipeline](const SdfPassEntry& e) { return e.pPipeline == &pipeline; });
    }

    void SlimeGameModule::OnEnterPlay(Game::GameModuleContext& ctx)
    {
        mpPhysics  = ctx.pPhysics;
        mpPipeline = ctx.pPipeline;

        // 手感组件化：Edit 世界里挂的 SlimeTuningComponent 覆盖内置默认值。必须在建
        // control point / Blob.Reset 之前——blobRadius 要在 Reset 时进 rest 几何、
        // SDF 渲染半径也读 mBlobParams.blobRadius，才真正改变史莱姆大小。
        ApplyTuningOverrides(ctx.pWorld);

        // 无物理世界（headless / 宿主未装配）→ graceful 退化，不建 body。
        if (!mpPhysics)
        {
            return;
        }

        // —— 关卡静态几何 ——
        mLevel = MakeLevel();
        mLevelBodies.clear();
        mLevelBodies.reserve(mLevel.size());
        for (const auto& b : mLevel)
        {
            const glm::vec2          center = 0.5f * (b.min + b.max);
            const glm::vec2          half   = 0.5f * (b.max - b.min);
            Phys::RigidBodyComponent rb;
            rb.type            = Phys::BodyType::Static;
            rb.initialPosition = center;
            Phys::ColliderComponent col;
            col.shape        = Phys::BoxDesc{half, {0.0f, 0.0f}};
            col.friction     = 0.0f;
            col.categoryBits = b.isGround ? kCatGround : kCatWall;
            col.maskBits     = 0xFFFFFFFFu;
            const Phys::BodyHandle h = mpPhysics->AddBody(rb, col);
            if (h.IsValid())
            {
                mLevelBodies.push_back(h);
            }
        }

        // —— control point：dynamic 刚体（velocity 驱动 + 自管重力）——
        // gravityScale=0 使其恒不受宿主世界重力影响（重力由手感逻辑自加）。故本模块
        // 不改宿主 PhysicsWorld 的 gravity：即便宿主建的是带重力的世界，control point
        // 免疫（gravityScale=0）、静态关卡免疫（static），行为与原 spike 的零重力世界一致。
        {
            Phys::RigidBodyComponent rb;
            rb.type            = Phys::BodyType::Dynamic;
            rb.initialPosition = kSpawn;
            rb.fixedRotation   = true;
            rb.gravityScale    = 0.0f;
            Phys::ColliderComponent col;
            col.shape        = Phys::BoxDesc{{mParams.cpHalfWidth, mParams.cpHalfHeight}, {0.0f, 0.0f}};
            col.density      = 1.0f;
            col.friction     = 0.0f;
            col.restitution  = 0.0f;
            col.categoryBits = kCatPlayer;
            col.maskBits     = 0xFFFFFFFFu;
            mCp              = mpPhysics->AddBody(rb, col);
        }

        // —— 运行态复位（多次 Enter/Stop 之间干净起步）——
        mVel             = {0.0f, 0.0f};
        mGrounded        = false;
        mWasGrounded     = false;
        mCoyoteTimer     = 0.0f;
        mJumpBufferTimer = 0.0f;
        mWallDir = mLastWallDir = mWallJumpAwayDir = 0;
        mSliding = mGrabbing = false;
        mWallCoyoteTimer = mWallJumpLockTimer = mWallJumpFlash = 0.0f;
        mStamina         = mParams.wallStamina;
        mLastHalfW = mLastHalfH = -1.0f; // 触发首帧 SyncColliderSize
        mAccumulator     = 0.0f;
        mStepsThisFrame  = 0;
        mRenderTime      = 0.0f;
        mCpState         = {};
        mDroplets.clear();
        mJuiceMode       = JuiceMode::None;
        mJuiceTimer      = 0.0f;
        mLandings.clear();
        mVxHistory.fill(0.0f);
        mVxHead          = 0;
        mInputFlashTimer = 0.0f;
        mBlobCentroid    = glm::vec2(0.0f);
        mDivergence = mApexDivergence = mPeakDivergence = mPrevVy = 0.0f;
        mMotionState        = SlimeMotionState::Idle;
        mLandingTimer       = 0.0f;
        mLaunchTimer        = 0.0f;
        mDeformPrevGrounded = false;
        mDeformPrevVy       = 0.0f;
        mCurSx              = 1.15f;
        mCurSy              = 0.80f;
        mCurStiff           = 200.0f;
        mAutoDemo           = false;
        mDemoPhase          = DemoPhase::Stand;
        mDemoTimer          = 0.0f;
        mDemoJumpFired      = false;

        // blob 复位到出生点静止起步（避免弹簧从远处猛甩）。
        mBlob.Reset(kSpawn, mBlobParams.blobRadius);

        // SDF pass（若已注册）Play 期启用——每个视口 pipeline 一份，全部启用。
        for (auto& entry : mSdfPasses)
        {
            entry.pPass->SetEnabled(true);
        }

        // Play 期 2D 游戏相机：正交机位对齐 standalone showcase framing（halfH 1.9
        // @16:9，沿 -Z 正视 XY 平面）。运行态实体——编辑器 Stop 快照还原回收、
        // Edit 世界保持无相机（Scene 视口回编辑器 3D 轨道相机）。
        if (ctx.pWorld != nullptr)
        {
            mCamFocus  = kSpawn;
            mCamEntity = ctx.pWorld->CreateEntity();
            ctx.pWorld->AddComponent<Scene::TransformComponent>(mCamEntity,
                                                                Scene::TransformComponent{});
            ctx.pWorld->AddComponent<Scene::NameComponent>(mCamEntity,
                                                           Scene::NameComponent{"GameCamera (Play)"});
            const float halfH = 1.9f;
            const float halfW = halfH * (16.0f / 9.0f);
            auto cam = Render::Camera::Orthographic(-halfW, halfW, -halfH, halfH, 0.1f, 100.0f);
            const glm::vec3 focus(kSpawn, 0.0f);
            cam.view = glm::lookAt(focus + glm::vec3(0.0f, 0.0f, 10.0f), focus,
                                   glm::vec3(0.0f, 1.0f, 0.0f));
            ctx.pWorld->AddComponent<Render::Camera>(mCamEntity, cam);
        }

        // （原"Play 期暗场背景"硬编码已删：模块不再劫持 sky / 清屏色，背景交给
        // 场景的 EnvironmentComponent 决定——用户拍板天空盒随场景进 Play。暗场
        // 需求由"场景不挂环境组件"自然表达。）
    }

    void SlimeGameModule::Tick(Game::GameModuleContext& ctx, float dt)
    {
        if (!mpPhysics)
        {
            return;
        }

        dt = std::clamp(dt, 0.0f, kMaxFrameDt);

        // 输入源：真实键盘 / 自动演示脚本（后者注入虚拟 action，忽略键盘）。
        if (mAutoDemo)
        {
            UpdateDemoScript(dt);
        }
        else
        {
            SampleInput();
        }

        mRenderTime += dt; // gel 光斑漂移 / 眼呼吸 / 高光脉动（视觉时钟，与物理步无关）

        // D1 形变：派生状态 → 查表 → 平滑 → 喂 blob（本帧所有固定子步共用同一形状）。
        UpdateDeform(dt);

        mAccumulator += dt;
        mStepsThisFrame = 0;
        while (mAccumulator >= kFixedDt)
        {
            FixedStep(kFixedDt);
            mAccumulator -= kFixedDt;
            ++mStepsThisFrame;
        }

        // 拉取 control point 最新状态供 overlay / Loop B。
        const auto bxf    = mpPhysics->GetBodyTransform(mCp);
        const auto bvel   = mpPhysics->GetLinearVelocity(mCp);
        mCpState.position = bxf.position;
        mCpState.velocity = bvel;
        mCpState.grounded = mGrounded;

        mVxHistory[mVxHead] = bvel.x;
        mVxHead             = (mVxHead + 1) % static_cast<int>(mVxHistory.size());

        // apex 分叉测量（spec 核心产出 = "果冻预算"）。
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

        // 把最新 blob 软体喂给全部 SDF pass 实例（perimeter 世界坐标 + centroid + radius + 时钟）。
        if (!mSdfPasses.empty() && mBlob.Initialized())
        {
            const auto& bp = mBlob.Positions();
            for (auto& entry : mSdfPasses)
            {
                entry.pPass->SetBlob(bp.data(), mBlob.PerimeterCount(), mBlob.Centroid(),
                                     mBlobParams.blobRadius);
                entry.pPass->SetTime(mRenderTime);
            }
        }

        // juice：推进溅射粒子 + 分裂/合并脚本，喂 SDF pass 的 droplet 通道。
        UpdateDroplets(dt);

        // 2D 游戏相机跟随（blob 质心为焦点）+ 视觉参数实时重读（PlaySafe 字段）。
        UpdateGameCamera(ctx.pWorld, dt);
        ApplyVisualTuning(ctx.pWorld);

        DrawDebug();
        // 宿主负责 Render —— 本模块不调 Pipeline::Render。

        mInput.BeginFrame(); // 帧末推进输入状态机：Pressed→Held、Released→Idle
    }

    void SlimeGameModule::OnEvent(const Platform::WindowEvent& event)
    {
        // 只取 key 喂 InputContext；resize 归宿主（不再转 Pipeline::OnResize）。
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
    }

    void SlimeGameModule::OnExitPlay(Game::GameModuleContext& ctx)
    {
        (void)ctx;
        TearDownBodies(); // 须在 mpPhysics 置空前

        // 清运行态（具体复位在下次 OnEnterPlay）。
        mDroplets.clear();
        mLandings.clear();
        mJuiceMode  = JuiceMode::None;
        mJuiceTimer = 0.0f;

        // SDF pass 常驻（GPU 资源不销毁），仅禁用避免 edit 态残留最后一帧史莱姆。
        for (auto& entry : mSdfPasses)
        {
            entry.pPass->SetEnabled(false);
        }

        // Play 期相机实体：编辑器路径快照还原会回收，但 runtime 宿主无快照——
        // 显式销毁保证两宿主一致（编辑器下重复销毁无害，快照还原先行即不 valid）。
        if (mCamEntity.IsValid() && ctx.pWorld != nullptr)
        {
            ctx.pWorld->DestroyEntity(mCamEntity);
        }
        mCamEntity = {};

        mpPhysics  = nullptr;
        mpPipeline = nullptr;
    }

    void SlimeGameModule::TearDownBodies()
    {
        if (!mpPhysics)
        {
            return;
        }
        mpPhysics->RemoveBody(mCp); // idempotent：无效句柄 no-op
        mCp = {};
        for (const auto& h : mLevelBodies)
        {
            mpPhysics->RemoveBody(h);
        }
        mLevelBodies.clear();
    }

    std::span<const Scene::ComponentSerializerEntry> SlimeGameModule::ComponentSerializers() const
    {
        // 单元素静态表：GetSlimeTuningSerializerEntry() 返回 program 生命周期 static，
        // 拷进本表后 span 视图活到进程退出（宿主 Save/Load 期只读）。追加游戏组件时扩表即可。
        static const std::array<Scene::ComponentSerializerEntry, 1> kEntries{
            GetSlimeTuningSerializerEntry(),
        };
        return kEntries;
    }

    void SlimeGameModule::UpdateGameCamera(World* pWorld, float dt)
    {
        if (pWorld == nullptr || !mCamEntity.IsValid())
        {
            return;
        }
        auto* cam = pWorld->GetComponent<Render::Camera>(mCamEntity);
        if (cam == nullptr)
        {
            return;
        }
        // 指数平滑跟随（帧率无关）。焦点取 blob 质心（软体视觉重心）而非 control
        // point——果冻滞后感由 blob 本身提供，相机不再叠加弹性。
        const glm::vec2 target = mBlob.Initialized() ? mBlobCentroid : mCpState.position;
        mCamFocus += (target - mCamFocus) * (1.0f - std::exp(-6.0f * dt));
        const glm::vec3 focus(mCamFocus, 0.0f);
        cam->view = glm::lookAt(focus + glm::vec3(0.0f, 0.0f, 10.0f), focus,
                                glm::vec3(0.0f, 1.0f, 0.0f));
    }

    void SlimeGameModule::ApplyVisualTuning(World* pWorld)
    {
        if (pWorld == nullptr || mSdfPasses.empty())
        {
            return;
        }
        auto view = pWorld->Registry().view<SlimeTuningComponent>();
        if (view.begin() == view.end())
        {
            return;
        }
        const auto& c = view.get<SlimeTuningComponent>(*view.begin());
        for (auto& entry : mSdfPasses)
        {
            auto& t      = entry.pPass->GetTunables();
            t.bodyColor  = c.bodyColor;
            t.deepColor  = c.deepColor;
            t.rimColor   = c.rimColor;
            t.eyeColor   = c.eyeColor;
            t.speckColor = c.speckColor;
            t.glowColor  = c.glowColor;
            t.domeScale  = c.domeScale;
            t.rimGain    = c.rimGain;
            t.specGain   = c.specGain;
            t.ambient    = c.ambient;
            t.eyeGain    = c.eyeGain;
            t.speckGain  = c.speckGain;
            t.glowGain   = c.glowGain;
            t.opacity    = c.opacity;
            t.coreGain   = c.coreGain;
        }
    }

    void SlimeGameModule::ApplyTuningOverrides(World* pWorld)
    {
        if (!pWorld)
        {
            return; // headless / 宿主未装配 World → 维持内置默认手感
        }
        // 只取第一个挂 SlimeTuningComponent 的实体（spike 单史莱姆，多实体取先遇者）。
        auto view = pWorld->Registry().view<SlimeTuningComponent>();
        for (const auto e : view)
        {
            const SlimeTuningComponent& c = view.get<SlimeTuningComponent>(e);
            mParams.jumpSpeed             = c.jumpSpeed;
            mParams.maxRunSpeed           = c.maxRunSpeed;
            mParams.riseGravity           = c.riseGravity;
            mParams.fallGravity           = c.fallGravity;
            mBlobParams.blobRadius        = c.blobRadius; // 进 Blob.Reset rest 几何 + SDF 半径
            break;
        }
    }

    // ===========================================================================
    // 输入
    // ===========================================================================

    void SlimeGameModule::BuildActionMap()
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
        add("grab", {In::KeyCode::LeftShift, In::KeyCode::RightShift});
        add("climb_up", {In::KeyCode::Up});
        add("climb_down", {In::KeyCode::Down});
        mInput.Push(std::move(map));
    }

    void SlimeGameModule::SampleInput()
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

        // 爬墙输入：抓墙 hold + 上下攀爬。
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

    // ===========================================================================
    // 自动演示（attract / 免手玩动态验证；本模块暂无 toggle，留待宿主接线）
    // ===========================================================================

    void SlimeGameModule::StartDemo()
    {
        mDemoPhase     = DemoPhase::Stand;
        mDemoTimer     = 0.0f;
        mDemoJumpFired = false;
        ResetControlPoint();
    }

    void SlimeGameModule::AdvanceDemoPhase(DemoPhase next)
    {
        mDemoPhase     = next;
        mDemoTimer     = 0.0f;
        mDemoJumpFired = false;
    }

    void SlimeGameModule::UpdateDemoScript(float dt)
    {
        // 每帧先清零虚拟输入（不注入抓墙 / 攀爬）。
        mMoveAxis      = 0.0f;
        mJumpHeld      = false;
        mGrabHeld      = false;
        mClimbUpHeld   = false;
        mClimbDownHeld = false;

        mDemoTimer += dt;
        const float cpx = mCpState.position.x; // 上帧快照，够做视野边界判断

        switch (mDemoPhase)
        {
            case DemoPhase::Stand:
                if (mDemoTimer >= 1.5f)
                {
                    AdvanceDemoPhase(DemoPhase::Jump);
                }
                break;

            case DemoPhase::Jump:
                mJumpHeld = true; // 全程按住 → 满跳
                if (!mDemoJumpFired)
                {
                    mJumpBufferTimer = mParams.jumpBufferTime; // 一次性触发起跳
                    mDemoJumpFired   = true;
                }
                if (mDemoTimer >= 3.0f)
                {
                    AdvanceDemoPhase(DemoPhase::RunRight);
                }
                break;

            case DemoPhase::RunRight:
                mMoveAxis = 1.0f;
                if (cpx > kSpawn.x + 2.5f || mDemoTimer >= 1.5f)
                {
                    AdvanceDemoPhase(DemoPhase::StopR);
                }
                break;

            case DemoPhase::StopR:
                if (mDemoTimer >= 0.6f)
                {
                    AdvanceDemoPhase(DemoPhase::RunLeft);
                }
                break;

            case DemoPhase::RunLeft:
                mMoveAxis = -1.0f;
                if (cpx < kSpawn.x - 2.0f || mDemoTimer >= 1.5f)
                {
                    AdvanceDemoPhase(DemoPhase::StopL);
                }
                break;

            case DemoPhase::StopL:
                if (mDemoTimer >= 0.6f)
                {
                    AdvanceDemoPhase(DemoPhase::Stand);
                }
                break;
        }
    }

    // ===========================================================================
    // 固定物理子步 + 探针
    // ===========================================================================

    void SlimeGameModule::FixedStep(float dt)
    {
        SyncColliderSize(); // cp 尺寸 slider 改动经 ReplaceFixture 生效（Step 外）

        glm::vec2 pos = mpPhysics->GetBodyTransform(mCp).position;

        const bool grounded = ProbeGrounded(pos);

        // coyote：在地面刷满，离地后随时间衰减。
        if (grounded)
        {
            mCoyoteTimer = mParams.coyoteTime;
        }
        else
        {
            mCoyoteTimer = std::max(0.0f, mCoyoteTimer - dt);
        }

        // 落点采样（判据 #2）：空中→落地跳变那一刻记录 x。
        if (grounded && !mWasGrounded && mVel.y <= 0.0f)
        {
            RecordLanding(pos);
        }
        mWasGrounded = grounded;
        mGrounded    = grounded;

        // 爬墙：侧向探墙 + coyote + 锁定计时 + 抓墙判定（在水平/跳跃/重力之前定状态）。
        // 窄缝双壁：优先取玩家正按向的那面墙，否则任一侧有墙就取。
        int wallDir = 0;
        if (mParams.wallEnabled && !grounded)
        {
            const bool wallR = ProbeWall(pos, +1);
            const bool wallL = ProbeWall(pos, -1);
            if (mMoveAxis > 0.0f && wallR)
                wallDir = +1;
            else if (mMoveAxis < 0.0f && wallL)
                wallDir = -1;
            else if (wallR)
                wallDir = +1;
            else if (wallL)
                wallDir = -1;
        }
        mWallDir = wallDir;
        if (wallDir != 0)
        {
            mLastWallDir     = wallDir;
            mWallCoyoteTimer = mParams.wallCoyoteTime;
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

        // 水平：朝目标速度加/减速（空中控制减弱）。抓墙不横向漂；墙跳锁定期禁"回墙"。
        float moveAxis = mMoveAxis;
        if (mWallJumpLockTimer > 0.0f && mWallJumpAwayDir != 0 && moveAxis != 0.0f &&
            (moveAxis > 0.0f) != (mWallJumpAwayDir > 0))
        {
            moveAxis = 0.0f;
        }
        const float target = (grabbing ? 0.0f : moveAxis * mParams.maxRunSpeed);
        const float accel  = grounded ? mParams.groundAccel : mParams.airAccel;
        const float decel  = grounded ? mParams.groundDecel : mParams.airDecel;
        mVel.x             = Approach(mVel.x, target, accel, decel, dt);

        // 贴地：站立时不让向下速度累积（保持接触、便于探测）。
        if (grounded && mVel.y <= 0.0f)
        {
            mVel.y = 0.0f;
        }

        // 跳跃：接地跳（buffer + coyote）优先；否则墙跳（buffer + 墙 coyote，非接地）。
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
            const int away     = (wallDir != 0) ? -wallDir : -mLastWallDir;
            mVel.y             = mParams.wallJumpSpeedY;
            mVel.x             = static_cast<float>(away) * mParams.wallJumpSpeedX;
            mWallJumpAwayDir   = away;
            mWallJumpLockTimer = mParams.wallJumpLockTime;
            mWallJumpFlash     = 0.15f;
            mJumpBufferTimer   = 0.0f;
            mWallCoyoteTimer   = 0.0f;
            airborne           = true;
            grabbing           = false; // 墙跳抵消本步抓墙（否则重力段会把 vy 覆盖回攀爬速度）
        }
        if (mJumpBufferTimer > 0.0f)
        {
            mJumpBufferTimer = std::max(0.0f, mJumpBufferTimer - dt);
        }

        // 抓墙：重力挂起，竖直由 ↑/↓ 攀爬输入控制。
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
        // 重力：上升 / 下落分档 + apex 衰减 + 松手切断上升（可变跳跃高度）。
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
                    g = mParams.riseGravity * (mJumpHeld ? 1.0f : mParams.lowJumpMult);
                }
            }
            else
            {
                g = (std::abs(mVel.y) < mParams.apexThreshold)
                        ? mParams.fallGravity * mParams.apexGravityMult
                        : mParams.fallGravity;
            }
            mVel.y -= g * dt;
            mVel.y = std::max(mVel.y, -mParams.maxFallSpeed);

            // 贴墙下滑：按住朝墙 + 下落 → 限速（比自由落体慢 = "抓住墙"）。
            const bool pressingIntoWall = wallDir != 0 && mMoveAxis != 0.0f &&
                                          (mMoveAxis > 0.0f) == (wallDir > 0);
            if (mParams.wallEnabled && pressingIntoWall && mVel.y < 0.0f)
            {
                mVel.y   = std::max(mVel.y, -mParams.wallSlideSpeed);
                mSliding = true;
            }
        }

        // 提交速度 + 步进物理（Box2D 解碰撞：撞墙/天花板修正速度）。
        const float intendedVy = mVel.y;
        mpPhysics->SetLinearVelocity(mCp, mVel);
        mpPhysics->Step(dt);

        // 回读：让自管积分器与 Box2D 解出的碰撞结果对齐。
        glm::vec2 postVel = mpPhysics->GetLinearVelocity(mCp);
        glm::vec2 postPos = mpPhysics->GetBodyTransform(mCp).position;

        // corner correction：上升中顶到角落（vy 被打没）时小幅水平推移恢复上升。
        if (mParams.cornerCorrectionDist > 0.0f && intendedVy > 0.1f &&
            postVel.y <= 0.05f && !ProbeGrounded(postPos))
        {
            if (TryCornerCorrect(postPos, intendedVy))
            {
                postVel = mpPhysics->GetLinearVelocity(mCp);
                postPos = mpPhysics->GetBodyTransform(mCp).position;
            }
        }

        mVel = postVel;

        // Loop B：同一固定子步内推进软体 blob（用本子步已解算的 cp 位置当弹簧 target）。
        // 碰撞几何直接用单一 mLevel（Blob::Step 只读其 min/max）。
        mBlob.Step(dt, postPos, mLevel, mBlobParams);
    }

    bool SlimeGameModule::ProbeGrounded(glm::vec2 pos) const
    {
        if (mVel.y > 0.05f)
        {
            return false; // 正在上升不算接地
        }
        const float     feetY      = pos.y - mParams.cpHalfHeight;
        constexpr float kLandEps   = 0.06f;
        constexpr float kProbeDist = 0.10f;
        const float     originY    = feetY + kLandEps;
        const float     maxDist    = kLandEps + kProbeDist;
        // categoryBits=all，maskBits=kSolidMask（只命中关卡实心几何、自动排除自身）。
        const Phys::QueryFilter filter{0xFFFFFFFFu, kSolidMask};
        for (const float sx : {pos.x - mParams.cpHalfWidth, pos.x, pos.x + mParams.cpHalfWidth})
        {
            const Phys::RaycastHit hit =
                mpPhysics->RaycastClosest({sx, originY}, {0.0f, -1.0f}, maxDist, filter);
            if (hit.hit)
            {
                return true;
            }
        }
        return false;
    }

    bool SlimeGameModule::HeadClearAt(float x, float y) const
    {
        const float                         headLo = y + mParams.cpHalfHeight - 0.05f;
        const float                         headHi = y + mParams.cpHalfHeight + 0.05f;
        const Phys::QueryFilter             filter{0xFFFFFFFFu, kCatWall};
        const std::vector<Phys::BodyHandle> hits = mpPhysics->OverlapAABB(
            {x - mParams.cpHalfWidth, headLo}, {x + mParams.cpHalfWidth, headHi}, filter);
        return hits.empty(); // 无实心几何交叠 = 头部让开
    }

    bool SlimeGameModule::ProbeWall(glm::vec2 pos, int dir) const
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
                mpPhysics->RaycastClosest({pos.x, sy}, rayDir, maxDist, filter);
            if (hit.hit)
            {
                return true;
            }
        }
        return false;
    }

    bool SlimeGameModule::TryCornerCorrect(glm::vec2 pos, float restoreVy)
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
                    mpPhysics->SetBodyTransform(mCp, t);
                    mVel.y = restoreVy;
                    mpPhysics->SetLinearVelocity(mCp, mVel);
                    return true;
                }
            }
        }
        return false;
    }

    void SlimeGameModule::SyncColliderSize()
    {
        if (mParams.cpHalfWidth == mLastHalfW && mParams.cpHalfHeight == mLastHalfH)
        {
            return;
        }
        Phys::ColliderComponent col;
        col.shape        = Phys::BoxDesc{{mParams.cpHalfWidth, mParams.cpHalfHeight}, {0.0f, 0.0f}};
        col.density      = 1.0f;
        col.friction     = 0.0f;
        col.restitution  = 0.0f;
        col.categoryBits = kCatPlayer;  // ReplaceFixture 会重置过滤位，须显式重设
        col.maskBits     = 0xFFFFFFFFu;
        mpPhysics->ReplaceFixture(mCp, col);
        mLastHalfW = mParams.cpHalfWidth;
        mLastHalfH = mParams.cpHalfHeight;
    }

    void SlimeGameModule::RecordLanding(glm::vec2 pos)
    {
        const float feetY = pos.y - mParams.cpHalfHeight;
        mLandings.push_back({pos.x, feetY});
        if (mLandings.size() > 10)
        {
            mLandings.erase(mLandings.begin());
        }
    }

    void SlimeGameModule::ResetControlPoint()
    {
        Phys::BodyTransform t{};
        t.position = kSpawn;
        mpPhysics->SetBodyTransform(mCp, t);
        mpPhysics->SetLinearVelocity(mCp, {0.0f, 0.0f});
        mVel             = {0.0f, 0.0f};
        mCoyoteTimer     = 0.0f;
        mJumpBufferTimer = 0.0f;

        mBlob.Reset(kSpawn, mBlobParams.blobRadius);
        mApexDivergence = 0.0f;
        mPeakDivergence = 0.0f;
        mPrevVy         = 0.0f;
    }

    // ===========================================================================
    // D1 形变驱动
    // ===========================================================================

    SlimeMotionState SlimeGameModule::DeriveMotionState(float dt)
    {
        const bool  grounded = mGrounded;
        const float vy       = mCpState.velocity.y;
        const float vx       = mCpState.velocity.x;

        if (mLandingTimer > 0.0f)
        {
            mLandingTimer = std::max(0.0f, mLandingTimer - dt);
        }
        if (mLaunchTimer > 0.0f)
        {
            mLaunchTimer = std::max(0.0f, mLaunchTimer - dt);
        }

        // 着地沿 + 落地冲击够大 → 触发 Landing 一次性计时 + 溅射水滴。
        if (grounded && !mDeformPrevGrounded && mDeformPrevVy < -kLandingImpactThresh)
        {
            mLandingTimer = kLandingDuration;
            SpawnSplatter();
        }
        // 起跳沿（离地且上升）→ 触发 Launch 极短窗口。
        if (!grounded && mDeformPrevGrounded && vy > 0.0f)
        {
            mLaunchTimer = kLaunchDuration;
        }
        mDeformPrevGrounded = grounded;
        mDeformPrevVy       = vy;

        if (mLandingTimer > 0.0f)
        {
            return SlimeMotionState::Landing;
        }
        if (mLaunchTimer > 0.0f)
        {
            return SlimeMotionState::Launch;
        }
        if (!grounded)
        {
            return (vy > 0.0f) ? SlimeMotionState::Rising : SlimeMotionState::Falling;
        }
        if (std::abs(vx) > kSlideVxThresh)
        {
            return SlimeMotionState::Sliding;
        }
        return SlimeMotionState::Idle;
    }

    void SlimeGameModule::UpdateDeform(float dt)
    {
        // 总是派生状态（DeriveMotionState 内含落地溅射检测 + 计时，不可跳），但强制姿态
        // 置位时用 mForcedState 取代它做形变查表（showcase 摆姿）。
        const SlimeMotionState derived = DeriveMotionState(dt);
        mMotionState                   = mHasForcedState ? mForcedState : derived;
        if (!mDeformEnabled)
        {
            mBlob.SetTargetShape(glm::mat2(1.0f)); // 圆（退回手动 stiffness）
            return;
        }
        const MotionShape tgt = MotionShapeTarget(mMotionState);

        // 指数平滑：状态切换不突变。dt=0 时 a=0（不动），安全。
        const float a = 1.0f - std::exp(-dt / std::max(1e-4f, mDeformTau));
        mCurSx += (tgt.sx - mCurSx) * a;
        mCurSy += (tgt.sy - mCurSy) * a;
        mCurStiff += (tgt.stiffness - mCurStiff) * a;

        // Rolling：在各向异性 diag 上叠加翻滚旋转；其余态旋角归零。
        glm::mat2 shape(mCurSx, 0.0f, 0.0f, mCurSy); // 列主序 diag(sx,sy)
        if (mMotionState == SlimeMotionState::Rolling)
        {
            mRollAngle += dt * mRollSpeed;
            const float c = std::cos(mRollAngle);
            const float s = std::sin(mRollAngle);
            shape = glm::mat2(c, s, -s, c) * shape; // R(angle) · diag
        }
        else
        {
            mRollAngle = 0.0f;
        }
        mBlob.SetTargetShape(shape);
        mBlobParams.stiffness = mCurStiff;
    }

    // ===========================================================================
    // juice：落地溅射 / 分裂 / 合并
    // ===========================================================================

    void SlimeGameModule::SpawnSplatter()
    {
        const glm::vec2 base = mCpState.position +
                               glm::vec2(0.0f, -mBlobParams.blobRadius * 0.5f);
        constexpr int n = 5;
        for (int i = 0; i < n; ++i)
        {
            // 上半放射（0.15π..0.85π），速度按 index 变化（确定性无 rand）。
            const float ang   = 3.14159265f *
                                (0.15f + 0.7f * static_cast<float>(i) / static_cast<float>(n - 1));
            const float speed = 2.6f + 0.5f * static_cast<float>(i % 3);
            SlimeDroplet d;
            d.pos        = base;
            d.vel        = glm::vec2(std::cos(ang), std::sin(ang) + 0.4f) * speed;
            d.baseRadius = mBlobParams.blobRadius * 0.20f;
            d.radius     = d.baseRadius;
            d.maxLife    = 0.4f;
            d.life       = d.maxLife;
            mDroplets.push_back(d);
        }
    }

    void SlimeGameModule::TriggerSplit()
    {
        mJuiceMode  = JuiceMode::Split;
        mJuiceTimer = 0.0f;
    }

    void SlimeGameModule::TriggerMerge()
    {
        mJuiceMode  = JuiceMode::Merge;
        mJuiceTimer = 0.0f;
    }

    void SlimeGameModule::UpdateDroplets(float dt)
    {
        constexpr float kDropGravity = 14.0f;
        for (auto& d : mDroplets)
        {
            d.life -= dt;
            d.vel.y -= kDropGravity * dt;
            d.pos += d.vel * dt;
            d.radius = d.baseRadius * std::max(0.0f, d.life / d.maxLife); // 随生命缩小
        }
        mDroplets.erase(std::remove_if(mDroplets.begin(), mDroplets.end(),
                                       [](const SlimeDroplet& d)
                                       { return d.life <= 0.0f; }),
                        mDroplets.end());

        std::vector<glm::vec2> pos;
        std::vector<float>     rad;
        for (const auto& d : mDroplets)
        {
            pos.push_back(d.pos);
            rad.push_back(d.radius);
        }
        UpdateJuiceScript(dt, pos, rad); // 追加分裂/合并脚本 droplet

        const int cap = SlimeMetaballPass::kMaxDroplets;
        if (static_cast<int>(pos.size()) > cap)
        {
            pos.resize(cap);
            rad.resize(cap);
        }
        for (auto& entry : mSdfPasses)
        {
            entry.pPass->SetDroplets(pos.empty() ? nullptr : pos.data(),
                                     rad.empty() ? nullptr : rad.data(),
                                     static_cast<int>(pos.size()));
        }
    }

    void SlimeGameModule::UpdateJuiceScript(float dt, std::vector<glm::vec2>& pos,
                                            std::vector<float>& rad)
    {
        if (mJuiceMode == JuiceMode::None)
        {
            return;
        }
        constexpr float kDur = 1.3f;
        mJuiceTimer += dt;
        const float t = std::clamp(mJuiceTimer / kDur, 0.0f, 1.0f);

        const glm::vec2 c   = mBlobCentroid; // 主体质心（世界）
        const float     R   = mBlobParams.blobRadius;
        const float     dir = 1.0f;          // 向右进出

        // 大 droplet：分裂 t:0→1 飞出；合并 t:0→1 靠回。
        const float     bigT   = (mJuiceMode == JuiceMode::Split) ? t : (1.0f - t);
        const glm::vec2 bigPos = c + glm::vec2(dir * R * 2.2f * bigT, R * 0.2f);
        pos.push_back(bigPos);
        rad.push_back(R * 0.6f);

        // 细颈：分裂随 t 变细并断，合并随 t 变粗融。
        const float neck = (mJuiceMode == JuiceMode::Split) ? (1.0f - t) : t;
        if (neck > 0.05f)
        {
            for (int k = 1; k <= 2; ++k)
            {
                const float f = static_cast<float>(k) / 3.0f; // 0.33 / 0.67 沿连线
                pos.push_back(c + (bigPos - c) * f);
                rad.push_back(R * 0.22f * neck);
            }
        }

        if (mJuiceTimer >= kDur)
        {
            mJuiceMode  = JuiceMode::None;
            mJuiceTimer = 0.0f;
        }
    }

    // ===========================================================================
    // overlay：场景碰撞盒 + control point 标记 + 速度 + 输入标记 + 落点 + blob gel
    // ===========================================================================

    void SlimeGameModule::DrawDebug()
    {
        if (!mpPipeline)
        {
            return;
        }
        auto* dbg = mpPipeline->GetDebugDrawScene();
        if (!dbg)
        {
            return;
        }

        // beauty 展示模式：隐藏关卡线框 + 所有 gameplay overlay，只留史莱姆(黑底)。
        if (!mSlime.beauty)
        {
            for (const auto& b : mLevel)
            {
                dbg->AddAabb(glm::vec3(b.min, 0.0f), glm::vec3(b.max, 0.0f), b.color);
            }

            const glm::vec2 p = mCpState.position;
            const glm::vec3 c3{p, 0.0f};

            // control point 小框（gameplay 真相层），可经 slider 关。
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

            // 爬墙 overlay：贴墙侧竖线（grey=触墙 / cyan=下滑 / yellow=抓墙）。
            if (mWallDir != 0)
            {
                const float         wx = p.x + static_cast<float>(mWallDir) * mParams.cpHalfWidth;
                const std::uint32_t wc = mGrabbing ? kYellow : (mSliding ? kCyan : kGrey);
                dbg->AddLine(glm::vec3(wx, p.y - mParams.cpHalfHeight, 0.0f),
                             glm::vec3(wx, p.y + mParams.cpHalfHeight, 0.0f), wc);
            }
            // 墙跳闪：从 cp 指向离墙方向的短线。
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

            // 输入瞬间标记（判据 #1）：刚按下时在 cp 上方闪白三角 + 竖线。
            if (mInputFlashTimer > 0.0f)
            {
                const float y0 = p.y + mParams.cpHalfHeight + 0.15f;
                dbg->AddTriangle(glm::vec3(p.x - 0.15f, y0, 0.0f),
                                 glm::vec3(p.x + 0.15f, y0, 0.0f),
                                 glm::vec3(p.x, y0 + 0.3f, 0.0f), kWhite);
                dbg->AddLine(glm::vec3(p.x, p.y, 0.0f), glm::vec3(p.x, y0, 0.0f), kWhite);
            }

            // 落点散布打点（判据 #2）：每次落地 x 处一根橙色短竖线。
            for (const auto& lp : mLandings)
            {
                dbg->AddLine(glm::vec3(lp.x, lp.y, 0.0f),
                             glm::vec3(lp.x, lp.y + 0.35f, 0.0f), kAmber);
            }
        }

        // Loop B：软体 blob（主可见物 = 史莱姆 gel 渲染）。
        if (mBlob.Initialized())
        {
            const auto& bp = mBlob.Positions();
            const int   n  = mBlob.PerimeterCount();

            // Tier1 CPU gel：SDF pass 开启时接管渲染、跳过此路径（关掉 SDF 恢复）。
            const bool sdfActive =
                !mSdfPasses.empty() && mSdfPasses.front().pPass->IsEnabled();
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

            // apex 分叉 overlay（spec 核心产出）：cp ↔ blob 视觉质心连线 + 质心十字。beauty 隐藏。
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

} // namespace spike01
