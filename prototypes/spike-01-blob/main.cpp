// spike-01-blob —— 流体史莱姆 context-sensitive 手感 Spike 1（骨架）
//
// 设计规格：../spike-01-blob-feel.md（执行规格）+ ../phase0-pillars.md（宪法）。
//
// 本文件目前是**可编骨架**，不是手感原型本体。它只回答一个工程问题：
// "OrangeGames 经 find_package(OrangeEngine) 消费引擎，能不能开窗 + 渲染 +
// debug draw？" —— 把这条消费链路立住，后续 Spike 才有地基往上搭。
//
// 当前画面：正交 2D 视角，debug draw 勾出 Spike 1 规格里那一屏场景的轮廓
// （平地 + 两块平台 + 一道窄缝 + 一个边沿），外加一个占位方块代表未来的
// "隐藏 control point / 软体 blob"。
//
// 尚未落地（按规格的优先级，逐项接）：
//   * 调参面板：规格列为"第一优先级地基"，但引擎公共面暂无消费者 ImGui
//     hook（OrangeEngine GAP-2026-05-27-consumer-imgui-tuning-hook）。先补
//     引擎 hook，再回来把旋钮接成 slider。
//   * Loop A（control point 刚体手感）：输入(Action/ActionMap) + Box2D 静态
//     世界 + velocity 驱动的隐藏 control point + coyote/jump-buffer/apex。
//   * Loop B（软体 blob）：自写 Verlet/PBD ~12-20 质点，弹簧吸附 control point。
//   * 判据 overlay：输入瞬间标记 + control point 速度曲线 + control point↔blob
//     质心连线（apex 分叉）+ 10 次落点散布打点。
//
// 注意：运行需要引擎 builtin shader 在 .exe 旁（shaders/orange_engine/*.spv）。
// 引擎 install 暂未装这些（见 CMakeLists.txt 的 ORANGE_ENGINE_SHADER_DIR
// workaround 与对应 GAP）。

#include <orange/engine/app/AppConfig.h>
#include <orange/engine/app/AppHost.h>
#include <orange/engine/app/FrameContext.h>
#include <orange/engine/app/Layer.h>
#include <orange/engine/asset/AssetHandle.h>
#include <orange/engine/asset/AssetRegistry.h>
#include <orange/engine/asset/MeshAsset.h>
#include <orange/engine/asset/ShaderAsset.h>
#include <orange/engine/asset/ShaderLoader.h>
#include <orange/engine/platform/WindowEvent.h>
#include <orange/engine/render/Camera.h>
#include <orange/engine/render/DebugDrawScene.h>
#include <orange/engine/render/LightComponent.h>
#include <orange/engine/render/MaterialInstance.h>
#include <orange/engine/render/MaterialSystem.h>
#include <orange/engine/render/Pipeline.h>
#include <orange/engine/render/RenderableComponent.h>
#include <orange/engine/scene/Entity.h>
#include <orange/engine/scene/TransformComponent.h>
#include <orange/engine/scene/World.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <utility>
#include <vector>

using namespace Orange::Engine;
using Orange::Engine::Asset::AssetHandle;
using Orange::Engine::Asset::AssetRegistry;
using Orange::Engine::Asset::MeshAsset;
using Orange::Engine::Asset::ShaderAsset;
using Orange::Engine::Asset::ShaderLoader;
using Orange::Engine::Asset::VertexPosition3;
using Orange::Engine::Asset::VertexUV2;
using Orange::Engine::Render::Camera;
using Orange::Engine::Render::DebugDrawScene;
using Orange::Engine::Render::DirectionalLight;
using Orange::Engine::Render::MaterialSystem;
using Orange::Engine::Render::Pipeline;
using Orange::Engine::Render::RenderableComponent;
using Orange::Engine::Scene::TransformComponent;

namespace
{

// debug draw 颜色：ABGR packed（低 8 位 R，高 8 位 A）。
constexpr std::uint32_t kGreen = 0xFF00FF00u;  // 地面
constexpr std::uint32_t kCyan  = 0xFFFFFF00u;  // 平台
constexpr std::uint32_t kRed   = 0xFF0000FFu;  // 窄缝两壁
constexpr std::uint32_t kAmber = 0xFF1A78FFu;  // 边沿

// 占位方块：之后被"隐藏 control point + 软体 blob"取代。
std::unique_ptr<MeshAsset> MakeUnitCubeMesh()
{
    constexpr float h = 0.5f;
    std::array<std::array<VertexPosition3, 4>, 6> faces = {{
        {{ { h, -h,  h}, { h, -h, -h}, { h,  h, -h}, { h,  h,  h} }},  // +X
        {{ {-h, -h, -h}, {-h, -h,  h}, {-h,  h,  h}, {-h,  h, -h} }},  // -X
        {{ {-h,  h,  h}, { h,  h,  h}, { h,  h, -h}, {-h,  h, -h} }},  // +Y
        {{ {-h, -h, -h}, { h, -h, -h}, { h, -h,  h}, {-h, -h,  h} }},  // -Y
        {{ {-h, -h,  h}, { h, -h,  h}, { h,  h,  h}, {-h,  h,  h} }},  // +Z
        {{ { h, -h, -h}, {-h, -h, -h}, {-h,  h, -h}, { h,  h, -h} }},  // -Z
    }};

    std::vector<VertexPosition3> positions;
    std::vector<VertexUV2>       uvs;
    std::vector<std::uint32_t>   indices;
    positions.reserve(24);
    uvs.reserve(24);
    indices.reserve(36);
    for (const auto& face : faces)
    {
        const std::uint32_t base = static_cast<std::uint32_t>(positions.size());
        for (int i = 0; i < 4; ++i)
        {
            positions.push_back(face[i]);
            uvs.push_back({0.0f, 0.0f});
        }
        indices.push_back(base + 0);
        indices.push_back(base + 2);
        indices.push_back(base + 1);
        indices.push_back(base + 0);
        indices.push_back(base + 3);
        indices.push_back(base + 2);
    }
    auto pMesh = std::make_unique<MeshAsset>(std::move(positions),
                                             std::move(uvs),
                                             std::move(indices));
    pMesh->ComputeSmoothNormalsFromTriangles();
    return pMesh;
}

class SpikeLayer : public Layer
{
public:
    SpikeLayer(Pipeline& pipeline, World& world, Entity blob)
        : Layer("SpikeLayer"), mPipeline(pipeline), mWorld(world), mBlob(blob)
    {
    }

    void OnUpdate(const FrameContext& /*frame*/) override
    {
        // TODO(Loop A)：读输入驱动隐藏 control point（velocity 驱动）+ 跳跃 /
        // coyote / jump buffer / apex；这里先让占位方块静止。
        // TODO(Loop B)：软体 blob 弹簧吸附 control point。

        if (auto* dbg = mPipeline.GetDebugDrawScene())
        {
            DrawLevelOutline(*dbg);
        }
        mPipeline.Render(mWorld);
    }

    bool OnEvent(const Platform::WindowEvent& event) override
    {
        if (auto* resize = std::get_if<Platform::WindowResizeEvent>(&event))
        {
            mPipeline.OnResize(resize->width, resize->height);
        }
        return false;
    }

private:
    // Spike 1 规格里那一屏场景的轮廓：平地 + 两块平台 + 一道窄缝 + 一个边沿。
    // 全部画在 z=0 的 XY 平面上，正交相机正对。后续这些 debug 线会被换成
    // Box2D 静态碰撞体（线只作可视参照）。
    static void DrawLevelOutline(DebugDrawScene& dbg)
    {
        auto seg = [&dbg](glm::vec2 a, glm::vec2 b, std::uint32_t c)
        {
            dbg.AddLine(glm::vec3(a, 0.0f), glm::vec3(b, 0.0f), c);
        };

        // 平地：y = -3，横跨视野。
        seg({-7.5f, -3.0f}, {7.5f, -3.0f}, kGreen);
        // 左平台。
        seg({-6.0f, -0.5f}, {-2.5f, -0.5f}, kCyan);
        // 右平台 + 边沿（右端是一个可挂的 ledge）。
        seg({2.5f, 0.5f}, {6.0f, 0.5f}, kCyan);
        seg({6.0f, 0.5f}, {6.0f, -0.5f}, kAmber);
        // 窄缝：两道竖壁，留一条刚够 blob 变形钻过的缝。
        seg({-0.4f, -3.0f}, {-0.4f, 1.0f}, kRed);
        seg({0.4f, -3.0f}, {0.4f, 1.0f}, kRed);
    }

    Pipeline& mPipeline;
    World&    mWorld;
    Entity    mBlob;
};

}  // namespace

int main()
{
    AppConfig cfg{};
    cfg.window.title  = "OrangeGames - spike-01-blob (skeleton)";
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

    auto cubeRes = assets.Insert<MeshAsset>("builtin/cube", MakeUnitCubeMesh());
    if (cubeRes.IsErr())
    {
        std::fprintf(stderr, "Insert<MeshAsset> failed\n");
        return 1;
    }
    AssetHandle<MeshAsset> cubeHandle = cubeRes.Value();

    MaterialSystem materials(assets);
    if (auto rb = materials.RegisterBuiltins(); rb.IsErr())
    {
        std::fprintf(stderr, "RegisterBuiltins failed\n");
        return 1;
    }
    auto blobInstance = materials.CreateInstance("toon");
    if (!blobInstance)
    {
        std::fprintf(stderr, "CreateInstance(\"toon\") returned null\n");
        return 1;
    }

    World world;

    // 占位 blob：原点附近一个小方块，未来被隐藏 control point + 软体 blob 取代。
    Entity blobEntity = world.CreateEntity();
    {
        TransformComponent xf{};
        xf.position = {0.0f, -2.0f, 0.0f};
        world.AddComponent(blobEntity, xf);
        RenderableComponent r;
        r.mesh             = cubeHandle;
        r.materialInstance = blobInstance.get();
        world.AddComponent(blobEntity, r);
    }

    // 主光：稍微侧上方，给占位方块一点 toon 分段。
    Entity lightEntity = world.CreateEntity();
    {
        TransformComponent lightXf{};
        lightXf.rotation = Orange::Engine::Render::MakeDirectionalLightRotationFromDir(
            glm::vec3(0.2f, -0.6f, -1.0f));
        world.AddComponent(lightEntity, lightXf);
        DirectionalLight dl{};
        dl.color     = glm::vec3(1.0f, 0.95f, 0.85f);
        dl.intensity = 1.1f;
        world.AddComponent(lightEntity, dl);
    }

    // 正交 2D 相机：一屏覆盖约 16×9 世界单位，正对 XY 平面（沿 -Z 看）。
    // 平台跳跃的最终视角；3D 透视留给未来真要 2.5D 时再换。
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

    Pipeline pipeline;
    if (auto r = pipeline.Initialize(host->GetWindow(), assets); r.IsErr())
    {
        std::fprintf(stderr, "Pipeline::Initialize failed (code=%u)\n",
                     static_cast<unsigned>(r.Error()));
        return 1;
    }
    pipeline.SetMaterialSystem(&materials);

    host->PushLayer(std::make_unique<SpikeLayer>(pipeline, world, blobEntity));

    const int rc = host->Run();
    pipeline.Shutdown();
    return rc;
}
