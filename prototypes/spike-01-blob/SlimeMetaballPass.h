#ifndef SPIKE01_SLIME_METABALL_PASS_H
#define SPIKE01_SLIME_METABALL_PASS_H

// Tier 2 —— 自定义 fullscreen SDF metaball pass。
//
// M0：证明整条 RHI 管线（pipeline / descriptor / UBO / alpha blend / layout
// transition）在引擎 InsertPass(AfterMainPass) hook 下跑通（硬编码中心 1 质点）。
// M1：接真 blob 软体——每帧 SetBlob 灌 14 perimeter + 1 centroid 世界坐标，逐像素
// 多质点 metaball 密度场投影出连续平滑史莱姆剪影（跟随 blob，纯绿 mask，着色留 M2）。
//
// consumer 允许直接 #include <orange/rhi/...>（header isolation 只约束引擎内部，
// 不约束游戏侧；IRenderPass 就是引擎为游戏开的下沉到 RHI 级的逃生舱口）。本头
// 仅前向声明 RHI 类型，具体 include 落在 .cpp。

#include <orange/engine/render/IRenderPass.h>
#include <orange/engine/render/RenderPassContext.h>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace Orange::Rhi
{
    class RHIDevice;
    class RHIPipeline;
    class RHIDescriptorSetLayout;
    class RHIDescriptorPool;
    class RHIDescriptorSet;
    class RHIBuffer;
} // namespace Orange::Rhi

namespace Orange::Resource
{
    class ShaderResource;
} // namespace Orange::Resource

namespace spike01
{

    class SlimeMetaballPass final : public Orange::Engine::Render::IRenderPass
    {
    public:
        // 引擎 renderer 是双缓冲（2 帧在飞）；分 3 片属过量分配保险——写的 slot
        // 与最近 2 帧用的 slot 恒不同，避免写正被 GPU 读的内存。
        static constexpr std::uint32_t kFramesInFlight = 3;
        static constexpr int           kMaxPoints      = 16;
        static constexpr int           kMaxDroplets    = 8; // juice 额外 metaball 点上限

        SlimeMetaballPass();
        ~SlimeMetaballPass() override; // 在 .cpp defaulted，让 RHI unique_ptr 成员析构在完整类型下实例化

        // 调参（ImGui live）。M1 剪影：falloffScale/isoLevel。M2 材质：domeScale/rimGain/
        // specGain/ambient。M3 角色元素强度：eyeGain/speckGain/glowGain（emissive 喂 bloom）。
        struct Tunables
        {
            float falloffScale = 1.0f;  // M1：falloff = falloffScale × blob 投影半径（ndc-y）
            float isoLevel     = 0.6f;  // M1：smoothstep 剪影阈值
            float domeScale    = 1.38f; // M3：半球半径倍数（盖住剪影侧带；边缘 N.z→0 让 Fresnel 出来）
            float rimGain      = 0.85f; // M3：Fresnel 亮边（全透体后轮廓靠边线+bloom 表达；>1.0 晕过肥）
            float specGain     = 1.50f; // M2：湿润高光强度（spec power 提高后锐点需更亮才可读）
            float ambient      = 0.40f; // M2：环境光基线（透光模型下压低，让垂直梯度对比读出来）
            float eyeGain      = 3.0f;  // M3：发光眼 emissive 强度
            float speckGain    = 2.3f;  // M3：体内光斑 emissive 强度
            float glowGain     = 1.0f;  // M3：底部接触辉光 emissive 强度（与 core/pool/底亮边叠加，>1.2 白洗）
            float opacity      = 0.40f; // M4 质感：体内不透明度（透明是"透亮"的主体——黑底下内部
                                        // 应读成"透过果冻的背景"而非实心填充；边缘吸收带/Fresnel 回实）
            float coreGain     = 1.0f;  // M4 质感：内芯发光核强度（暖色主要走乘性梯度，additive 只点睛）

            // 颜色（原 frag 常量提为 UBO uniform；默认值 = 2026-07-08 对参考图收敛值，
            // 与 SlimeTuningComponent 默认值保持同步）。
            glm::vec3 bodyColor  = {0.13f, 0.66f, 0.20f}; // 主体绿（下半通透亮端；梯度结构撑住后可抬亮）
            glm::vec3 deepColor  = {0.03f, 0.26f, 0.08f}; // 上半深绿（垂直梯度深端 + 边缘吸收带色）
            glm::vec3 rimColor   = {0.62f, 1.00f, 0.50f}; // Fresnel 亮边
            glm::vec3 eyeColor   = {1.00f, 0.92f, 0.45f}; // 发光眼
            glm::vec3 speckColor = {1.00f, 0.86f, 0.34f}; // 体内金斑（参考图偏金非黄绿）
            glm::vec3 glowColor  = {0.35f, 0.95f, 0.30f}; // 底部接触辉光
        };

        const char* Name() const noexcept override { return "SlimeMetaballPass"; }
        void        Setup(Orange::Engine::Render::RenderGraphBuilder& builder) override;
        void        Execute(Orange::Engine::Render::RenderPassContext& ctx) override;

        // 运行时开关（ImGui 勾选）——关掉本 pass 只是 Execute 早退，不动 HDR 布局。
        void SetEnabled(bool enabled) noexcept { mEnabled = enabled; }
        bool IsEnabled() const noexcept { return mEnabled; }

        Tunables& GetTunables() noexcept { return mTunables; }

        // M1 数据流：每帧 blob.Step 之后由 SpikeLayer 调，缓存 perimeter 世界坐标 +
        // centroid + radius。Execute 用缓存写 UBO。perimeterWorld 指向 blob.Positions()
        // 前 count 个 perimeter 点；count 超 kMaxPoints-1 时截断（末尾留给 centroid）。
        void SetBlob(const glm::vec2* perimeterWorld, int count,
                     glm::vec2 centroidWorld, float radius);

        // M3：视觉动画时钟（秒，单调递增）——眼呼吸 / 光斑明灭。SpikeLayer 每帧传
        // mRenderTime（与物理步解耦的视觉时钟）。Execute 写进 UBO。
        void SetTime(float seconds) noexcept { mTime = seconds; }

        // juice：每帧喂额外 metaball 点（落地溅射 / 分裂 / 合并的水滴，世界坐标 + 世界半径）。
        // 独立于 blob 14+1 质点；shader 累加进同一 field 与主体天然 smin 融合 / 断开。
        // count 超 kMaxDroplets 截断。传 count=0（或不调）= 无 droplet。
        void SetDroplets(const glm::vec2* pos, const float* radius, int count);

    private:
        // std140 UBO，与 slime_metaball.frag 逐字对齐：
        // mat4(64) + vec4×4(64) + vec4×6 颜色(96) + vec4[16](256) + vec4[8](128) = 608B。
        struct SlimeUbo
        {
            float uViewProj[16];            // world -> clip（= ctx.pViewProjData）
            float uParams0[4];              // x=count, y=isoLevel, z=aspect, w=falloffRadius(ndc-y)
            float uParams1[4];              // x=blobNdcRadius, y=domeScale, z=rimGain, w=specGain
            float uParams2[4];              // x=ambient, y=time, z=eyeGain, w=speckGain
            float uParams3[4];              // x=glowGain, y=dropletCount, z=opacity, w=coreGain
            float uColors[6][4];            // rgb=颜色（顺序：body/deep/rim/eye/speck/glow）
            float uPoints[kMaxPoints][4];   // xy=world pos（末尾一个是 centroid）
            float uDroplets[kMaxDroplets][4]; // xy=world pos, z=falloff(ndc-y), w=intensity
        };

        bool     mEnabled = true;
        Tunables mTunables;

        // M1 blob 缓存（SetBlob 写、Execute 读；同线程无需同步）。
        std::vector<glm::vec2> mPerimeterWorld;
        glm::vec2              mCentroidWorld{0.0f, 0.0f};
        float                  mBlobRadius = 0.5f;
        bool                   mHasBlob    = false;
        float                  mTime       = 0.0f; // M3 视觉动画时钟

        // juice droplet 缓存（世界坐标 + 世界半径；SetDroplets 写、Execute 读）。
        std::vector<glm::vec2> mDropletPos;
        std::vector<float>     mDropletRadius;

        Orange::Rhi::RHIDevice* mpDevice = nullptr; // Setup 缓存的借用指针

        std::unique_ptr<Orange::Resource::ShaderResource>    mVs;
        std::unique_ptr<Orange::Resource::ShaderResource>    mFs;
        std::unique_ptr<Orange::Rhi::RHIPipeline>            mPipeline;
        std::unique_ptr<Orange::Rhi::RHIDescriptorSetLayout> mLayout;
        std::unique_ptr<Orange::Rhi::RHIDescriptorPool>      mPool;
        std::array<std::unique_ptr<Orange::Rhi::RHIDescriptorSet>, kFramesInFlight> mSets;
        std::unique_ptr<Orange::Rhi::RHIBuffer> mUbo;

        std::uint64_t mSliceBytes = 0; // 单片大小（对齐到 minUniformBufferOffsetAlignment）
    };

} // namespace spike01

#endif // SPIKE01_SLIME_METABALL_PASS_H
