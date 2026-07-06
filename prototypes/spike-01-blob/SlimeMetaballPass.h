#ifndef SPIKE01_SLIME_METABALL_PASS_H
#define SPIKE01_SLIME_METABALL_PASS_H

// Tier 2 M0 —— 自定义 fullscreen SDF metaball pass。
//
// 目标只有一个：证明整条 RHI 管线（graphics pipeline / descriptor / UBO / alpha
// blend / layout transition）能在引擎的 InsertPass(AfterMainPass) hook 下跑通，
// 出一个平滑圆 alpha-blend 合成到 HDR 上屏。**不追求好看、不接真 blob 数据**
//（M0 硬编码 1 个屏幕中心质点，逐像素密度场出圆）。
//
// consumer 允许直接 #include <orange/rhi/...>（header isolation 只约束引擎内部，
// 不约束游戏侧；IRenderPass 就是引擎为游戏开的下沉到 RHI 级的逃生舱口）。本头
// 仅前向声明 RHI 类型，具体 include 落在 .cpp。

#include <orange/engine/render/IRenderPass.h>
#include <orange/engine/render/RenderPassContext.h>

#include <array>
#include <cstdint>
#include <memory>

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

        SlimeMetaballPass();
        ~SlimeMetaballPass() override; // 在 .cpp defaulted，让 RHI unique_ptr 成员析构在完整类型下实例化

        const char* Name() const noexcept override { return "SlimeMetaballPass"; }
        void        Setup(Orange::Engine::Render::RenderGraphBuilder& builder) override;
        void        Execute(Orange::Engine::Render::RenderPassContext& ctx) override;

        // 运行时开关（ImGui 勾选）——关掉本 pass 只是 Execute 早退，不动 HDR 布局。
        void SetEnabled(bool enabled) noexcept { mEnabled = enabled; }
        bool IsEnabled() const noexcept { return mEnabled; }

    private:
        // std140 UBO，与 slime_metaball.frag 逐字对齐：mat4(64) + vec4(16) + vec4[16](256)。
        struct SlimeUbo
        {
            float uViewProj[16];             // M1 才用；M0 留单位阵占位
            float uParams0[4];               // x=count, y=isoLevel, z=aspect, w=time
            float uPoints[kMaxPoints][4];    // xy=NDC, z=radius, w=weight
        };

        bool mEnabled = true;

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
