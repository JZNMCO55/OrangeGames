#include "SlimeMetaballPass.h"

#include <orange/resource/ShaderResource.h>
#include <orange/rhi/RHIBuffer.h>
#include <orange/rhi/RHICommandList.h>
#include <orange/rhi/RHIDescriptor.h>
#include <orange/rhi/RHIDevice.h>
#include <orange/rhi/RHIPipeline.h>
#include <orange/rhi/RHIRendering.h>
#include <orange/rhi/RHITexture.h>

#include <cstring>
#include <filesystem>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace spike01
{

    namespace Rhi = Orange::Rhi;
    using Orange::Engine::Render::RenderGraphBuilder;
    using Orange::Engine::Render::RenderPassContext;

    namespace
    {
        std::uint64_t AlignUp(std::uint64_t value, std::uint64_t align)
        {
            if (align <= 1)
            {
                return value;
            }
            return (value + align - 1) & ~(align - 1);
        }

        // 引擎按 exe 相对路径找 shader（GetExecutableDir，见 VfxSystem/MaterialSystem）。
        // OrangeRender 的 ShaderResource::CreateFromFile 是 CWD 相对（裸 fopen），故这里
        // 自己拼 exe 相对绝对路径，让从任意 CWD 启动都能加载 .spv。
        std::filesystem::path GetExecutableDir()
        {
#ifdef _WIN32
            wchar_t buffer[MAX_PATH];
            const DWORD len = ::GetModuleFileNameW(nullptr, buffer, MAX_PATH);
            if (len == 0 || len == MAX_PATH)
            {
                return std::filesystem::current_path();
            }
            return std::filesystem::path(std::wstring(buffer, len)).parent_path();
#else
            return std::filesystem::current_path();
#endif
        }
    } // namespace

    SlimeMetaballPass::SlimeMetaballPass()  = default;
    SlimeMetaballPass::~SlimeMetaballPass() = default;

    void SlimeMetaballPass::Setup(RenderGraphBuilder& builder)
    {
        mpDevice = builder.pDevice;
        if (mpDevice == nullptr)
        {
            return;
        }
        // 幂等：Resize 会重复调 Setup。本 pass 资源与分辨率无关（pipeline 用固定 HDR
        // 格式、UBO 每帧写），已建则直接跳过。
        if (mPipeline)
        {
            return;
        }
        Rhi::RHIDevice& device = *mpDevice;

        // shader 模块（.spv 由 CMake POST_BUILD 编到 .exe 旁 shaders/）。
        const std::string vsPath = (GetExecutableDir() / "shaders" / "fullscreen.vert.spv").string();
        const std::string fsPath = (GetExecutableDir() / "shaders" / "slime_metaball.frag.spv").string();
        mVs = Orange::Resource::ShaderResource::CreateFromFile(device, Rhi::ShaderStage::Vertex, vsPath, "slime.vs");
        mFs = Orange::Resource::ShaderResource::CreateFromFile(device, Rhi::ShaderStage::Fragment, fsPath, "slime.fs");
        if (!mVs || !mFs || mVs->GetModule() == nullptr || mFs->GetModule() == nullptr)
        {
            mVs.reset();
            mFs.reset();
            return; // 缺 .spv → 不建 pipeline，Execute 会早退（无崩溃，只是没圆）
        }

        // descriptor set layout：binding 0 = UBO（fragment stage）。
        Rhi::DescriptorSetLayoutDesc layoutDesc{};
        layoutDesc.mBindings = {
            {0, Rhi::DescriptorType::UniformBuffer, 1, Rhi::ShaderStage::Fragment},
        };
        mLayout = device.CreateDescriptorSetLayout(layoutDesc);
        if (!mLayout)
        {
            return;
        }

        // 每 FIF slice 一个 set：非动态 UBO 不能用 dynamicOffsets 选片，改为每片一个
        // set 指向自己的 slice。也避开"更新在用 set"的 hazard（set 一次性 update）。
        Rhi::DescriptorPoolDesc poolDesc{};
        poolDesc.mMaxSets   = kFramesInFlight;
        poolDesc.mPoolSizes = {{Rhi::DescriptorType::UniformBuffer, kFramesInFlight}};
        mPool               = device.CreateDescriptorPool(poolDesc);
        if (!mPool)
        {
            return;
        }

        // UBO：一整块切 kFramesInFlight 片。非动态 UBO 的 descriptor offset 必须是
        // minUniformBufferOffsetAlignment 的倍数，故按该值对齐单片大小。
        const std::uint64_t uboAlign = device.GetCapabilities().mLimits.mMinUniformBufferOffsetAlignment;
        mSliceBytes                  = AlignUp(sizeof(SlimeUbo), uboAlign == 0 ? 256 : uboAlign);

        Rhi::BufferDesc ubDesc{};
        ubDesc.mSize        = mSliceBytes * kFramesInFlight;
        ubDesc.mUsage       = Rhi::BufferUsage::Uniform;
        ubDesc.mMemoryUsage = Rhi::MemoryUsage::CpuToGpu;
        mUbo                = device.CreateBuffer(ubDesc);
        if (!mUbo)
        {
            return;
        }

        for (std::uint32_t i = 0; i < kFramesInFlight; ++i)
        {
            mSets[i] = device.AllocateDescriptorSet(*mPool, *mLayout);
            if (!mSets[i])
            {
                return;
            }
            Rhi::DescriptorWrite write{};
            write.mBinding             = 0;
            write.mType                = Rhi::DescriptorType::UniformBuffer;
            write.mBufferInfo.mpBuffer = mUbo.get();
            write.mBufferInfo.mOffset  = static_cast<std::uint64_t>(i) * mSliceBytes;
            write.mBufferInfo.mRange   = sizeof(SlimeUbo);
            device.UpdateDescriptorSet(*mSets[i], &write, 1);
        }

        // graphics pipeline：空顶点输入（顶点靠 gl_VertexIndex 生成）+ CullMode::None
        // + 深度双关 + alpha over blend。
        Rhi::GraphicsPipelineDesc gp{};
        gp.mShaderStages.push_back({Rhi::ShaderStage::Vertex, mVs->GetModule(), "main"});
        gp.mShaderStages.push_back({Rhi::ShaderStage::Fragment, mFs->GetModule(), "main"});
        gp.mRasterizer.mCullMode           = Rhi::CullMode::None;
        gp.mDepthStencil.mDepthTestEnable  = false;
        gp.mDepthStencil.mDepthWriteEnable = false;
        gp.mDescriptorSetLayouts.push_back(mLayout.get());

        Rhi::ColorBlendAttachmentDesc blend{};
        blend.mBlendEnable         = true;
        blend.mSrcColorBlendFactor = Rhi::BlendFactor::SrcAlpha;
        blend.mDstColorBlendFactor = Rhi::BlendFactor::OneMinusSrcAlpha; // 覆盖式 over
        blend.mColorBlendOp        = Rhi::BlendOp::Add;
        blend.mSrcAlphaBlendFactor = Rhi::BlendFactor::One;
        blend.mDstAlphaBlendFactor = Rhi::BlendFactor::One;
        blend.mAlphaBlendOp        = Rhi::BlendOp::Add;
        blend.mColorWriteMask      = Rhi::ColorWriteMask::All;
        gp.mColorBlend.mAttachments.push_back(blend);

        // 注入目标 HDR scene color = RGBA16Float（引擎 hdrColor 格式；dynamic rendering
        // 要求 pipeline color format 与之逐位兼容）。
        gp.mRenderTargets.mColorFormats.push_back(Rhi::TextureFormat::RGBA16Float);
        gp.mpDebugName = "SlimeMetaballPass";

        mPipeline = device.CreateGraphicsPipeline(gp);
    }

    void SlimeMetaballPass::Execute(RenderPassContext& ctx)
    {
        if (!mEnabled || !mPipeline || ctx.pCmdList == nullptr || ctx.pHdrColorView == nullptr)
        {
            return;
        }

        Rhi::RHICommandList& cmd     = *ctx.pCmdList;
        Rhi::RHITextureView* hdrView = ctx.pHdrColorView;
        Rhi::RHITexture&     hdrTex  = hdrView->GetTexture();

        // FIF slice：frameIndex 是引擎单调递增的 ring-buffer 索引。写的 slot 与最近
        // 2 帧不同 → 不撞正被 GPU 读的内存。
        const std::uint32_t slot        = static_cast<std::uint32_t>(ctx.frameIndex % kFramesInFlight);
        const std::uint64_t sliceOffset = static_cast<std::uint64_t>(slot) * mSliceBytes;

        // 写本 slice：M0 硬编码 1 个屏幕中心质点。
        if (auto* pMapped = static_cast<std::uint8_t*>(mUbo->Map()))
        {
            auto* pUbo = reinterpret_cast<SlimeUbo*>(pMapped + sliceOffset);
            std::memset(pUbo, 0, sizeof(SlimeUbo));
            pUbo->uViewProj[0] = pUbo->uViewProj[5] = pUbo->uViewProj[10] = pUbo->uViewProj[15] = 1.0f;

            const float aspect = (ctx.height != 0)
                                     ? static_cast<float>(ctx.width) / static_cast<float>(ctx.height)
                                     : 1.0f;
            pUbo->uParams0[0] = 1.0f;   // count
            pUbo->uParams0[1] = 0.5f;   // isoLevel
            pUbo->uParams0[2] = aspect; // aspect
            pUbo->uParams0[3] = 0.0f;   // time（M0 未用）

            pUbo->uPoints[0][0] = 0.0f; // NDC x（屏幕中心）
            pUbo->uPoints[0][1] = 0.0f; // NDC y
            pUbo->uPoints[0][2] = 0.4f; // radius
            pUbo->uPoints[0][3] = 0.0f; // weight（M0 未用）

            mUbo->Unmap();
        }

        // 主 pass（RecordOffscreenPass）末尾把 HDR 留在 ShaderReadOnly（引擎不变量，
        // 且 InsertPass 位于粒子 pass 之后——本 spike 无粒子，布局仍是 ShaderReadOnly）。
        // 自己翻回 ColorAttachment 写一遍，再翻回 ShaderReadOnly，让下游 debug-draw /
        // bloom 接续（与引擎 VfxSystem::DrawParticles 同模式）。RenderPassContext 只给
        // view，经 RHITextureView::GetTexture() 取 texture 做 transition。
        cmd.TransitionTexture(hdrTex, Rhi::TextureLayout::ShaderReadOnly,
                              Rhi::TextureLayout::ColorAttachment);

        Rhi::ColorAttachment att{};
        att.mpView   = hdrView;
        att.mLoadOp  = Rhi::LoadOp::Load; // 保留主 pass 已画的场景，叠加合成
        att.mStoreOp = Rhi::StoreOp::Store;

        Rhi::RenderingDesc rd{};
        rd.mRenderArea.mWidth  = ctx.width;
        rd.mRenderArea.mHeight = ctx.height;
        rd.mColorAttachments.push_back(att);

        cmd.BeginRendering(rd);

        Rhi::RHIViewport vp{};
        vp.mWidth    = static_cast<float>(ctx.width);
        vp.mHeight   = static_cast<float>(ctx.height);
        vp.mMaxDepth = 1.0f;
        cmd.SetViewport(vp);

        Rhi::RHIScissor sc{};
        sc.mWidth  = ctx.width;
        sc.mHeight = ctx.height;
        cmd.SetScissor(sc);

        cmd.BindGraphicsPipeline(*mPipeline);
        cmd.SetDescriptorSet(0, *mSets[slot]); // 非动态 UBO：空 dynamicOffsets，靠 per-slice set 选片
        cmd.Draw(3, 1, 0, 0);                  // big-triangle 覆盖全屏

        cmd.EndRendering();

        cmd.TransitionTexture(hdrTex, Rhi::TextureLayout::ColorAttachment,
                              Rhi::TextureLayout::ShaderReadOnly);
    }

} // namespace spike01
