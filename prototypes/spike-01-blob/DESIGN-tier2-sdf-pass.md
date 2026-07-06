# Tier 2 —— 逐像素 SDF metaball 史莱姆渲染（实现规格）

> 状态：**设计定稿，未开工**。本文件是 Tier 2「逐像素 SDF fragment pass」的施工级规格。
> 记录日期：2026-07-06。归属：OrangeGames 首游 Spike 1 · 实体渲染阶段 Tier 2。
> 上位蓝图：[`DESIGN-solid-render-phase.md`](DESIGN-solid-render-phase.md)（§1-§3 的架构/SDF 数学/材质表仍权威）。
> 本文件**纠正**上位蓝图的 §5 / §6 / §10 —— 那里"引擎缺口卡死逐像素路线"的判断基于旧的通读，2026-07-06 两个 agent 重新核实 OR/OE 渲染面后**已推翻**（见 §2）。

---

## 0. 为什么开这份文档

2026-07-06 演示 Tier 1（CPU 逐面平涂 gel，OG main `ca6b9eb`）时，用户当场判两处**不达标**：

1. **画质差参考图远**：CPU 逐面单色平涂 → 剪影有棱、高光糊成一团白、接触辉光是块突兀的纯绿椭圆贴片、无折射/无真 SSS。根因在 `main.cpp` 的 `EmitSlimeGel`/`ShadeGel`（每径向条带一个 `AddTriangle` 单色）+ `silhouetteSub=3`（撞 DebugDraw 4096 顶点上限）。
2. **移动/跳跃全程一个状态**：这是**形变没做**，非渲染问题——见 §9（本文件不解决形变，只解决渲染）。

Tier 1 本质是被"引擎缺口"逼退的 CPU 替身，天花板就在那。用户拍板走 **Tier 2 正路：逐像素 SDF/metaball fragment shader**。本文件把这条路落到可施工。

---

## 2. 引擎能力核实结论（2026-07-06，推翻旧判断）

两个 agent 通读 `OrangeRender` + `OrangeEngine` 渲染面，逐条给结论：

| 能力 | 结论 | 证据 |
|---|---|---|
| 自定义 graphics pipeline + 任意 fs/vs | ✅ OR 公共 API 全覆盖 | `RHIDevice::CreateGraphicsPipeline`、`samples/particle_field`+`offscreen_to_screen` |
| 传 15 质点数组进 shader | ✅ 走 **UBO**（push constant 128B 保底塞不下 180B） | `BufferUsage::Uniform` + descriptor 四件套 + `Map/Unmap`(CpuToGpu) |
| 游戏侧注入自定义 fullscreen pass | ✅ **这是 Tier 2 正路** | `Pipeline::InsertPass(AfterMainPass,…)` / `SetAuxPassProvider` 均已接通 |
| 逐帧动态 mesh 顶点更新 | ❌ 不需要（我们不走 mesh） | mesh 按 handle 缓存 / GpuOnly / 永不重传（`Pipeline.cpp:2448`） |
| 高层 Material + 自定义 uniform 数组 | ❌ 不需要（我们不走 Material） | 缺 per-instance Material UBO（引擎 G1 大工程）+ uniform 无数组类型 |
| DebugDraw 4096 顶点上限 | 走 pass 后 gel 不再用 DebugDraw，**规避** | `DebugDrawScene.cpp:162` |

**净结论**：Tier 2 **纯 OG 单 session 就能做，OrangeRender / OrangeEngine 零源码改动**。唯一"缺口"是旧判断的误读——引擎早就为外部自定义 pass 开了正门（`IRenderPass` / `IAuxPassProvider`），只是 Tier 1 那次没走。

---

## 3. 架构选型

### 3.1 渲染注入路径 —— 三选一

| 路径 | 可行性 | 结论 |
|---|---|---|
| A. 高层 `Material` 挂自定义 fragment shader | ❌ 传不进质点数组（无数组 uniform + push-constant hardcode 三名） | 弃 |
| B. 逐帧动态 mesh 变形 + 表面 shader | ❌ mesh GpuOnly 永不重传 | 弃 |
| C. **自定义 fullscreen fragment pass**（自建 pipeline+UBO+SDF shader） | ✅ 全部现成 API + 官方样例 | **选定** |

路径 C 一次解决上位蓝图 §2 决策 B 列的四件事：剪影平滑 + 材质着色 + 假 3D 圆润 + 分裂/合并（SDF `smin` 天生支持）。

### 3.2 注入 hook —— `InsertPass(AfterMainPass)`（主）vs `IAuxPassProvider`（备）

两条都在"主 pass+粒子已写完 HDR、bloom 还没跑"这个 hook 点，语义都对。**两条都要 pass 自己 `BeginRendering`**（读头确认：InsertPass 的 `pHdrColorView` 已是 ColorAttachment layout、pass 自己 Begin/Bind/Draw/End；AuxProvider 进来是 ShaderReadOnly、pass 自己 transition 到 ColorAttachment 再 Begin）。差异：

| | `InsertPass(AfterMainPass)` ★推荐 | `IAuxPassProvider` |
|---|---|---|
| **FIF 帧索引**（选 UBO slice） | ✅ `frameIndex`（注释即"ring-buffer 索引"） | ❌ 无字段 → provider 自己数帧 |
| HDR 句柄 | `pHdrColorView` = `RHITextureView*`（直接 BeginRendering） | `pHdrColor` = `RHITexture*`（需自建 view） |
| descriptor pool | `pSharedPool`（可能为 null，仍建议自建） | 无字段 → **必自建池** |
| slot 数 | 同 stage **多 pass** | **单 provider**（第二个覆盖前者；编辑器 grid 占同 slot，spike 不跑编辑器故 OK） |
| 全屏顶点 shader | 自带 `fullscreen.vert`（14 行，附录 §12.5 有全文） | **白送** `pFullscreenVs`（引擎已编 big-triangle） |
| HDR 目标格式 | 从注入目标查/确认（风险 §8.3） | **白送** `hdrColorFormat`（建 PSO 不 hardcode） |
| viewProj | `pViewProjData`（裸 `const float*`，16 float 列主序） | `viewProj`/`invViewProj`（`glm::mat4`） |
| depth 采样 | `pSceneDepthView` | `pSceneDepth` + `pHdrSampler` + `sceneDepthIsShaderReadOnly` |

**选 `InsertPass(AfterMainPass)`**：给 `pHdrColorView`（免自建 view）+ `frameIndex`（免自己数帧做 FIF 环索引，这是每帧更新 UBO 的刚需）+ 多 pass。代价只是自带 14 行 `fullscreen.vert`（附录有全文，微不足道）和确认 HDR 格式（风险 §8.3）。`pipeline.InsertPass(PipelineStage::AfterMainPass, std::make_unique<SlimeMetaballPass>(...))` 接入。

> AuxProvider 白送的 `pFullscreenVs`/`hdrColorFormat` 便利，抵不过 `frameIndex`+`view` 的省事。两者 pass 内部实现（pipeline/UBO/draw/shader）几乎一致，若日后要 depth 采样折射或想省 vert，切换成本低。

---

## 4. 数据流 / UBO schema

每帧把 blob 的 15 质点（世界坐标）灌进一个 UBO，fragment shader 逐像素读它求密度场。

### 4.1 坐标约定

- blob 质点是**世界坐标**（`Blob::Positions()`，glm::vec2）。
- fragment shader 工作在**裁剪/屏幕空间**。两条办法：
  - **(a) 传世界坐标 + viewProj**：shader 里对每个质点做 `viewProj * vec4(p,0,1)` 投到 NDC，再和当前像素 NDC 比。质点少（15）投影开销可忽略。**选这个**（数值稳、跟相机走）。
  - (b) CPU 端预投影成 NDC 再传：省 shader 投影但 CPU 多算。质点太少不值当。

### 4.2 UBO 布局（std140，逐质点 16B 对齐）

std140 下数组元素对齐到 16B，故每质点用一个 `vec4` 装满，别用 `vec2`（会被 padding 浪费且易错位）：

```glsl
// slime_metaball.ubo —— set=0, binding=0，fragment stage
layout(std140, set = 0, binding = 0) uniform SlimeUBO {
    mat4  uViewProj;      // 主相机 viewProj（世界→clip）
    vec4  uParams0;       // x=pointCount, y=blobRadius(u), z=isoLevel, w=time
    vec4  uParams1;       // x=sminK(合并圆角), y=rimPow, z=specPow, w=alphaEdge
    vec4  uCentroidAxis;  // xy=质心(世界), zw=主轴方向(单位向量,眼睛/UV 帧)
    vec4  uPoints[16];    // 每个: xy=质点世界坐标, z=该点半径, w=权重(水滴/碎块用)
} ubo;
```

- 15 质点 + 1 备用（分裂时临时第二团的水滴可占 `uPoints[15]` 或扩容）。`uParams0.x` 传真实 count。
- 颜色/光照常量：先按上位蓝图 §3.3 的值 **hardcode 在 shader**（少一层传参），调好后再决定要不要提成 UBO 字段走 ImGui live 调（Tier 1 的 `SlimeParams` slider 体验值得保留 → 大概率提上来）。

### 4.3 每帧更新

`Execute`/`RenderAuxPass` 里，从 blob 拿 15 质点 → 写进当前 frame-in-flight 的 UBO 分片（`frameIndex % FIF`）→ `Map/写/Unmap`（CpuToGpu 内存）。绝不复用上一帧还在 GPU 用的分片（用 `frameIndex` 或 FIF 环索引避 hazard，抄 `particle_field` 的分片法）。

---

## 5. SDF fragment shader 分解

沿用上位蓝图 §3 的数学（此处只标"在 pass 里怎么落"）。逐像素：

1. **像素世界坐标**：从 NDC + `invViewProj` 反投到世界 XY 平面（2.5D，z=0 平面）。或反过来把质点投到屏幕、在屏幕空间求距离——二选一，选世界空间（`smin` 半径单位一致好调）。
2. **密度场 / SDF**：
   - **主体** = 对 14 点闭合 perimeter 多边形求 signed distance `d(x)`（点到线段集合最短距离带内外号），再按小半径 ρ 膨胀 → 棱角圆成胶质弧面。**保住 sim 的 area 约束形状**。
   - **水滴/碎块** = 额外圆形 SDF（`uPoints[].w>0` 的点），`smin(d_main, d_drop, uParams1.x)` 平滑并入 → 靠近自然连、飞离自然断（状态 6/11/12 免特判）。
   - **备选纯 metaball**：15 质点各当场心 `Σ falloff(r_i)` 阈值化（抄 `particle_field.frag` 的逐像素 falloff 累加）——更 blobby 但剪影不由环直接控。默认多边形 SDF，纯 metaball 留 A/B 对比开关。
3. **剪影**：`isoLevel` 阈值 + `smoothstep` 抗锯齿边；`d>0` 区域 `discard`/alpha=0。
4. **假 3D 法线**（roundness）：穹顶高度 `h=sqrt(R²-d²)` 近似；`N.xy=∇d`(2D 梯度，有限差分)、`N.z=sqrt(1-|N.xy|²)`；视线 `V≈+z`。
5. **gel 着色**（同一 shader 叠加，对应上位蓝图 §3.3 材质表）：
   - Fresnel 亮边 `pow(1-N.z, rimPow)` × 黄绿；
   - 假 SSS：厚度 `|d|`/穹顶高度 驱动 wrap-diffuse（薄处亮体色、厚处深绿）；
   - 湿润高光：Blinn/GGX（主光在上方）；
   - 发光眼（两椭圆，emissive>1 喂 bloom）：在 blob 局部帧（`uCentroidAxis`）定义 UV，随体拉伸/压扁；
   - 体内光斑：局部 UV 散布的 emissive 点（Tier 1 的 14 确定性点搬进 shader，逐像素明灭）；
   - 接触辉光：接触点下方 additive 径向（可 shader 内按最低点算，或留给 VfxSystem）；
   - **alpha**：核心边深、边缘透（输出 alpha<1）；pipeline 开 **alpha blend** 叠到 HDR（自定义 pass 不受 `pbr.frag` 写死 alpha=1 限制）。
6. **输出**：linear HDR RGB + alpha，alpha-blend 进 `pHdrColor`，接现有 bloom（threshold/intensity 沿用 Tier 1 值）。

---

## 6. OG 工程改动

### 6.1 接 OrangeRender RHI 依赖

`prototypes/spike-01-blob/CMakeLists.txt` 现在只 `target_link_libraries(... OrangeEngine::orange_engine)`。需加：

```cmake
find_package(OrangeRender CONFIG REQUIRED)   # SDK 默认 D:/sdk/orange-render（CMAKE_PREFIX_PATH）
target_link_libraries(spike-01-blob PRIVATE
    OrangeEngine::orange_engine
    OrangeRender::orange_render)             # ← pass 实现要 include <orange/rhi/...>
```

pass 的 `.cpp` 里 `#include <orange/rhi/...>`（RHIDevice/Pipeline/Buffer/Descriptor/CommandList）。**注意**：这让游戏代码"下沉到 RHI 级"，是引擎明确开放的逃生舱口（`IRenderPass.h` 注释点名水面/屏幕扭曲/overlay），架构上正当。

### 6.2 SDF shader 编译 + 部署

- 写 `shaders/slime_metaball.frag` + `shaders/fullscreen.vert`（附录 §12.5 有全文，InsertPass 路径要自带 vert）→ build 期 `glslangValidator` 编成 `.spv`（附录 §12.6 有 CMake 模板）。
- `.spv` 部署二选一：
  - **(a) 起步最快**：POST_BUILD copy 到 `.exe` 旁 + 运行时 `ShaderResource::CreateFromFile("shaders/xxx.spv")`（同 OR 样例，附录 §12.7）。
  - **(b) 后续优化**：EmbedSpv 把 spv 编进头 + `CreateShaderModule(embeddedBlob)`（免运行时找文件，对引擎内注入 pass 更稳）。

### 6.3 SDK 前置验证（开工第一步）

`find_package(OrangeRender)` 要求 OR SDK 已 install（`D:/sdk/orange-render`）。开工先验证 SDK 在位且版本匹配当前 umbrella pointer；不在则先 `build_all.py` 或单独 install OR。（memory 记 SDK 近期重装过，需实测确认。）

---

## 7. 退休 Tier 1

- `main.cpp` 的 `EmitSlimeGel` / `ShadeGel` / `FillEllipse` / `SlimeParams` 的 CPU 平涂路径：Tier 2 上屏验证达标后**移除**，或降级为 `#ifdef` 对照。
- 保留 `wireframe` 开关（画 perimeter loop + 辐条）作 sim debug 对照——它和渲染路线正交，仍有用。
- `beauty` 开关语义不变（隐藏关卡线框 + overlay）。
- 背景设置（`SetSkyEnabled(false)` + `SetSceneClearColor` + bloom 阈值）**保留**，Tier 2 直接复用。

---

## 8. 风险 / 未知数（开工时逐个消解）

**三个 must-confirm-with-engine（agent 提示，M0 首先实测落定）**：

1. **注入回调是否已 `BeginRendering`**：读头判断走 `InsertPass` 时 `pCmdList`=offscreenCmd、`pHdrColorView` 已 ColorAttachment layout，pass **自己** `BeginRendering(pHdrColorView)`/Bind/Draw/`EndRendering`（附录 §12.4）。M0 首帧确认这假设——若引擎已开好 rendering，去掉自己的 Begin/End、只留 bind+draw。
2. **FIF slot 索引**：用 `RenderPassContext.frameIndex % kFramesInFlight` 选 UBO slice（`frameIndex` 注释即"ring-buffer 索引"）。确认注入点在引擎 per-frame 同步之后（GPU 已释放该 slot），否则写正被 GPU 读的内存 → 撕裂。
3. **注入目标 HDR color 的确切 `TextureFormat`**：pipeline `mColorFormats` 必须与之逐位兼容（dynamic rendering 才不报错）。候选 `RGBA16Float`/`R11G11B10Float`（`RHITypes.h:64-74`）；从注入目标 RT desc 查证，别 hardcode。

**其余**：
4. **alpha blend 字段**：`ColorBlendAttachmentDesc` 填 `mBlendEnable=true` + src=`SrcAlpha`/dst=`OneMinusSrcAlpha`（覆盖式 over，gel 主体用）或 dst=`One`（叠加，接触辉光/光斑另开 draw 用）+ op=`Add`（附录 §12.1 全字段）。
5. **descriptor pool 生命周期**：pass 自建池，在 pass 对象里长寿命持有（`Setup` 建、`Resize` 刷），**别每帧建**。
6. **HDR 是 pre-tonemap linear**：着色算 linear、值可 >1（喂 bloom）。别在 shader 里 tonemap（下游 tonemap pass 管）。
7. **多边形 SDF 逐像素成本**：每像素对 14 段线求距离。屏幕不大 + blob 占屏小可接受；若慢，裁剪到 blob 屏幕包围盒（scissor / discard 早退）。
8. **只依赖公共 `orange_render`**：`particle_field` 只链公共 SDK（可 copy-paste 对已装 SDK 编）；**绝不**链 `orange_render_internals` / 碰 `backend/vulkan/*` 私有头（`offscreen_to_screen` 链 internals 是因它 headless 手动构造 device，我们注入路径不需要）。

---

## 9. 与形变 / 12 状态的关系（本文件不做）

Tier 2 只解决**渲染**（把当前 blob 形状渲成 gel）。用户演示逮的"移动/跳跃全程一个状态"是**形变没做**——`Blob.h` 的 `restOffset` 写死正圆 + 强 PBD 圆约束，12 状态姿态一个没实现（Blob.h 注释自认留 Step B）。

形变是**并行的另一块、纯 game 侧**（改 `Blob.h` 的 `restOffset`/`stiffness` 按 grounded/velocity/蓄力驱动下压/拉伸/落地铺开），**不依赖 Tier 2**，也不被 Tier 2 阻塞。两者可任意先后：

- 先 Tier 2：史莱姆变好看但仍是圆球晃动；
- 先形变：史莱姆有姿态但仍是 CPU 平涂糊；
- 二者叠加才是参考图那种"有姿态的漂亮 gel"。

形变的逐状态映射见上位蓝图 §4（`stiffness(state)` 查表 / 落地溅射 / stranding leash / squeeze），本文件不重复。

---

## 10. 分期验收（先打通管线，再堆效果）

沿用"先 baseline 后加档"：

1. **M0 管线打通**（最高风险，先做）：`InsertPass(AfterMainPass)` 注入 → fullscreen pass → UBO 传 1 个质点 → SDF 出**一个平滑圆**alpha-blend 上屏。**只证整条 RHI 管线在本引擎跑通**（pipeline/descriptor/UBO/blend/layout 全对 + §8 三个 must-confirm 实测落定），不追求好看。
2. **M1 剪影**：15 质点 → 多边形 SDF + smin → 平滑连续剪影（对比 Tier 1 的棱角），状态 1-10 跑起来剪影不见棱。
3. **M2 假 3D + 基础材质**：假法线打光 + 绿半透明 + Fresnel 亮边 + 假 SSS 厚度 + 湿润高光。对齐参考图基础质感。
4. **M3 juice**：发光眼（局部帧 UV）+ 体内光斑 + 接触辉光 + caustics 流动 + 分裂/合并 smin 过渡。
5. **M4 收尾**：退休 Tier 1；ImGui 把关键参数提成 UBO 字段 live 调；真机 dogfood 对参考图神似度。

每步真机截图验证（windows-mcp Screenshot 截运行窗口；键盘进不去 GLFW 窗口，手感项用户手玩）。

---

## 11. session 归属

- **本 Tier 2 = 纯 OrangeGames 单 session**（renderer/engine 零源码改动）。不需要上位蓝图 §6 设想的前置"Session E(引擎补 alphaBlend)"——alpha blend 在自定义 pass 的 pipeline 里自己设，不经引擎 Material。
- 撞上引擎/渲染器**真缺口**（如某 RHI API 缺失）时按纪律登记（`OrangeRender/docs/incoming_feature.md` / `OrangeEngine/docs/engine-known-gaps.md`，纯文档跨仓豁免），**不**同 session 改引擎代码。
- 本设计文档 + 上位蓝图均纯文档（ADR-010 豁免），任意 session 可写/改。

---

## 12. 实现骨架附录（可照抄的 OR 样例代码）

> 均来自 `OrangeRender/samples/`，2026-07-06 agent 精读 + 交叉核对 RHI 公共头确认签名。API 若与实际不符以运行时为准。

### 12.0 两样例分工

- **`offscreen_to_screen` = 史莱姆 pass 主骨架**：它就是 consumer 下发的 fullscreen fragment pass（自建 pipeline + descriptor + big-triangle 全屏 draw + layout transition）。史莱姆直接照抄它的 pass 2，把"采样 sceneColor 的 `CombinedImageSampler`"换成"读 15 质点的 `UniformBuffer`"。
- **`particle_field` 提供两块料**：① 每帧流式 buffer + FIF 分片的完整写法（史莱姆 UBO 每帧更新必须照它，否则写 GPU 正读的内存）；② 单质点 falloff 数学。
- ⚠️ **关键区别**：`particle_field` 是**每质点一个 billboard quad + 硬件叠加混合**跨多次 draw 累积密度；史莱姆是**单次全屏 draw、在 fragment shader 里对 15 个质点 for 循环累加**。故 `particle_field.frag` 只当**单质点 falloff 数学参考**，累加循环要自己在 shader 写（读 UBO 数组）。

### 12.1 自定义 pipeline（空顶点输入 + 深度关 + alpha blend）

结构照抄 `offscreen_to_screen/main.cpp:190-202`（**没有** push `mVertexInput.mBindings` = 顶点靠 `gl_VertexIndex` 生成）：

```cpp
Rhi::GraphicsPipelineDesc gp{};
gp.mShaderStages.push_back({Rhi::ShaderStage::Vertex,   pFullVsMod->GetModule(), "main"});
gp.mShaderStages.push_back({Rhi::ShaderStage::Fragment, pSlimeFsMod->GetModule(), "main"});
gp.mRasterizer.mCullMode           = Rhi::CullMode::None;  // 无 vertex buffer，绕开 winding
gp.mDepthStencil.mDepthTestEnable  = false;
gp.mDepthStencil.mDepthWriteEnable = false;
gp.mDescriptorSetLayouts.push_back(pLayout.get());        // 史莱姆 UBO layout
gp.mRenderTargets.mColorFormats.push_back(/* 注入目标 HDR 格式，见 §8.3 */);
// mInputAssembly.mTopology 默认 TriangleList（RHIPipeline.h:109），不用设
```

alpha-blend attachment 用 `particle_field/main.cpp:278-291`（史莱姆 gel 主体把 dst 换成 `OneMinusSrcAlpha` 做覆盖式 over）：

```cpp
Rhi::ColorBlendAttachmentDesc blend{};
blend.mBlendEnable         = true;
blend.mSrcColorBlendFactor = Rhi::BlendFactor::SrcAlpha;
blend.mDstColorBlendFactor = Rhi::BlendFactor::OneMinusSrcAlpha; // gel over；接触辉光/光斑另开 draw 用 One(叠加)
blend.mColorBlendOp        = Rhi::BlendOp::Add;
blend.mSrcAlphaBlendFactor = Rhi::BlendFactor::One;
blend.mDstAlphaBlendFactor = Rhi::BlendFactor::One;
blend.mAlphaBlendOp        = Rhi::BlendOp::Add;
blend.mColorWriteMask      = Rhi::ColorWriteMask::All;
gp.mColorBlend.mAttachments.push_back(blend);
auto pSlimePipeline = device.CreateGraphicsPipeline(gp);
```

### 12.2 descriptor 四件套（UBO binding，fragment stage）

`offscreen_to_screen/main.cpp:153-175` 模板，类型从 `CombinedImageSampler` 换 `UniformBuffer`、write 从 `mImageInfo` 换 `mBufferInfo`（据 `RHIDescriptor.h:20-72`）：

```cpp
Rhi::DescriptorSetLayoutDesc layoutDesc{};
layoutDesc.mBindings = {
    {0, Rhi::DescriptorType::UniformBuffer, 1, Rhi::ShaderStage::Fragment},
};
auto pLayout = device.CreateDescriptorSetLayout(layoutDesc);

Rhi::DescriptorPoolDesc poolDesc{};
poolDesc.mMaxSets   = 1;
poolDesc.mPoolSizes = {{Rhi::DescriptorType::UniformBuffer, 1}};
auto pPool = device.CreateDescriptorPool(poolDesc);

auto pSet = device.AllocateDescriptorSet(*pPool, *pLayout);

Rhi::DescriptorWrite write{};
write.mBinding             = 0;
write.mType                = Rhi::DescriptorType::UniformBuffer;
write.mBufferInfo.mpBuffer = pMetaballUbo.get();
write.mBufferInfo.mOffset  = 0;                        // 用动态偏移时首绑 0
write.mBufferInfo.mRange   = sizeof(SlimeUBO);         // 单 slice 大小
device.UpdateDescriptorSet(*pSet, &write, 1);
```

> `DescriptorBindingDesc` 第 4 字段是 `ShaderStageFlags`（`RHIPipeline.h:23`），单个 `ShaderStage::Fragment` 隐式转即可。**FIF 配合选动态 UBO**：descriptor 只 update 一次，每帧靠 `SetDescriptorSet` 的 `dynamicOffsets` 指到当前 slice（见 §12.4），比每帧 `UpdateDescriptorSet` 省。

### 12.3 每帧更新 UBO + FIF 分片

创建（`particle_field/main.cpp:372-380`，一整块切 `kFramesInFlight` 份，`mUsage` 换 `Uniform`）：

```cpp
constexpr uint64_t kSliceBytes  = sizeof(SlimeUBO);            // std140 对齐后，每质点 vec4 打包
constexpr uint64_t kBufferBytes = kSliceBytes * kFramesInFlight;
Rhi::BufferDesc ubDesc{};
ubDesc.mSize        = kBufferBytes;
ubDesc.mUsage       = Rhi::BufferUsage::Uniform;
ubDesc.mMemoryUsage = Rhi::MemoryUsage::CpuToGpu;
auto pMetaballUbo   = device.CreateBuffer(ubDesc);
```

每帧算 slice 偏移 + Map/写/Unmap（`particle_field/main.cpp:428-443`，`RHIBuffer.h:9-16`）：

```cpp
const uint32_t fifSlot     = static_cast<uint32_t>(ctx.frameIndex % kFramesInFlight); // ← RenderPassContext.frameIndex
const uint64_t sliceOffset = static_cast<uint64_t>(fifSlot) * kSliceBytes;

auto* pMapped = static_cast<uint8_t*>(pMetaballUbo->Map());
auto* pSlot   = reinterpret_cast<SlimeUBO*>(pMapped + sliceOffset);
PackBlobIntoUBO(*pSlot, blob, viewProj, /* params */);  // 15 质点 + viewProj + 参数
pMetaballUbo->Unmap();
```

### 12.4 fullscreen draw 录制 + transition

`offscreen_to_screen/main.cpp:276-311`（命令签名：`RHICommandList.h` Draw@129 / SetDescriptorSet@194 / BindGraphicsPipeline@126）：

```cpp
pCmd->BeginRendering(rd);   // rd 绑 ctx.pHdrColorView 作 color attachment（走 InsertPass 已 ColorAttachment layout）

Rhi::RHIViewport vp{}; vp.mWidth = (float)w; vp.mHeight = (float)h; vp.mMaxDepth = 1.0f;
pCmd->SetViewport(vp);
Rhi::RHIScissor sc{}; sc.mWidth = w; sc.mHeight = h;
pCmd->SetScissor(sc);

pCmd->BindGraphicsPipeline(*pSlimePipeline);
pCmd->SetDescriptorSet(0, *pSet, {static_cast<uint32_t>(sliceOffset)}); // ← 动态偏移选 FIF slice
pCmd->Draw(3, 1, 0, 0);     // big-triangle 覆盖 [-1,1]^2

pCmd->EndRendering();
```

transition（`TransitionTexture(tex, from, to)` @ `RHICommandList.h:218`）：走 `InsertPass` 读 UBO 不采样纹理，**不需要** ShaderReadOnly transition；仅当要采样主 pass 颜色做折射/融背景时才加一次 `ColorAttachment→ShaderReadOnly`。若改走 `IAuxPassProvider`，则必须遵守其"进 ShaderReadOnly→自己 transition ColorAttachment→离开回 ShaderReadOnly"契约。

### 12.5 shader

`fullscreen.vert` 全文（`offscreen_to_screen/shaders/fullscreen.vert`，big-triangle，史莱姆**原样用**）：

```glsl
#version 450
layout(location = 0) out vec2 vUv;
void main()
{
    vec2 pos = vec2((gl_VertexIndex == 1) ?  3.0 : -1.0,
                    (gl_VertexIndex == 2) ?  3.0 : -1.0);
    vUv = (pos + vec2(1.0)) * 0.5;
    gl_Position = vec4(pos, 0.0, 1.0);
}
```

单质点 falloff 数学（`particle_field/shaders/particle_field.frag:14-33`，二次衰减 `(1-r)²`）就是史莱姆每质点的密度核。史莱姆 `slime_metaball.frag` 把"每质点一次 draw"改成"单 draw 内 for 循环累加"（纯 metaball 版骨架，多边形 SDF 版按 §5 换密度函数）：

```glsl
#version 450
layout(location = 0) in  vec2 vUv;
layout(location = 0) out vec4 outColor;
layout(std140, set = 0, binding = 0) uniform SlimeUBO {
    mat4 uViewProj;
    vec4 uParams0;      // x=count, y=blobRadius, z=isoLevel, w=time
    vec4 uCentroidAxis;
    vec4 uPoints[16];   // xy=世界坐标, z=radius, w=weight
} U;
void main() {
    int count = int(U.uParams0.x);
    float field = 0.0;
    for (int i = 0; i < count; ++i) {
        // 世界坐标 → clip → NDC（与像素 NDC 同系）；或反投像素到世界后比距离，二选一
        vec4 clip = U.uViewProj * vec4(U.uPoints[i].xy, 0.0, 1.0);
        vec2 ndc  = clip.xy / clip.w;
        float r   = length((vUv * 2.0 - 1.0) - ndc) / U.uPoints[i].z;
        if (r < 1.0) { float a = 1.0 - r; field += a * a; }   // ← 复用 particle_field falloff
    }
    float mask = smoothstep(U.uParams0.z - 0.05, U.uParams0.z, field);
    // TODO M2+：∇field 造假法线 → Fresnel/SSS/高光/眼/光斑（§5）
    outColor = vec4(vec3(mask), mask);
}
```

> `particle_field.frag` 的 UV 是 quad 局部 `[-1,1]`，史莱姆用全屏 `[0,1]`（`fullscreen.vert` 的 `vUv`）；质点坐标要换算到同一系。UBO `vec4 uPoints[N]` 打包（非 `vec2`）是避 std140 数组 stride 陷阱。

### 12.6 build 期 GLSL→SPIR-V

`particle_field/CMakeLists.txt:17-36`（glslangValidator POST_BUILD；`offscreen_to_screen` 结构相同）：

```cmake
find_program(SLIME_GLSLANG NAMES glslangValidator HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin")
if (NOT SLIME_GLSLANG)
    message(FATAL_ERROR "spike-01-blob: glslangValidator not found under VULKAN_SDK.")
endif ()
add_custom_command(TARGET spike-01-blob POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:spike-01-blob>/shaders"
    COMMAND "${SLIME_GLSLANG}" -V "${CMAKE_CURRENT_SOURCE_DIR}/shaders/fullscreen.vert"
            -o "$<TARGET_FILE_DIR:spike-01-blob>/shaders/fullscreen.vert.spv"
    COMMAND "${SLIME_GLSLANG}" -V "${CMAKE_CURRENT_SOURCE_DIR}/shaders/slime_metaball.frag"
            -o "$<TARGET_FILE_DIR:spike-01-blob>/shaders/slime_metaball.frag.spv"
    VERBATIM COMMENT "Compiling slime_metaball shaders")
```

> `particle_field` 只 `PRIVATE orange_render`（纯公共 SDK）；史莱姆同款，**绝不**链 `orange_render_internals`。运行时靠工作目录相对路径找 `.spv`（`VS_DEBUGGER_WORKING_DIRECTORY` / 现有 `ORANGE_ENGINE_SHADER_DIR` copy 机制）。

### 12.7 shader 模块加载

高层路径（推荐，`particle_field/main.cpp:353-366`，`ShaderResource.h:42-47`）：

```cpp
auto pFullVsMod  = Resource::ShaderResource::CreateFromFile(device, Rhi::ShaderStage::Vertex,
                       "shaders/fullscreen.vert.spv", "slime.vs");
auto pSlimeFsMod = Resource::ShaderResource::CreateFromFile(device, Rhi::ShaderStage::Fragment,
                       "shaders/slime_metaball.frag.spv", "slime.fs");
// pFullVsMod->GetModule() / pSlimeFsMod->GetModule() 喂进 §12.1 的 gp.mShaderStages
```

底层路径（走 EmbedSpv 内嵌 blob 时，`offscreen_to_screen/main.cpp:114-126`，`RHIShaderModule.h:15-20`）：

```cpp
auto pMod = device.CreateShaderModule({embeddedCode.data(),
                                       embeddedCode.size() * sizeof(uint32_t),
                                       Rhi::ShaderStage::Fragment});
```

### 12.8 file:line 照抄索引

| 需求 | 文件 | 行 |
|---|---|---|
| 空顶点输入 + 深度关 pipeline | `samples/offscreen_to_screen/main.cpp` | 190-202 |
| alpha-blend attachment | `samples/particle_field/main.cpp` | 278-291 |
| descriptor 四件套 | `samples/offscreen_to_screen/main.cpp` | 153-175 |
| CreateBuffer + FIF 分片 | `samples/particle_field/main.cpp` | 372-380 |
| 每帧 Map/写/Unmap + slice 偏移 | `samples/particle_field/main.cpp` | 428-443 |
| bind+SetDescriptorSet+Draw(3) | `samples/offscreen_to_screen/main.cpp` | 276-311 |
| TransitionTexture 三段 | `samples/offscreen_to_screen/main.cpp` | 217-218 / 254-259 / 315-316 |
| `fullscreen.vert` 全文 | `samples/offscreen_to_screen/shaders/fullscreen.vert` | 1-14 |
| falloff 密度数学 | `samples/particle_field/shaders/particle_field.frag` | 14-33 |
| glslangValidator POST_BUILD | `samples/particle_field/CMakeLists.txt` | 17-36 |
| ShaderResource::CreateFromFile | `samples/particle_field/main.cpp` | 353-366 |
| CreateShaderModule(blob) | `samples/offscreen_to_screen/main.cpp` | 114-126 |
| DescriptorWrite/mBufferInfo | `include/orange/rhi/RHIDescriptor.h` | 55-72 |
| GraphicsPipelineDesc | `include/orange/rhi/RHIPipeline.h` | 241-254 |
| SetDescriptorSet/dynamicOffsets/Draw | `include/orange/rhi/RHICommandList.h` | 129 / 194 / 197 |
| BufferDesc/Usage::Uniform/CpuToGpu | `include/orange/rhi/RHITypes.h` | 10-43 |
| HDR TextureFormat 候选 | `include/orange/rhi/RHITypes.h` | 64-74 |
