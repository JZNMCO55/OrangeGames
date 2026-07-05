# 实体渲染阶段 — 流体史莱姆视觉设计（blueprint）

> 状态：**设计蓝图，未开工**。本文件是"实体渲染阶段"的执行规格，供后续多个 session 按其拆分施工。
> 记录日期：2026-07-05。归属：OrangeGames 首游 Spike 1 后续阶段。
> 上位规格：手感母文档 [`spike-01-blob-feel.md`](spike-01-blob-feel.md)；dogfood 裁决 [`NOTES-dogfood-2026-07-05.md`](NOTES-dogfood-2026-07-05.md)。
> 目标风格图：用户提供的 12 状态史莱姆参考（绿色胶质 + Fresnel 亮边 + 发光眼睛 + 体内光斑 + 落地溅射）。

---

## 0. 这个阶段是什么 / 为什么现在做

`spike-01-blob-feel.md` 立了硬纪律：**"feel 在前，视觉在后"——metaball / 折射 / 菲涅尔 / subsurface 一律不碰，直到手感判据立住。** 2026-07-05 首次真机 dogfood 把 **Loop A 判成 PASS**（control point 落点可控可达），并把两件事**明确 defer 到"实体渲染阶段一起做"**：

- `BUG-2026-07-05-blob-stranding`（视觉/gameplay 永久脱节，见 dogfood 笔记）
- squeeze 判据（挤 0.7 窄缝好不好看，"依赖形变，须实体渲染才看得出"）

因此本阶段**打包三件事**：① 真实体渲染（本文件主体）；② stranding 修复；③ squeeze 判据落地。三者共用同一套"真视觉"，分开做是浪费。

**前置已满足**：Loop A 手感 PASS，gameplay 真相层（control point）稳。本阶段不动 gameplay 数值，只做视觉层 + 视觉层与真相层的耦合修复。

**本阶段不做**：折射 grab-pass（见 §7 边界）、真正的 SPH 自由粒子重构（分裂定位为 juice，见 §2）、3D 建模 12 个姿势（见 §1）。

---

## 1. 关键认知：不建 12 个姿势，姿势从 sim + shader 涌现

参考图看着很 3D，但**首游是 2.5D，不要去建 12 个 3D 模型或画 12 张 sprite**。12 个状态是**同一个软体 sim 在不同受力下的形状** + **同一个 shader 的着色**，程序化涌现。roundness（那种 3D 圆润）**从 2D SDF 伪造**，不是真几何。这既省美术、又天然覆盖状态之间的所有过渡帧。

两个正交的层，全程保持解耦（沿用 spike 核心架构）：

| 层 | 是什么 | 本阶段动它吗 |
|---|---|---|
| gameplay 真相层 | 隐藏 control point（精确落点/碰撞） | ❌ 不动（Loop A 已 PASS） |
| 形变层（sim） | `Blob.h` 15 质点 Verlet + PBD | ◐ 小改（§4） |
| 着色层（render） | 新增 2D metaball/SDF shader | ✅ 新建（§3） |

---

## 2. 架构决策

### 决策 A — sim 保留固定环 + 脚本化分裂/合并（用户 2026-07-05 拍板）

分裂/合并（状态 11/12）定位为**偶尔的演出 juice**，不是核心玩法动词。因此：

- **保留** `Blob.h` 现有单闭环 + distance/area 约束（已为"一个连贯史莱姆"调过手感，1-10 状态都靠它）。
- 分裂/合并当**离散脚本事件**处理：落地溅射 = spawn 几个短命小 blob/水滴；分裂 = 临时 spawn 第二个环 + 一颗水滴飞出再融回。**不**为这两个状态把整套 sim 重构成自由粒子内聚（架构 B）——那会让已调好的 Loop B 手感全部重来。
- 若日后分裂/合并升级为核心动词（绑形态切换/战斗脊柱），再评估架构 B 或混合，届时另立设计。

### 决策 B — 渲染走 2D metaball / SDF 密度场

一个技术同时解决四件事：**剪影平滑 + 材质着色 + 假 3D 圆润 + 分裂/合并视觉过渡**。全在渲染长板内。

**推荐模型：多边形 SDF + smooth-min 圆角（主体）＋ 附加 metaball 点（水滴/碎块）**——混合方案，兼顾控形与自由融合：

- **主体** = 对 14 点闭合 perimeter 多边形求 signed distance `d(x)`，再按小半径 ρ 膨胀 → 棱角自动圆成胶质弧面。**保住 sim 已 area 约束调好的形状与体积**，不丢控形。
- **水滴 / 分裂碎块** = 额外圆形 SDF，用 smooth-union（`smin`）融进主场 → 靠近自然连、飞离自然断，状态 6/11/12 的过渡不用特判。
- 备选：纯 metaball（15 质点各当场心，忽略多边形连接）——更"blobby"、水滴/分裂更统一，但剪影不再由环直接控。默认取上面的混合，纯 metaball 留作 A/B 对比。

绘制方式 = 在 blob 的**屏幕空间包围区域**画一个 quad，fragment shader 里算场 + 着色（不是全屏，省 fill）。这正是 `spike-01-blob-feel.md` 早预判的"逐帧形变实心 + metaball shader，属 Phase 3 好看阶段"。

---

## 3. 渲染管线设计

一个 fragment shader 走完剪影 + 假 3D + 材质。各要素与上一轮材质评估一一对应：

### 3.1 剪影 / SDF
- 求 `d(x)` = 到主体表面的 signed distance（内负外正），阈值 `d=0` 切剪影，`smoothstep` 抗锯齿边。
- 水滴/碎块 `smin` 融入。

### 3.2 假 3D 法线（roundness 来源）
- 从 SDF 造穹顶高度：`h = f(d)`（如 `sqrt(R² - d²)` 近似圆顶）。
- 法线：`N.xy = ∇d`（2D 梯度，指向边缘）× 权重，`N.z = sqrt(1 - |N.xy|²)`。中心朝 +z、边缘外翻 → 打光后读成 3D 圆顶。2.5D 视线取 `V ≈ +z`。

### 3.3 材质（上一轮评估的落点，同一 shader 内叠加）

| 视觉要素 | 技术 | 数据来源 |
|---|---|---|
| 亮绿 Fresnel 亮边 | `pow(1 - N.z, k)` × 绿 | 假法线 |
| 假 SSS 通透（薄处亮、厚处深绿） | 厚度 `|d|`/穹顶高度驱动 wrap-diffuse | SDF 内部距离 |
| 湿润顶部高光 | Blinn/GGX spec（主光在上方） | 假法线 + 光向 |
| 发光眼睛（两椭圆） | emissive > 1 → bloom | blob 局部帧 UV，跟质心/朝向 |
| 体内漂浮光斑 | emissive 点 或 additive 粒子 | 局部 UV 散布 / VfxSystem |
| 体内流动/caustics | UV 扰动 + caustics（抄 `water_basic.frag` 思路） | 局部 UV + time，× 厚度 |
| 半透明 | 输出 alpha < 1（核心边深、边缘透） | 厚度 |
| 落地接触辉光 | 接触点下方 additive 径向 decal | 接触点位置 |

- **眼睛/光斑跟随**：在 blob 局部坐标帧（质心 + 主轴朝向）里定义 UV，形变时眼睛随体拉伸/压扁，读起来"活"。
- **alpha 输出**：`pbr.frag` 现写死 `alpha=1`，本 shader 是自定义的、直接输出 alpha<1，不受此限；但 **pipeline 必须开标准 alpha blend**（见 §5 引擎缺口）。

### 3.4 引擎接线
- 走自定义 shader 扩展点，模板 = `OrangeEngine/samples/08_custom_shader`（`fresnel.frag.glsl` 已演示 Fresnel 自定义材质路径）。
- 把 N 个质点位置（或多边形顶点 + 水滴中心）作为 uniform/push 推进 shader。§5 验证这条路够不够。
- 体内光斑/落地溅射水滴可用 `VfxSystem`（additive billboard 粒子，天然发光晕、喂 bloom）。

---

## 4. sim 侧改动（`Blob.h` + 驱动层）

按 §1 表映射，逐状态列改动。参照 12 状态映射（现状见 dogfood 评估）：

- **状态 1/2/4/5/7/9/10**：现有环直接给，无需改。
- **状态 3 拉伸（高瘦形）**：上"状态相关 stiffness"（spike spec 机制 1，最高 ROI）——顶点/拉伸态调软让它拉长，落地/待机调硬收敛回可读轮廓。查表 `stiffness(state)`。
- **状态 6 落地冲击**：压扁已有；新增**接触瞬间 spawn 溅射水滴**（短命小 metaball/粒子，`smin` 融或飞离）。
- **状态 8 滚动**：现 `restOffset` 世界系固定、弹簧永远拉回"正立圆"，不会滚。需给 rest 帧加**角度状态**（restOffset 随滚动角旋转）或滚动态大幅调软靠碰撞摩擦涌现。**优先级低**（平台跳跃滚动少），可 defer。
- **状态 11/12 分裂/合并**：按决策 A **脚本事件**，不改核心 sim——临时第二环 + 水滴动画，视觉靠 §3 的 `smin` 场融合无缝过渡。
- **stranding 修复（must-fix）**：dogfood 选定的"接近才碰撞"leash——`SatisfyCollision` 门控在质心与 cp 距离阈值内，远则关碰撞让 blob ooze 回 cp，近则开碰撞保挤缝/贴形。加 slider 调阈值（= 碰撞 leash 量程）。**本阶段做，因为它此前就 defer 到这里。**
- **squeeze 判据**：squeeze 隧道几何已就位（缝宽 0.7，cp 0.36 可过、blob Ø1.0 须形变）。本阶段有真视觉后，判"blob 形变挤过去好不好看"。

---

## 5. 引擎缺口清单（撞上即记，落地走独立 OrangeEngine session）

| 缺口 | 侧 | 说明 | 阻塞度 |
|---|---|---|---|
| Material 缺"标准 alpha blend"档位 | OrangeEngine | 现只有 `additiveBlend`（会越叠越亮发白）；RHI 层 blend 已完整，只需 `Material` 暴露 `alphaBlend`/`BlendMode` enum + `PipelineImpl` 加分支 + 关 depth write。纯通用能力，不含 slime 概念 | **硬阻塞** |
| 自定义 shader + 质点 uniform 路径验证 | OrangeEngine | 确认 `samples/08_custom_shader` 的自定义材质路径能把 N 个质点/多边形顶点推进 shader（uniform 数组或 push constant）。大概率够，需实测 | 待验证 |
| 折射 grab-pass | OrangeRender/引擎 | 背景透过史莱姆的扭曲需把不透明 scene color 拷成纹理喂 shader。中等工作量 | **本阶段不做**（§7） |
| 透明物体深度排序 | OrangeEngine | 单个凸主角可用背面剔除/两 pass 规避，暂不需真排序器 | 可回避 |

**登记纪律**：缺口撞到时先记本仓 prototype 笔记（同子仓允许），转录进 `OrangeEngine/docs/engine-known-gaps.md` 须**单独 OrangeEngine session**（per-session 单子仓）。alphaBlend 也可走 `OrangeRender/docs/incoming_feature.md` 判定归属——但 blend 装配在 OrangeEngine 的 `PipelineImpl`，RHI 已够，**归 OrangeEngine**。

---

## 6. 跨仓 session 拆分（per-session 单子仓纪律）

本阶段横跨两个子仓的**代码**，**不能同 session 做**。顺序：

1. **Session E（OrangeEngine 代码）**：补 `Material` 的 `alphaBlend`/`BlendMode` 通用档位 + `PipelineImpl` 分支；验证 sample 08 的质点 uniform 路径够用（不够则一并补 + 加 sample 验证）。commit + push + tag。
2. **Session U（umbrella bump）**：`git -C OrangeEngine pull` + bump submodule pointer + SDK reinstall。
3. **Session G（OrangeGames 代码）**：写 metaball/SDF shader + §4 sim 改动（stranding leash / 状态 stiffness / 溅射 / squeeze）+ 12 状态驱动。消费 Session E 的 alpha blend。
4. **dogfood**：真机看 squeeze 挤缝 + stranding 是否消解 + 视觉是否达参考图神似度。

> 本设计文档本身是纯文档（ADR-010 豁免），可在任意 session 写/改；上述拆分只约束**代码**。

---

## 7. 边界 / 不做

- ❌ **折射 grab-pass**：v1 用"半透明 + Fresnel + 假 SSS + 体内粒子"已神似 ~85%，折射是后续 polish，不进本阶段。
- ❌ **架构 B（SPH 自由粒子）**：分裂定位 juice，保留固定环（决策 A）。
- ❌ **3D 建模/12 张 sprite**：姿势程序化涌现（§1）。
- ❌ **gameplay 数值**：Loop A 已 PASS，本阶段不动真相层。

---

## 8. 分期验收（先 MVP 再 polish 再 juice）

沿用 spike"先 baseline 后加档"心态，避免一上来堆特效：

1. **MVP**：SDF 剪影 + 假法线打光 + 基础绿半透明，状态 1-10 跑起来、剪影平滑不见棱角。**先证"程序化形状 → 平滑胶质剪影"这条链成立。**
2. **材质 polish**：叠 Fresnel 亮边 + 假 SSS 厚度 + 湿润高光 + 发光眼睛 + bloom。对齐参考图。
3. **juice**：体内光斑 + 落地溅射水滴 + caustics 流动 + 分裂/合并脚本事件 + 接触辉光。
4. **修复并入**：stranding leash + squeeze 判据在真视觉下 dogfood 收尾。

每期都跑真机 dogfood（键盘进不去 GLFW 窗口，用户手玩、Claude facilitate + 记档，同 2026-07-05 流程）。

---

## 9. 沉淀

本阶段跑完后，把"metaball/SDF 史莱姆渲染 + 假 3D + 假 SSS"的结论沉淀回 Orange-Wiki `case-studies/orange-games/`（source-driven 记结果），后续形态的液态渲染可直接引用。

---

## 10. 实现落地（2026-07-06）—— Tier 1 CPU 着色 gel（已上屏验证）

> 用户授权跨仓开发、以"逼近参考图"为目标。实际落地**只动 OrangeGames 一处**（`main.cpp`），**未改任何引擎代码**——§5/§6 计划的引擎缺口路线被现状否决，改走 debug-draw。真机（截图）验证：黑底上一只通透绿 gel 球 + 明亮 Fresnel 边 + 两只发光眼 + 体内黄绿光斑 + 底部接触辉光，材质神似参考图。

### 关键引擎发现（改变了 §2/§3/§5 的技术前提）

Explore agent 通读引擎渲染面后确认：

1. **引擎不支持逐帧动态顶点上传**。`Pipeline` 的 `meshCache` 对同一 mesh handle **永不重传**（`Pipeline.cpp` 见 handle 命中即 `continue`），且顶点 buffer 是 `MemoryUsage::GpuOnly`（非 host-mapped），`MeshAsset` 无 `SetPositions`。→ 软体每帧变形**唯一**能走的是 `DebugDrawScene`（每帧重提交几何）。**§3 的"自定义 fragment shader"路线对形变主角不成立**（只能配静态 mesh 做表面 shader 动画）。
2. **自定义 shader 的 per-instance 通道极窄**：push constant 硬编码、只认 `uBaseColor/uMRA/uEmissive` 三个 vec4、**无数组 uniform**（传不了 N 个质点）、且开 per-instance 会强制背 set-1 五张贴图。→ 真 metaball（质点场求和）在 shader 里传不进数据。
3. **alpha blend 要改引擎 3+ 处**（Material 加字段 + PipelineImpl 分支 + ShaderTemplateDesc 不拷 blend 标志）。但**黑底上不透明即可**，本次无需。
4. 背景/发光的现成 public API：`Pipeline::SetSkyEnabled(false)` + `SetSceneClearColor(r,g,b)`（关天空 + 清近黑）；`BloomPass.threshold/intensity`（bloom 发光）。

### 实际渲染模型（Tier 1）—— `main.cpp` `EmitSlimeGel`

debug-draw 每帧把 blob（Verlet 环 + Catmull-Rom 平滑轮廓）细分成 `rings×segments` 个小面，每面在 CPU 上算一遍伪着色再平涂：

- **假 3D**：半球假法线 `n=(dir·tm, sqrt(1-tm²))`（中心朝观者、边缘外翻）；
- **dome 漫反** `n·L` + **Blinn 高光** `pow(n·H, specPower)`（左上主光 → 湿润高光）+ **Fresnel 亮边** `pow(1-n.z, rimPower)`（黄绿发光边）+ **厚度 SSS** `mix(deepColor, bodyColor, tm)`（厚 center 深绿、薄 edge 亮）；
- **发光眼**（两椭圆，暖黄 > bloom 阈值强发光，呼吸缩放）+ **体内光斑**（14 点确定性布点，上浮 + 明灭）+ **接触辉光**（底部压扁绿椭圆）；
- 全部平涂进 HDR + bloom 前，靠三角密度 + bloom 糊成渐变；**beauty 开关**隐藏关卡线框 + gameplay overlay（纯史莱姆黑底对齐参考图）。

调参全走 ImGui slider（颜色/光照/密度 live 可调）。

### 约束 / 已知限制

- **DebugDraw 顶点上限 4096**（≈1365 三角，OrangeRender 侧常量）：约束了细分密度（当前 `silhouetteSub=3` + `rings=12` ≈ 3500 verts）。棱角靠 bloom 掩盖；要更平滑剪影得**改引擎抬高该上限**（跨仓，未做）。
- **逐像素平滑（Tier 2 真 fragment shader）被引擎动态 mesh 缺口卡死**——要么改引擎（加 host-visible dynamic vertex buffer + meshCache 淘汰），要么走 debug-draw 把不透明 blob 画进离屏 thickness/coverage 纹理、再加一个自定义 **screen-space post pass** 逐像素着色（需确认 consumer 能否注入自定义 post pass）。二者都是引擎级工作，登记为后续。
- **相机是固定 showcase 框**（拉近对准静止区）；gameplay 移动时不跟随，follow-cam 是后续（引擎已有 `CameraFollow2D` 可消费）。
- 分裂/合并（juice）、落地溅射水滴、stranding leash、squeeze 判据本次**未做**（材质优先）；留后续。

### 登记：OrangeRender 增强候选（撞上即记，未实现）

- **DebugDraw 顶点上限可配 / 抬高**：4096 对"实心程序化 2D 图形（软体 gel）"偏小。建议 OrangeRender 把 `mMaxTriangleVertices` 提高或做成可配置。→ 待单独 OrangeRender session 登记 `incoming_feature.md`。
- **消费者动态 mesh 上传**：见上，软体逐帧变形的通用解。→ 同上，OrangeEngine `engine-known-gaps.md`。
