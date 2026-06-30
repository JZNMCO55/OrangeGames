// spike-01-blob —— 流体史莱姆 context-sensitive 手感 Spike 1 · Loop A
//
// 设计规格：../spike-01-blob-feel.md（执行规格）+ ../phase0-pillars.md（宪法）。
//
// 本文件实现 spike 的 **Loop A —— gameplay 真相层**：一个隐藏的 control point
// （Box2D dynamic 刚体，用 debug draw 画成小框）承载落点 / 碰撞 / 输入响应，
// 调成标准平台跳跃手感。**本里程碑不做软体 blob**（Loop B 的事）；control point
// 只用 debug draw 标记，不碰 metaball / 折射 / 任何好看渲染（spec 硬纪律：feel 在前）。
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

#include <orange/engine/app/AppConfig.h>
#include <orange/engine/app/AppHost.h>
#include <orange/engine/app/FrameContext.h>
#include <orange/engine/app/Layer.h>
#include <orange/engine/asset/AssetRegistry.h>
#include <orange/engine/asset/ShaderAsset.h>
#include <orange/engine/asset/ShaderLoader.h>
#include <orange/engine/input/Action.h>
#include <orange/engine/input/ActionMap.h>
#include <orange/engine/input/InputContext.h>
#include <orange/engine/input/InputDevice.h>
#include <orange/engine/physics/BodyHandle.h>
#include <orange/engine/physics/ColliderComponent.h>
#include <orange/engine/physics/ColliderDesc.h>
#include <orange/engine/physics/PhysicsWorld.h>
#include <orange/engine/physics/RigidBodyComponent.h>
#include <orange/engine/platform/WindowEvent.h>
#include <orange/engine/render/Camera.h>
#include <orange/engine/render/DebugDrawScene.h>
#include <orange/engine/render/MaterialSystem.h>
#include <orange/engine/render/Pipeline.h>
#include <orange/engine/scene/Entity.h>
#include <orange/engine/scene/World.h>

// 消费者自行 #include <imgui.h>（引擎把 imgui include 目录 PUBLIC 暴露，
// 公共头本身零 imgui 类型；GAP-2026-05-27-consumer-imgui-tuning-hook 已补完）。
#include <imgui.h>

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
namespace In   = Orange::Engine::Input;
namespace Phys = Orange::Engine::Physics;

namespace
{

// debug draw 颜色：ABGR packed（低 8 位 R，高 8 位 A，0xAA_BB_GG_RR）。
constexpr std::uint32_t kGreen   = 0xFF00FF00u;  // 平地
constexpr std::uint32_t kCyan    = 0xFFFFFF00u;  // 平台
constexpr std::uint32_t kRed     = 0xFF0000FFu;  // 窄缝两壁
constexpr std::uint32_t kAmber   = 0xFF1A78FFu;  // 落点打点
constexpr std::uint32_t kYellow  = 0xFF00FFFFu;  // control point 标记
constexpr std::uint32_t kMagenta = 0xFFFF00FFu;  // 速度向量
constexpr std::uint32_t kWhite   = 0xFFFFFFFFu;  // 输入瞬间标记
constexpr std::uint32_t kGrey    = 0xFF808080u;  // 接地指示

// 固定物理步长：60Hz（spec 地基 #2，与渲染帧率解耦）。
constexpr float kFixedDt = 1.0f / 60.0f;
// 单帧累加器上限：避免窗口切回前台 / 断点后的"长帧"一次推进过多子步。
constexpr float kMaxFrameDt = 0.10f;

// control point 出生点（左侧地面上方一点，落下后稳定在平地上）。
constexpr glm::vec2 kSpawn{-5.0f, -2.5f};

// ---------------------------------------------------------------------------
// 手感旋钮（全部走 ImGui slider，不 hardcode 数值）。
// baseline 抄 Celeste 经典起点（coyote ~5 帧 / buffer ~6 帧 / 跑速 ~9 u/s），
// 供用户第二天真机微调；详见 spec "循环 A 调参顺序" + "别从零发明手感"。
// ---------------------------------------------------------------------------
struct FeelParams
{
    // —— 地面移动（先让走/跑跟手，判据 #1 延迟在这关）——
    float maxRunSpeed = 9.0f;    // 最大跑速 u/s（Celeste ~9 量级）
    float groundAccel = 90.0f;   // 地面加速度 u/s^2（~0.1s 到顶速）
    float groundDecel = 120.0f;  // 地面减速度 u/s^2（停得比加速快 → 更跟手）

    // —— 空中控制（单独减弱，空中没地面那么灵）——
    float airAccel = 60.0f;      // 空中加速度 u/s^2
    float airDecel = 60.0f;      // 空中减速度 u/s^2

    // —— 跳跃（依赖链：先调这组再调 juice）——
    float jumpSpeed      = 16.0f;  // 起跳初速度 u/s
    float riseGravity    = 45.0f;  // 上升段重力 u/s^2
    float fallGravity    = 70.0f;  // 下落段重力（比上升大 → 下落更跟手）
    float apexThreshold  = 3.0f;   // |vy| 低于此值视为 apex（顶点）
    float apexGravityMult = 0.55f; // apex 段重力衰减系数（顶点悬停手感）
    float lowJumpMult    = 2.5f;   // 上升中松手 → 重力放大倍率（可变跳跃高度）
    float maxFallSpeed   = 25.0f;  // 终端下落速度上限 u/s

    // —— juice 辅助（建立在跳跃调好之上）——
    float coyoteTime          = 0.08f; // 离开平台后仍可起跳的宽限 s（~5 帧）
    float jumpBufferTime      = 0.10f; // 落地前预输入跳跃的缓冲 s（~6 帧）
    float cornerCorrectionDist = 0.25f; // 顶到角落时的最大水平推移 u（0 = 关）

    // —— control point 尺寸（小框 / 小胶囊代理）——
    float cpHalfWidth  = 0.18f;  // 半宽（须 < 窄缝半宽 0.4 才钻得过）
    float cpHalfHeight = 0.28f;  // 半高
};

// 一块静态碰撞盒（既喂 Box2D 静态 body，又作 analytic 探针的几何真相）。
struct LevelBox
{
    glm::vec2     min;
    glm::vec2     max;
    std::uint32_t color;
    bool          isGround;  // 平地不参与天花板角落修正探测
};

// Spike 1 规格那一屏：平地 + 左/右平台 + 右边沿 ledge + 中间窄缝两壁。
// 单一数据源：同一份 box 列表既建 Box2D 静态体，又供接地 / 角落 analytic 探针。
std::vector<LevelBox> MakeLevel()
{
    return {
        // 平地：top y=-3，横跨视野（厚 0.5）。
        {{-9.0f, -3.5f}, {9.0f, -3.0f}, kGreen, true},
        // 左平台：top y=-0.5，x∈[-6,-2.5]（厚 0.2）。
        {{-6.0f, -0.7f}, {-2.5f, -0.5f}, kCyan, false},
        // 右平台：top y=0.5，x∈[2.5,6]；右端 x=6 的竖面就是可挂的 ledge。
        {{2.5f, 0.3f}, {6.0f, 0.5f}, kCyan, false},
        // 窄缝左壁：内面 x=-0.4，从地面 top 升到 y=1（厚 0.1）。
        {{-0.5f, -3.0f}, {-0.4f, 1.0f}, kRed, false},
        // 窄缝右壁：内面 x=0.4 —— 内侧缝宽 0.8，刚够 control point（宽 0.36）钻过。
        {{0.4f, -3.0f}, {0.5f, 1.0f}, kRed, false},
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

// ===========================================================================
// SpikeLayer —— Loop A 全部逻辑（输入 + 固定步长物理 + 手感 + overlay + 调参面板）。
// ===========================================================================
class SpikeLayer : public Layer
{
public:
    SpikeLayer(Pipeline& pipeline, World& world, Phys::PhysicsWorld& phys,
               Phys::BodyHandle cp, std::vector<LevelBox> level)
        : Layer("SpikeLayer")
        , mPipeline(pipeline)
        , mWorld(world)
        , mPhys(phys)
        , mCp(cp)
        , mLevel(std::move(level))
    {
        BuildActionMap();
        mVxHistory.fill(0.0f);
    }

    // 留给 Loop B：软体 blob 每帧读这个快照，用弹簧吸附 control point。
    const ControlPointState& GetControlPoint() const noexcept { return mCpState; }

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
        dt = std::clamp(dt, 0.0f, kMaxFrameDt);
        mAccumulator += dt;
        mStepsThisFrame = 0;
        while (mAccumulator >= kFixedDt)
        {
            FixedStep(kFixedDt);
            mAccumulator -= kFixedDt;
            ++mStepsThisFrame;
        }

        // —— 拉取 control point 最新状态供 overlay / Loop B ——
        const auto bxf = mPhys.GetBodyTransform(mCp);
        const auto bvel = mPhys.GetLinearVelocity(mCp);
        mCpState.position = bxf.position;
        mCpState.velocity = bvel;
        mCpState.grounded = mGrounded;

        // 速度历史（判据 #1 延迟曲线）。
        mVxHistory[mVxHead] = bvel.x;
        mVxHead = (mVxHead + 1) % static_cast<int>(mVxHistory.size());

        if (mInputFlashTimer > 0.0f)
        {
            mInputFlashTimer = std::max(0.0f, mInputFlashTimer - dt);
        }

        DrawDebug();
        mPipeline.Render(mWorld);

        // 帧末推进输入状态机：Pressed→Held、Released→Idle。
        mInput.BeginFrame();
    }

    void OnImGui() override
    {
        ImGui::Begin("Loop A — control point 手感调参 (live, no recompile)");

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
    }

private:
    // —— 输入：A/D 或方向键移动 + Space 跳跃 ——
    void BuildActionMap()
    {
        In::ActionMap map;
        auto add = [&map](const char* name, std::initializer_list<In::KeyCode> keys)
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
        add("jump", {In::KeyCode::Space, In::KeyCode::W, In::KeyCode::Up});
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
        mMoveAxis = (right ? 1.0f : 0.0f) - (left ? 1.0f : 0.0f);

        const In::ActionState js = top->GetState("jump");
        mJumpHeld = In::IsHeld(js);
        const bool jumpPressed = In::IsTriggered(js);
        if (jumpPressed)
        {
            mJumpBufferTimer = mParams.jumpBufferTime;  // 预输入缓冲
        }

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
        SyncColliderSize();  // cp 尺寸 slider 改动经 ReplaceFixture 生效（帧外/Step 外）

        glm::vec2 pos = mPhys.GetBodyTransform(mCp).position;

        // 接地探测（analytic，针对 game 自持的 LevelBox —— 引擎 PhysicsWorld
        // 公共面无 contact / raycast 查询，见 NOTES-loopA.md）。
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
        mGrounded = grounded;

        // —— 水平：朝目标速度加/减速（空中控制单独减弱）——
        const float target = mMoveAxis * mParams.maxRunSpeed;
        const float accel = grounded ? mParams.groundAccel : mParams.airAccel;
        const float decel = grounded ? mParams.groundDecel : mParams.airDecel;
        mVel.x = Approach(mVel.x, target, accel, decel, dt);

        // 贴地：站立时不让向下速度累积（保持接触、便于 analytic 探测）。
        if (grounded && mVel.y <= 0.0f)
        {
            mVel.y = 0.0f;
        }

        // —— 跳跃：buffer 与 coyote 同时有效才起跳 ——
        bool airborne = !grounded;
        if (mJumpBufferTimer > 0.0f && mCoyoteTimer > 0.0f)
        {
            mVel.y = mParams.jumpSpeed;
            mJumpBufferTimer = 0.0f;
            mCoyoteTimer = 0.0f;
            airborne = true;
        }
        if (mJumpBufferTimer > 0.0f)
        {
            mJumpBufferTimer = std::max(0.0f, mJumpBufferTimer - dt);
        }

        // —— 重力：上升 / 下落分档 + apex 衰减 + 松手切断上升（可变跳跃高度）——
        if (airborne)
        {
            float g;
            if (mVel.y > 0.0f)
            {
                if (std::abs(mVel.y) < mParams.apexThreshold)
                {
                    g = mParams.riseGravity * mParams.apexGravityMult;  // apex 悬停
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
                        ? mParams.fallGravity * mParams.apexGravityMult  // apex 悬停（下落侧）
                        : mParams.fallGravity;
            }
            mVel.y -= g * dt;
            mVel.y = std::max(mVel.y, -mParams.maxFallSpeed);
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
    }

    // 接地：脚底是否落在某 LevelBox 顶面附近（且未在上升）。
    bool ProbeGrounded(glm::vec2 pos) const
    {
        if (mVel.y > 0.05f)
        {
            return false;  // 正在上升不算接地
        }
        const float feetY = pos.y - mParams.cpHalfHeight;
        constexpr float kLandEps = 0.06f;    // 容许略微嵌入 / 浮空
        constexpr float kProbeDist = 0.10f;  // 脚下探测距离
        for (const auto& b : mLevel)
        {
            const float top = b.max.y;
            const bool xOverlap = (pos.x + mParams.cpHalfWidth > b.min.x) &&
                                  (pos.x - mParams.cpHalfWidth < b.max.x);
            const bool yNear = (feetY <= top + kProbeDist) && (feetY >= top - kLandEps);
            if (xOverlap && yNear)
            {
                return true;
            }
        }
        return false;
    }

    // 头部在给定 x 是否与任一 solid box 的顶角交叠（corner correction 探测）。
    bool HeadClearAt(float x, float y) const
    {
        const float headLo = y + mParams.cpHalfHeight - 0.05f;
        const float headHi = y + mParams.cpHalfHeight + 0.05f;
        for (const auto& b : mLevel)
        {
            if (b.isGround)
            {
                continue;
            }
            const bool xOverlap = (x + mParams.cpHalfWidth > b.min.x) &&
                                  (x - mParams.cpHalfWidth < b.max.x);
            const bool yOverlap = (headHi > b.min.y) && (headLo < b.max.y);
            if (xOverlap && yOverlap)
            {
                return false;
            }
        }
        return true;
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
        col.shape       = Phys::BoxDesc{{mParams.cpHalfWidth, mParams.cpHalfHeight}, {0.0f, 0.0f}};
        col.density     = 1.0f;
        col.friction    = 0.0f;  // 水平由手感逻辑全权控制，不让地面摩擦掺和
        col.restitution = 0.0f;
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
        mVel = {0.0f, 0.0f};
        mCoyoteTimer = 0.0f;
        mJumpBufferTimer = 0.0f;
    }

    // —— overlay：场景碰撞盒 + control point 标记 + 速度向量 + 输入标记 + 落点 ——
    void DrawDebug()
    {
        auto* dbg = mPipeline.GetDebugDrawScene();
        if (!dbg)
        {
            return;
        }

        // 静态世界（debug 线保留作可视参照；同时就是真 Box2D 碰撞体的轮廓）。
        for (const auto& b : mLevel)
        {
            dbg->AddAabb(glm::vec3(b.min, 0.0f), glm::vec3(b.max, 0.0f), b.color);
        }

        const glm::vec2 p = mCpState.position;
        const glm::vec3 c3{p, 0.0f};

        // control point 小框（gameplay 真相层 —— 将来被 Loop B 软体 blob 取代）。
        const glm::vec3 half{mParams.cpHalfWidth, mParams.cpHalfHeight, 0.0f};
        dbg->AddAabb(c3 - half, c3 + half, kYellow);

        // 接地指示：脚底一条短横线（grey=空中, yellow=接地）。
        const float feetY = p.y - mParams.cpHalfHeight;
        dbg->AddLine(glm::vec3(p.x - mParams.cpHalfWidth, feetY, 0.0f),
                     glm::vec3(p.x + mParams.cpHalfWidth, feetY, 0.0f),
                     mGrounded ? kYellow : kGrey);

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
    }

    // 引擎引用 / 物理
    Pipeline&            mPipeline;
    World&               mWorld;
    Phys::PhysicsWorld&  mPhys;
    Phys::BodyHandle     mCp;
    std::vector<LevelBox> mLevel;

    // 输入
    In::InputContext mInput;
    float            mMoveAxis = 0.0f;
    bool             mJumpHeld = false;

    // 手感状态
    FeelParams       mParams;
    glm::vec2        mVel{0.0f, 0.0f};
    bool             mGrounded = false;
    bool             mWasGrounded = false;
    float            mCoyoteTimer = 0.0f;
    float            mJumpBufferTimer = 0.0f;
    float            mLastHalfW = -1.0f;  // 触发首帧 ReplaceFixture 同步
    float            mLastHalfH = -1.0f;

    // 物理累加器
    float            mAccumulator = 0.0f;
    int              mStepsThisFrame = 0;

    // 对外快照（Loop B）
    ControlPointState mCpState;

    // overlay / 判据
    float                  mInputFlashTimer = 0.0f;
    std::array<float, 180> mVxHistory{};
    int                    mVxHead = 0;
    std::vector<glm::vec2> mLandings;
};

}  // namespace

int main()
{
    AppConfig cfg{};
    cfg.window.title  = "OrangeGames - spike-01-blob (Loop A: control point feel)";
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
        const float halfH = 5.0f;
        const float aspect = static_cast<float>(cfg.window.width)
                           / static_cast<float>(cfg.window.height);
        const float halfW = halfH * aspect;
        Camera cam = Camera::Orthographic(-halfW, halfW, -halfH, halfH, 0.1f, 100.0f);
        cam.view = glm::lookAt(glm::vec3(0.0f, 0.0f, 10.0f),
                               glm::vec3(0.0f, 0.0f, 0.0f),
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
        const glm::vec2 center = 0.5f * (b.min + b.max);
        const glm::vec2 half   = 0.5f * (b.max - b.min);
        Phys::RigidBodyComponent rb;
        rb.type            = Phys::BodyType::Static;
        rb.initialPosition = center;
        Phys::ColliderComponent col;
        col.shape    = Phys::BoxDesc{half, {0.0f, 0.0f}};
        col.friction = 0.0f;
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
        col.shape       = Phys::BoxDesc{{0.18f, 0.28f}, {0.0f, 0.0f}};
        col.density     = 1.0f;
        col.friction    = 0.0f;
        col.restitution = 0.0f;
        cpBody = physWorld.AddBody(rb, col);
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

    // 接通引擎托管 ImGui overlay + 把 LayerStack 的 OnImGui 派发接进来（调参面板）。
    if (auto im = pipeline.EnableImGui(); im.IsErr())
    {
        std::fprintf(stderr, "Pipeline::EnableImGui failed (code=%u)\n",
                     static_cast<unsigned>(im.Error()));
        return 1;
    }
    pipeline.SetImGuiSubmit([h = host.get()]() { h->DispatchImGui(); });

    host->PushLayer(std::make_unique<SpikeLayer>(pipeline, world, physWorld, cpBody, level));

    const int rc = host->Run();
    pipeline.Shutdown();
    return rc;
}
