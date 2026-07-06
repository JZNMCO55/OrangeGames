# 12 状态形变驱动（施工规格）

> 状态：**设计定稿，未开工**。本文件是"让史莱姆动起来有姿态"的施工规格——与 Tier 2 渲染（[`DESIGN-tier2-sdf-pass.md`](DESIGN-tier2-sdf-pass.md)）**独立、互不阻塞**（见那份 §9）。
> 记录日期：2026-07-06。权威美术目标：`../../art/refs/slime-12-states-reference.png`（12 态）。
> 上位蓝图：[`DESIGN-solid-render-phase.md`](DESIGN-solid-render-phase.md) §4（本文件把那里的映射落到可施工的参数级）。

---

## 0. 为什么

用户 2026-07-06 演示 Tier 1 时逮的第二个问题：**移动/跳跃全程只有一个状态（永远圆球）**。根因（`Blob.h`）：每个质点的弹簧目标 = `cpPos + mRestOffset[i]`，`mRestOffset` 在 `BuildRestGeometry` 写死成**固定正圆**（`cos/sin*radius`），从不变；再叠强 PBD 圆约束（edge 1.0 + area 0.5 + 8 迭代）死命维持圆。**12 状态姿态一个都没实现**（`Blob.h` 注释自认"状态相关 stiffness = 机制 1，留 Step B"）。

本文件补这块。渲染（Tier 2）读 blob 质点位置画 SDF，形变改 blob 质点目标形状——**两者通过 blob 质点解耦**，谁先做都行，叠加才到参考图那种"有姿态的漂亮 gel"。

---

## 1. 核心机制：状态驱动 restOffset 变换 + stiffness 调制

**关键洞察**：不建 12 个姿势，姿势从"改弹簧目标形状 + 调软硬 + 让弹簧超调"涌现（沿用上位蓝图 §1）。

现在 `restOffset[i] = baseCircle[i]`（固定正圆）。改成：

```
restOffset[i] = M · baseCircle[i]        // baseCircle[i] = (cos θ_i, sin θ_i)·radius
M = R(angle) · diag(sx, sy)              // 各向异性缩放 + 旋转对齐运动方向
```

驱动层（`main.cpp` `SpikeLayer`）每帧按 control point 运动状态算目标 `(sx, sy, angle)` + `stiffness`，**SmoothDamp 平滑插值**当前值→目标值（避免状态切换突变），传给 blob。史莱姆姿态 = 弹簧追这个变了形的目标 + 惯性滞后/超调涌现。

三档实现，按 ROI 分层：

- **Tier A（对称各向异性缩放，覆盖大多数）**：`M = R·diag(sx,sy)`。做出下压/拉伸/跳起/落地/滚动/滑行的主体形变。**先做这层。**
- **Tier B（不对称 taper，polish）**：线性 `M` 保持中心对称，做不出"倒水滴/前圆后粗"。加沿主轴的位置相关缩放（taper）补不对称。M2 后 polish。
- **脚本事件 / 碰撞涌现（不走 restOffset）**：溅射水滴 / 分裂 / 合并 = 脚本 spawn；挤压 = 碰撞约束天然涌现；回弹 = 弹簧欠阻尼超调天然涌现。

---

## 2. 状态检测：`SlimeMotionState`

从 control point（gameplay 真相层，已 PASS）每帧派生。输入：`grounded`、`velocity(vx,vy)`、蓄力信号（下蹲键/jump buffer）、（可选）贴墙。

```
enum class SlimeMotionState { Idle, Crouch, Launch, Rising, Falling, Landing, Sliding, Rolling, Squeezed };
```

派生逻辑（优先级从上到下，`DeriveMotionState`）：

| 状态 | 触发判据 |
|---|---|
| **Landing** | 本帧 grounded 且上帧空中（着地沿）+ 落地垂直冲击 `|vy_prev|` 大 → 触发一次性冲击（计时 ~0.12s） |
| **Squeezed** | blob 质点被两侧碰撞同时挤（`SatisfyCollision` 报双侧接触）→ 走碰撞涌现，不设 M |
| **Crouch** | grounded 且蓄力键按住（起跳前下压） |
| **Launch** | 离地后极短窗口（起跳后 ~0.1s）或 `vy > vyLaunchThresh` 上升初期 |
| **Rising** | 空中 `vy > 0`（上升） |
| **Falling** | 空中 `vy < 0`（下降） |
| **Sliding** | grounded 且 `|vx| > vxSlideThresh`（高速水平） |
| **Rolling** | （可选，defer）grounded 高速 + 特定条件 |
| **Idle** | grounded 且低速（兜底） |

状态切换不硬切——目标 `(sx,sy,angle,stiffness)` 查表后靠 §1 的 SmoothDamp 过渡。

---

## 3. 逐状态施工表

`sx`=横向、`sy`=纵向缩放（相对基础圆 radius=0.5u）；`angle`=M 旋转（多数 0，滑行/滚动对齐运动）；`stiffness`=当帧弹簧硬度（基线 200）。参照 `art/refs/slime-12-states-reference.png`：

| # 参考态 | MotionState | sx | sy | angle | stiffness | 关键涌现/备注 |
|---|---|---|---|---|---|---|
| 1 静止（矮 dome） | Idle | 1.15 | 0.80 | 0 | 200 | 底平靠地面碰撞压平底部质点 |
| 2 下压（更扁） | Crouch | 1.35 | 0.60 | 0 | 400（硬，蓄力紧实） | 蓄力越深越扁（按蓄力量插值 sy 0.60→0.75） |
| 3 拉伸（高瘦柱） | Launch | 0.70 | 1.55 | 0 | 120（**软**） | 软让纵向拉长滞后明显 = 果冻拉丝感 |
| 4 跳起（饱满球） | Rising | 0.95 | 1.10 | 0 | 200 | 接近圆略纵长 |
| 5 下落（倒水滴） | Falling | 0.88 | 1.20 | 0 | 180 | **Tier B taper**：顶圆、底收窄下垂（下沉方向拖尾） |
| 6 落地冲击（极扁+溅射） | Landing | 1.60 | 0.42 | 0 | 350（硬瞬间） | 触发**溅射水滴脚本**（§5）；计时后转 Idle |
| 7 回弹（隆起+裙边） | —（Landing 尾） | — | — | — | 回 200 | **不特设**：Landing 后目标回 Idle，欠阻尼弹簧超调 = 顶部隆起+底裙边 |
| 8 滚动（斜椭圆） | Rolling | 1.30 | 0.85 | 随滚角 | 150（软） | **优先级低，defer**（平台跳跃滚动少） |
| 9 受挤压（方鼓包） | Squeezed | — | — | — | — | **不特设**：窄缝两壁碰撞挤 + area 约束保体积鼓包（squeeze 判据看这个） |
| 10 滑行（横躺水滴） | Sliding | 1.50 | 0.72 | atan2(vy,vx)≈0 | 200 | 沿 vx 拉长；**Tier B taper** 前（运动向）圆、后粗 |
| 11 分裂 | —（脚本） | — | — | — | — | **脚本事件**（§5）：临时第二环+细颈+飞出小球，SDF smin 融 |
| 12 合并（饱满椭圆） | —（脚本尾） | 1.15 | 1.00 | 0 | 200 | 合并脚本收尾定型；smin 融回单体 |

---

## 4. `Blob.h` 改动点

最小侵入，保留现有 Verlet+PBD 核（已调手感）：

1. 加成员 `glm::mat2 mTargetTransform{1.0f}`（默认单位=当前正圆行为，向后兼容）。
2. `BuildRestGeometry` 里：`mRestOffset[i] = mTargetTransform * (glm::vec2(cos,sin)*radius)`。中心点仍 0。
3. 加公共接口 `void SetTargetTransform(const glm::mat2& m)`：驱动层每帧调；变化超阈值时重算 restOffset（`BuildRestGeometry` 已有 slider 重建路径，复用）。
   - 或更省：Step 里 spring target 用 `cpPos + mTargetTransform * baseOffset[i]`，`baseOffset` 存基础圆、变换在 Step 内实时乘（免重建）。**推荐这个**（每帧变换平滑，不想每帧 rebuild）。
4. `stiffness`/`damping` 已是 `BlobParams` 字段——驱动层每帧改 `mBlobParams.stiffness` 即可，无需改 Blob.h。
5. **area 约束的 rest 面积**：各向异性缩放后多边形面积变（sx·sy≠1 时）。area 约束会抵抗形变把它拉回原面积——**要让 restArea 跟着 M 缩放**（`mRestArea *= sx*sy`），否则拉伸态被 area 约束死死拽回圆。这是关键坑，M1 首验。

---

## 5. 脚本事件（非 restOffset）

- **落地溅射（状态 6）**：Landing 触发瞬间 spawn N 个短命小 blob/水滴（初速朝外上方 + 重力），~0.4s 生命，SDF 里当附加 metaball 点（`uPoints[].w>0`）smin 融或飞离。
- **分裂（状态 11）**：脚本 spawn 第二个临时环 + 细颈（两环间插值中间点）+ 飞出小球；SDF smin 让颈部自然拉断。**juice，后期**。
- **合并（状态 12）**：第二环向主体靠拢，smin 距离渐小自然融，融毕删第二环。**juice，后期**。

这三个都靠 Tier 2 SDF 的 smooth-min 做视觉过渡（渲染线提供），故排在渲染 M2+ 之后。

---

## 6. stranding 修复 + squeeze（本阶段并入，dogfood 逮的 must-fix）

- **stranding leash**（`BUG-2026-07-05-blob-stranding`，dogfood 选定"接近才碰撞"）：`SatisfyCollision` 门控在"质心↔cp 距离 < 阈值"内——远则关碰撞让 blob ooze 回 cp（消除硬碰撞覆盖弹簧导致的视觉/gameplay 永久脱节），近则开碰撞保挤缝/贴形。加 slider 调阈值。**这是形变线的一部分，本阶段做。**
- **squeeze 判据**：窄缝几何已就位（缝宽 0.7，cp 0.36 可过、blob Ø1.0 须形变）。有 Tier 2 真视觉 + 挤压涌现（状态 9）后，dogfood 判"挤过去好不好看"。

---

## 7. 分步

1. **D0**：`Blob.h` 加 `mTargetTransform` + Step 内实时乘 + restArea 跟随缩放（§4.5 坑）。headless 测：设 sx=0.7/sy=1.5，验质点排成竖椭圆且 area 约束不拽回。
2. **D1**：驱动层 `SlimeMotionState` + `DeriveMotionState` + 查表 + SmoothDamp 插值。先接 **3 个最明显态**（下压/拉伸/落地冲击），真机看跳跃时史莱姆压→拉→扁的姿态涌现。
3. **D2**：补齐 Idle/Rising/Falling/Sliding + 回弹超调调参。
4. **D3**：stranding leash + squeeze 判据。
5. **D4（juice）**：溅射水滴 + 分裂/合并脚本（依赖 Tier 2 smin）；Tier B taper（下落/滑行不对称）。
6. **dogfood**：真机手玩（键盘进不去 GLFW 窗口，用户玩、Claude 记档），对参考图逐态核。

D0 有 headless 回归（`tests/blob_collision_test.cpp` 同款，只连 glm）；D1+ 靠真机 dogfood（形变是手感，看数值不够）。

---

## 8. 与渲染线的关系

- 形变改 blob 质点**目标形状**，渲染（Tier 2）读 blob 质点**当前位置**画 SDF——通过质点位置解耦。
- 可先做形变（当前 CPU 平涂下就能看姿态，虽糊）或先做渲染（好看但仍圆球晃）。**叠加 = 参考图效果**。
- 溅射/分裂/合并的视觉靠 Tier 2 SDF smin，故 §5 juice 排在渲染 M2+ 后。
