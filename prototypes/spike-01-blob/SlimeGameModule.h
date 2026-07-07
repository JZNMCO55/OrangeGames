// spike-01-blob —— SlimeGameModule：把 spike 的史莱姆玩法抽成 IGameModule。
//
// spike 原是独立 .exe（main.cpp 里 SpikeLayer + main() 装配）。本模块把
// SpikeLayer 的**全部玩法逻辑**（Loop A control point 手感 + Loop B 软体
// blob + D1 状态形变 + juice + Tier2 SDF 渲染接线）搬进一个语言无关的
// IGameModule，交由编辑器 / runtime 宿主（GameModuleHost）驱动，实现
// edit-while-play + Stop 还原（ADR-021 PIE 双语言宿主）。
//
// 分工（哪些留宿主、哪些进模块）：
//   * 进模块 = 玩法装配：control point body + 关卡静态 body（OnEnterPlay 建、
//     OnExitPlay 拆）、固定步长自管物理（WantsOwnPhysicsStep=true）、SDF
//     metaball pass 注册（RegisterRenderPasses）+ 逐帧喂数据（Tick）。
//   * 留宿主 = 表现装配：相机 / clear color / sky / bloom 链 —— 不进模块。
//
// 与 spike 的差异：
//   * 去掉 Layer::OnImGui（IGameModule 无调参段；手感参数保持内置成员默认值，
//     组件化 / Inspector 是后续 defer 项）；
//   * 去掉 mPipeline.Render / OnResize（宿主管渲染与 resize）；
//   * 引擎引用（Pipeline / PhysicsWorld）全改为经 GameModuleContext 传入。
//
// Blob.h + SlimeMetaballPass.h 原地复用（本模块 include，不改不移）。

#ifndef SPIKE01_SLIME_GAME_MODULE_H
#define SPIKE01_SLIME_GAME_MODULE_H

#include <orange/engine/game/IGameModule.h>
#include <orange/engine/input/InputContext.h>
#include <orange/engine/physics/BodyHandle.h>

#include "Blob.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace spike01
{

    // Tier2 自定义 SDF pass（前向声明——模块只持有借用裸指针，pass 所有权在
    // Pipeline；完整类型在 .cpp include）。
    class SlimeMetaballPass;

    // ---------------------------------------------------------------------------
    // 手感旋钮（原 spike 的 FeelParams，内置成员默认值；组件化 defer）。
    // baseline 抄 Celeste 经典起点，供真机微调。
    // ---------------------------------------------------------------------------
    struct FeelParams
    {
        // —— 地面移动 ——
        float maxRunSpeed = 9.0f;
        float groundAccel = 90.0f;
        float groundDecel = 120.0f;

        // —— 空中控制 ——
        float airAccel = 60.0f;
        float airDecel = 60.0f;

        // —— 跳跃 ——
        float jumpSpeed       = 16.0f;
        float riseGravity     = 45.0f;
        float fallGravity     = 70.0f;
        float apexThreshold   = 3.0f;
        float apexGravityMult = 0.55f;
        float lowJumpMult     = 2.5f;
        float maxFallSpeed    = 25.0f;

        // —— juice 辅助 ——
        float coyoteTime           = 0.08f;
        float jumpBufferTime       = 0.10f;
        float cornerCorrectionDist = 0.25f;

        // —— 爬墙 wall-climb ——
        bool  wallEnabled      = true;
        float wallSlideSpeed   = 3.5f;
        float wallJumpSpeedX   = 11.0f;
        float wallJumpSpeedY   = 15.0f;
        float wallJumpLockTime = 0.12f;
        float wallCoyoteTime   = 0.08f;
        float wallClimbSpeed   = 4.0f;
        float wallStamina      = 0.0f; // 0 = 无限
        float wallDetectDist   = 0.06f;

        // —— control point 尺寸 ——
        float cpHalfWidth  = 0.18f;
        float cpHalfHeight = 0.28f;
    };

    // 一块静态碰撞盒：既喂物理静态 body、又供 debug 线框、又作 Blob 碰撞 AABB 源。
    struct LevelBox
    {
        glm::vec2     min;
        glm::vec2     max;
        std::uint32_t color;
        bool          isGround; // true=平地；false=平台/墙
    };

    // control point 对外快照（Loop B 软体 blob 弹簧吸附用）。
    struct ControlPointState
    {
        glm::vec2 position{0.0f, 0.0f};
        glm::vec2 velocity{0.0f, 0.0f};
        bool      grounded{false};
    };

    // 实心史莱姆外观（Tier1 CPU 逐面伪着色 gel；SDF pass 开启时不走此路径）。
    struct SlimeParams
    {
        bool fill      = true;
        bool wireframe = false; // 保留 debug 线框对照
        bool beauty    = true;  // 展示模式：只画史莱姆(黑底)，隐藏关卡线框 + overlay

        // —— 胶体分层色（厚→薄）——
        glm::vec3 deepColor  = {0.02f, 0.16f, 0.05f};
        glm::vec3 bodyColor  = {0.11f, 0.62f, 0.20f};
        glm::vec3 rimColor   = {0.55f, 1.00f, 0.55f};
        glm::vec3 eyeColor   = {1.00f, 0.92f, 0.45f};
        glm::vec3 speckColor = {0.85f, 1.00f, 0.42f};
        glm::vec3 glowColor  = {0.18f, 0.85f, 0.28f};

        // —— 伪光照 ——
        glm::vec3 lightDir  = {-0.35f, 0.85f, 0.42f};
        float     specPower = 32.0f;
        float     specGain  = 1.15f;
        float     rimPower  = 3.0f;
        float     rimGain   = 0.85f;
        float     ambient   = 0.30f;

        // —— 网格密度 / 细节 ——
        int   rings           = 12;
        int   silhouetteSub   = 3;
        bool  drawEyes        = true;
        bool  drawSpecks      = true;
        bool  drawContactGlow = true;
        float eyeSize         = 0.21f;
    };

    // D1 形变驱动状态（对齐 12 态参考图）。Crouch/Rolling/Squeezed 无 open-ground emergent
    // 触发（输入接线 defer），经 forced-state 摆姿（showcase / 未来 demo trigger）。
    enum class SlimeMotionState
    {
        Idle,
        Crouch,   // 下压/蓄力：宽扁矮
        Launch,   // 拉伸：起跳前纵向拉长
        Rising,
        Falling,
        Landing,
        Sliding,
        Rolling,  // 滚动：椭圆 + 翻滚（旋转 target shape）
        Squeezed, // 受挤压：窄缝压扁
    };

    // 自动演示阶段（attract / 免手玩动态验证：脚本注入虚拟输入，复用真实手感）。
    enum class DemoPhase
    {
        Stand,
        Jump,
        RunRight,
        StopR,
        RunLeft,
        StopL,
    };

    // juice 溅射水滴（物理粒子：朝外上放射 + 自管重力，随生命缩小）。
    struct SlimeDroplet
    {
        glm::vec2 pos{0.0f, 0.0f};
        glm::vec2 vel{0.0f, 0.0f};
        float     radius     = 0.1f;
        float     baseRadius = 0.1f;
        float     life       = 0.0f;
        float     maxLife    = 0.4f;
    };

    enum class JuiceMode
    {
        None,
        Split,
        Merge,
    };

    // ===========================================================================
    // SlimeGameModule —— spike 全部玩法逻辑的 IGameModule 封装。
    // ===========================================================================
    class SlimeGameModule final : public Orange::Engine::Game::IGameModule
    {
    public:
        SlimeGameModule();

        const char* Name() const noexcept override
        {
            return "SlimeGameModule";
        }

        // 注册期：InsertPass(AfterMainPass) 接入 SDF metaball pass，存裸指针供 Tick 喂数据。
        void RegisterRenderPasses(Orange::Engine::Render::Pipeline& pipeline) override;

        // 对偶：DLL 热重载卸载 slime.dll 前摘掉 SDF pass（pass 代码在 dll 内，悬垂会崩）。
        void UnregisterRenderPasses(Orange::Engine::Render::Pipeline& pipeline) override;

        // Play：建关卡静态 body + control point body、复位 blob / 运行态。
        void OnEnterPlay(Orange::Engine::Game::GameModuleContext& ctx) override;

        // spike 自管固定步长 accumulator，Step 自己发 → 宿主让位。
        bool WantsOwnPhysicsStep() const noexcept override
        {
            return true;
        }

        // 每帧：输入 → 固定步长物理 → 形变 → juice → 喂 SDF pass → debug 绘制。宿主管渲染。
        void Tick(Orange::Engine::Game::GameModuleContext& ctx, float dt) override;

        // 视口聚焦时宿主转发窗口事件：仅取 key 喂 InputContext（resize 归宿主）。
        void OnEvent(const Orange::Engine::Platform::WindowEvent& event) override;

        // 退 Play：拆 control point + 关卡 body、清运行态；SDF pass 常驻（仅禁用避免 edit 态残帧）。
        void OnExitPlay(Orange::Engine::Game::GameModuleContext& ctx) override;

        // 交出 SlimeTuningComponent 序列化器 → 宿主 merge 进 extraSerializers，令手感
        // 组件随场景 / Play-Stop 快照 round-trip（否则进 Play 时 Edit 世界种的组件丢失）。
        std::span<const Orange::Engine::Scene::ComponentSerializerEntry>
        ComponentSerializers() const override;

        // Loop B 软体 blob 每帧读此快照弹簧吸附 control point（对外只读）。
        const ControlPointState& GetControlPoint() const noexcept
        {
            return mCpState;
        }

    private:
        // —— 输入 ——
        void BuildActionMap();
        void SampleInput();

        // —— 自动演示 ——
        void StartDemo();
        void AdvanceDemoPhase(DemoPhase next);
        void UpdateDemoScript(float dt);

        // —— 固定物理子步 + 探针 ——
        void FixedStep(float dt);
        bool ProbeGrounded(glm::vec2 pos) const;
        bool HeadClearAt(float x, float y) const;
        bool ProbeWall(glm::vec2 pos, int dir) const;
        bool TryCornerCorrect(glm::vec2 pos, float restoreVy);
        void SyncColliderSize();
        void RecordLanding(glm::vec2 pos);
        void ResetControlPoint();

        // —— D1 形变驱动 ——
        SlimeMotionState DeriveMotionState(float dt);
        void             UpdateDeform(float dt);

        // —— juice ——
        void SpawnSplatter();
        void TriggerSplit();
        void TriggerMerge();
        void UpdateDroplets(float dt);
        void UpdateJuiceScript(float dt, std::vector<glm::vec2>& pos, std::vector<float>& rad);

        // —— overlay ——
        void DrawDebug();

        // 拆卸本模块 OnEnterPlay 加进物理世界的全部 body（control point + 关卡）。
        void TearDownBodies();

        // 从 Edit 世界第一个挂 SlimeTuningComponent 的实体读手感覆盖内置默认值
        //（OnEnterPlay 在建 body / Blob.Reset 之前调；无组件 / 无 World 则维持默认）。
        void ApplyTuningOverrides(Orange::Engine::World* pWorld);

        // —— 宿主经 GameModuleContext 传入的引擎引用（OnEnterPlay 存、Tick 复用）——
        Orange::Engine::Physics::PhysicsWorld* mpPhysics  = nullptr;
        Orange::Engine::Render::Pipeline*      mpPipeline = nullptr;

        // control point + 关卡几何（模块自建，OnExitPlay 拆）
        Orange::Engine::Physics::BodyHandle              mCp{};
        std::vector<Orange::Engine::Physics::BodyHandle> mLevelBodies;
        std::vector<LevelBox>                            mLevel;

        // Tier2 SDF pass（RegisterRenderPasses 存的借用指针；所有权在 Pipeline）
        SlimeMetaballPass* mpSdfPass = nullptr;

        SlimeParams mSlime; // Tier1 CPU 史莱姆外观（SDF 关时的 fallback + overlay）

        // 输入
        Orange::Engine::Input::InputContext mInput;
        float                               mMoveAxis      = 0.0f;
        bool                                mJumpHeld      = false;
        bool                                mGrabHeld      = false;
        bool                                mClimbUpHeld   = false;
        bool                                mClimbDownHeld = false;

        // 手感状态
        FeelParams mParams;
        glm::vec2  mVel{0.0f, 0.0f};
        bool       mGrounded        = false;
        bool       mWasGrounded     = false;
        float      mCoyoteTimer     = 0.0f;
        float      mJumpBufferTimer = 0.0f;

        // 爬墙状态
        int   mWallDir           = 0;
        int   mLastWallDir       = 0;
        bool  mSliding           = false;
        bool  mGrabbing          = false;
        float mWallCoyoteTimer   = 0.0f;
        float mWallJumpLockTimer = 0.0f;
        int   mWallJumpAwayDir   = 0;
        float mStamina           = 0.0f;
        float mWallJumpFlash     = 0.0f;

        float mLastHalfW = -1.0f; // 触发首帧 ReplaceFixture 同步
        float mLastHalfH = -1.0f;

        // 物理累加器
        float mAccumulator    = 0.0f;
        int   mStepsThisFrame = 0;
        float mRenderTime     = 0.0f; // 视觉动画时钟（与固定物理步解耦）

        // 对外快照（Loop B）
        ControlPointState mCpState;

        // Loop B 软体 blob
        spike01::Blob       mBlob{14};
        spike01::BlobParams mBlobParams;
        glm::vec2           mBlobCentroid{0.0f, 0.0f};
        float               mDivergence     = 0.0f;
        float               mApexDivergence = 0.0f;
        float               mPeakDivergence = 0.0f;
        float               mPrevVy         = 0.0f;

        // D1 形变驱动状态
        bool             mDeformEnabled      = true;
        SlimeMotionState mMotionState        = SlimeMotionState::Idle;
        float            mLandingTimer       = 0.0f;
        float            mLaunchTimer        = 0.0f;
        bool             mDeformPrevGrounded = false;
        float            mDeformPrevVy       = 0.0f;
        float            mCurSx              = 1.15f; // init = Idle 矮 dome
        float            mCurSy              = 0.80f;
        float            mCurStiff           = 200.0f;
        float            mDeformTau          = 0.06f;

        // 强制姿态（showcase / 未来 demo trigger）：置位则 UpdateDeform 用 mForcedState 取代
        // 物理派生态，令无 emergent 触发的 Crouch/Rolling/Squeezed 也能摆出。正常玩法恒 false。
        bool             mHasForcedState     = false;
        SlimeMotionState mForcedState        = SlimeMotionState::Idle;
        float            mRollAngle          = 0.0f; // Rolling 翻滚角（rad）
        float            mRollSpeed          = 3.2f; // 翻滚角速度（rad/s）

        // 自动演示
        bool      mAutoDemo      = false;
        DemoPhase mDemoPhase     = DemoPhase::Stand;
        float     mDemoTimer     = 0.0f;
        bool      mDemoJumpFired = false;

        // juice：溅射粒子 + 分裂/合并脚本
        std::vector<SlimeDroplet> mDroplets;
        JuiceMode                 mJuiceMode  = JuiceMode::None;
        float                     mJuiceTimer = 0.0f;

        // overlay / 判据
        float                  mInputFlashTimer = 0.0f;
        std::array<float, 180> mVxHistory{};
        int                    mVxHead = 0;
        std::vector<glm::vec2> mLandings;
    };

} // namespace spike01

#endif // SPIKE01_SLIME_GAME_MODULE_H
