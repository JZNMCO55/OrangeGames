#include "SlimeMetaballPass.h"

#include <orange/resource/ShaderResource.h>
#include <orange/rhi/RHIBuffer.h>
#include <orange/rhi/RHICommandList.h>
#include <orange/rhi/RHIDescriptor.h>
#include <orange/rhi/RHIDevice.h>
#include <orange/rhi/RHIPipeline.h>
#include <orange/rhi/RHIRendering.h>
#include <orange/rhi/RHITexture.h>

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
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

    void SlimeMetaballPass::SetBlob(const glm::vec2* perimeterWorld, int count,
                                    glm::vec2 centroidWorld, float radius)
    {
        if (perimeterWorld == nullptr || count <= 0)
        {
            mHasBlob = false;
            return;
        }
        // 末尾一格留给 centroid，故 perimeter 最多 kMaxPoints-1 个。
        const int n = std::min(count, kMaxPoints - 1);
        mPerimeterWorld.assign(perimeterWorld, perimeterWorld + n);
        mCentroidWorld = centroidWorld;
        mBlobRadius    = radius;
        mHasBlob       = true;
    }

    void SlimeMetaballPass::SetDroplets(const glm::vec2* pos, const float* radius, int count)
    {
        const int n = (pos != nullptr && radius != nullptr) ? std::min(count, kMaxDroplets) : 0;
        mDropletPos.assign(pos, pos + std::max(0, n));
        mDropletRadius.assign(radius, radius + std::max(0, n));
    }

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
        // M1：无 blob 数据（首帧 SetBlob 之前）就别画——不硬编码占位圆，避免闪一帧中心圆。
        if (!mHasBlob || mPerimeterWorld.empty() || ctx.pViewProjData == nullptr)
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

        const int   n      = static_cast<int>(mPerimeterWorld.size());
        const float aspect = (ctx.height != 0)
                                 ? static_cast<float>(ctx.width) / static_cast<float>(ctx.height)
                                 : 1.0f;

        // CPU 侧算 falloff 半径：投影 blob 到 NDC，取 centroid→perimeter 的平均 aspect-
        // corrected 距离 = blob 屏幕半径（ndc-y 单位）。falloff 随该半径缩放 → 自适应
        // 相机缩放 / blobRadius slider，不用在 pass 里拿 halfH。
        const glm::mat4 viewProj = glm::make_mat4(ctx.pViewProjData);
        auto            projNdc  = [&viewProj](glm::vec2 world)
        {
            const glm::vec4 clip = viewProj * glm::vec4(world, 0.0f, 1.0f);
            const float     w    = (std::abs(clip.w) > 1e-6f) ? clip.w : 1.0f;
            return glm::vec2(clip.x / w, clip.y / w);
        };
        const glm::vec2 centroidNdc = projNdc(mCentroidWorld);
        float           sumR        = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            glm::vec2 d = projNdc(mPerimeterWorld[i]) - centroidNdc;
            d.x *= aspect; // screen-isotropic（同 shader 度量）
            sumR += glm::length(d);
        }
        const float blobNdcRadius = (n > 0) ? (sumR / static_cast<float>(n)) : 0.1f;
        const float falloffNdc    = std::max(1e-3f, mTunables.falloffScale * blobNdcRadius);

        // 写本 slice：viewProj + 14 perimeter + 1 centroid（世界坐标）。
        if (auto* pMapped = static_cast<std::uint8_t*>(mUbo->Map()))
        {
            auto* pUbo = reinterpret_cast<SlimeUbo*>(pMapped + sliceOffset);
            std::memset(pUbo, 0, sizeof(SlimeUbo));
            std::memcpy(pUbo->uViewProj, ctx.pViewProjData, sizeof(float) * 16); // 列主序直拷

            pUbo->uParams0[0] = static_cast<float>(n + 1); // count = perimeter + centroid
            pUbo->uParams0[1] = mTunables.isoLevel;
            pUbo->uParams0[2] = aspect;
            pUbo->uParams0[3] = falloffNdc;

            // M2 材质参数：blobNdcRadius 供 shader 算显式半球，其余是迁移自 Tier1 的 gel 旋钮。
            pUbo->uParams1[0] = blobNdcRadius;
            pUbo->uParams1[1] = mTunables.domeScale;
            pUbo->uParams1[2] = mTunables.rimGain;
            pUbo->uParams1[3] = mTunables.specGain;
            // M3：ambient + 视觉时钟 + 眼/光斑/辉光 emissive 强度。
            pUbo->uParams2[0] = mTunables.ambient;
            pUbo->uParams2[1] = mTime;
            pUbo->uParams2[2] = mTunables.eyeGain;
            pUbo->uParams2[3] = mTunables.speckGain;
            pUbo->uParams3[0] = mTunables.glowGain;

            for (int i = 0; i < n; ++i)
            {
                pUbo->uPoints[i][0] = mPerimeterWorld[i].x;
                pUbo->uPoints[i][1] = mPerimeterWorld[i].y;
            }
            // 末尾一格 = centroid，填实内部（避免 metaball 环状中空）。
            pUbo->uPoints[n][0] = mCentroidWorld.x;
            pUbo->uPoints[n][1] = mCentroidWorld.y;

            // juice droplet：世界半径转 ndc-y（同主体 world→ndc 尺度 × falloffScale，融合行为一致）。
            const float worldToNdc = blobNdcRadius / std::max(mBlobRadius, 1e-4f);
            const int   dropN      = std::min(static_cast<int>(mDropletPos.size()), kMaxDroplets);
            pUbo->uParams3[1]      = static_cast<float>(dropN);
            for (int i = 0; i < dropN; ++i)
            {
                pUbo->uDroplets[i][0] = mDropletPos[i].x;
                pUbo->uDroplets[i][1] = mDropletPos[i].y;
                pUbo->uDroplets[i][2] = std::max(1e-4f, mTunables.falloffScale * mDropletRadius[i] * worldToNdc);
                pUbo->uDroplets[i][3] = 1.0f; // intensity
            }

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
