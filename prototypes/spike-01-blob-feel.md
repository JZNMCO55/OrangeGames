# Spike 1 — 流体史莱姆 context-sensitive 手感

> 状态：**设计已定，代码未开工**。本文件是下次 OrangeGames code session 的执行规格。
> 记录日期：2026-05-26。归属：OrangeGames 首游（Ori-like 平台跳跃，流体/史莱姆主角）原型阶段 Phase 1 spike。
> 上位设计宪法见 [`phase0-pillars.md`](phase0-pillars.md)（spine = 形态切换 / 战斗；流体 = 本体 + 所有形态共用的穿行地基）。

---

## 这个 spike 要回答的唯一问题

> **软体史莱姆能不能达到平台跳跃级的跟手手感，同时保留明显的果冻感？两者的折中点（"果冻预算"）在哪？**

这是一个 timebox **~1 周**的 spike，产出是**知识不是成品**——一个诚实的手感结论 + 一个可量化的旋钮值，不是一个能玩的关卡。

### 为什么先做这个（而不是灰盒胶囊）

用户拍板"软体先行、手感后调"，与通常的"先灰盒证明移动"有偏差，但这里尊重该选择，当作有边界的 spike：因为 **context-sensitive 液态形态**三重重合——最独特的设计 + 最高的技术风险 + 最对口用户的渲染/sim 长板。base 手感没立住、一个替代形态都没设计前，无法验证后面的"形态切换好不好玩"（那是 Spike 2）。所以这是地基。

**spine 拍板后地位升级**：首游脊柱已定为**形态切换 / 战斗**（[`phase0-pillars.md`](phase0-pillars.md)），流体本体是**所有形态共用的穿行地基**——不是某一个形态的事。所以本 spike 不只验"史莱姆好不好玩"，是验"整个游戏的移动地基立不立得住"。地位比 spine 拍板前更重。

---

## 唯一硬纪律：feel 在前，视觉在后

**这周 blob 只用 debug 线框 + 纯色。** metaball / screen-space 密度场 / 折射 / 菲涅尔 / subsurface **一律不碰**，直到下面成功判据里的 #1 #2 #3 至少立住。

这是用户作为渲染专家**唯一会掉的坑**：会本能地先把 blob 调好看，等发现它不跟手时已经投入很多。漂亮的果冻 ≠ 好玩的果冻。视觉好看留到手感证明跟手之后。

---

## 核心架构：果冻感和精确落点不在同一层

设计的关键洞察——不要让**可见的果冻**直接承载 gameplay，否则"好看"和"跟手"就是零和取舍（内部抖动越大、落点越随机）。改成**分层**：

| 层 | 是什么 | 承载 | 特性 |
|---|---|---|---|
| **gameplay 真相层** | 隐藏的刚体 **control target**（一个点/小胶囊） | 落点、平台碰撞、coyote/buffer/apex、输入响应 | 天生精确，跟刚体一样可预测 |
| **视觉表现层** | 可见的软体 blob | 只负责"看起来像果冻" | 用弹簧软软追 control point，随便晃 |

这样"落点 vs 果冻"从 0/1 取舍变成一个**可调的容差区间**：能让果冻松到什么程度。

### 必须点破的陷阱

gameplay 真相在隐藏点、玩家眼睛却在看果冻 → 当**果冻和 control point 分叉时，玩家照着果冻瞄、却按隐藏点落地** → 感觉"被骗"。

所以真正的约束**不是**"果冻必须精确"，而是：

> **果冻只在"玩家正在读它做决策"的瞬间需要和 control point 重合；纯位移途中随便分叉，那反而是 juice。**

玩家读图做决策的瞬间只有两三个：**跳跃顶点（决定往哪落）、站定待机、对准窄缝/边沿那一刻**。抓住这几个 moment，其余时间让它甩、拖尾、overshoot，全是加分的果冻感。

---

## 三个可叠的折中机制（按性价比排）

1. **状态相关 stiffness（最高 ROI）**：弹簧硬度不是常数，按状态切——顶点/待机/对缝时调硬（果冻快速收敛回可读轮廓），起跳冲击/落地/贴墙/位移途中调软（放开晃）。实现 = 一个 `stiffness(state)` 查表。
2. **底部接触点 x 锁定**：决定落点的只是 blob 最低那个接触顶点的水平位置。让最低接触顶点的 x 紧跟 control point 的 x（上半身随便晃）→ 落点稳、身体抖。视觉上"脚下稳、身上颤"，正好像果冻。
3. **落地/边沿 assist**：平台跳跃老把戏（corner correction / ledge snap）作用在 control point 上。果冻可以晃，gameplay 落点主动吸附到合理格点。让你能放更多果冻而不牺牲可玩性。

---

## 执行方法论：先 baseline，后加档

不要一上来就堆机制——否则把问题调没了看不见。分两步：

- **Step A — baseline**：单档软弹簧（保留明显果冻），control point = gameplay 真相，**先诚实量未经修饰的 apex 分叉有多糟**。
- **Step B — 按需加档**：仅当 baseline 的 apex 分叉超过"开始觉得被骗"的临界值时，才上机制 1（状态相关 stiffness 在顶点那一档收紧），必要时叠机制 2 / 3。

---

## 成功判据（全部可现场证伪）

1. **跟手延迟**：按下方向键到 control point 明显起速 ≤ ~2-3 帧（60fps 下 ~33-50ms）。超过就是发肉。判定：debug 叠"输入瞬间"标记 + control point 速度曲线，肉眼看滞后。
2. **落点可预测**：连续 10 次从同一平台起跳，**落点散布测在 control point 上**（按构造精确，这条应直接过）。
3. **挤缝是"可控通过"而非"挤进去运气好"**：对准窄缝按方向，blob 能稳定变形钻过，不卡住/不弹飞。这是主角的差异化卖点，必须当场能演示。
4. **形变服务于读图**：玩家能从 blob 形状一眼看出它在做什么（蓄力扁 / 冲刺拉长 / 贴墙挂住）——形变是反馈通道，不只是装饰。

### Spike 真正要搜的旋钮 = apex 分叉

新增核心待测量：**apex 分叉** = 跳跃顶点时 blob 视觉质心和 control point 的距离。

> Spike 1 的核心产出 = 回答一个数：**apex 分叉能放到多大，玩家才开始觉得"瞄不准/被骗"**。

那个临界值就是**果冻感预算**：低于它 → 尽情软；高于它 → 在顶点那一档收紧。干净、可证伪、一周能跑出来，且完全在用户长板里（约束求解 + 调参）。

---

## 技术方案（昨日已定）

- **场景**：一屏 —— 平地 + 2-3 平台 + 一道窄缝 + 一个边沿。
- **blob**：自写 Verlet/PBD，~12-20 质点 + 距离约束 + 面积/压力约束。碰撞打 **Box2D 静态世界**（Box2D 无原生软体，blob 自写比凑 joint 干净；物理 proxy 与视觉表示分离）。
- **关键技巧**：藏一个刚体 control target，blob 弹簧吸附它。输入驱动隐藏目标 = 精确；软体跟随 = squishy。调不跟手就把弹簧调硬 → 悄悄滑向混合方案（这正是机制 1 的连续谱）。

### 旋钮清单

- 约束 stiffness / damping（升级为**按状态几档**，非单一常数）
- control-target 弹簧硬度
- 地面摩擦
- 跳跃冲量
- coyote time / jump buffer / apex hang（作用在 control point 的质心，不是 blob）

---

## 引擎接线参考（大概率不用给引擎加 feature）

OrangeEngine 已有现成件，开搭前先抄这三个 sample 接线：

- `samples/06_physics_platformer` —— 平台跳跃 + Box2D bridge 接线
- `samples/10_thirty_seconds_demo` —— 综合 demo
- `samples/15_debug_draw_minimal` —— debug draw，足够画丑线框 blob

已确认引擎现状：Box2D 完整 bridge（`src/physics/box2d/Box2DBridge`、`PhysicsWorld`、`RigidBodyComponent`）+ debug draw（`src/render/DebugDrawScene`）+ procedural animator + Camera + RenderableComponent。

**软体 blob 仿真 + context-sensitive 环境逻辑是 OrangeGames 游戏代码，不是引擎 feature**（Box2D 不做软体）。唯一可能缺口 = 逐帧形变实心网格上传 + metaball/折射 shader（OrangeRender 活），属 Phase 3 好看阶段，Spike 1 不碰。

---

## 不在 spike 范围内

- ❌ metaball / 折射 / 菲涅尔 / subsurface 等任何好看的渲染（Phase 3）
- ❌ boss roster / 形态切换（**Spike 2 = 生死 spike**：脊柱是形态切换，最危险假设就在这里。范围 = base ↔ 1 个形态 + 一个招牌能力 + **限时回落**，验证 P1+P2；base 手感没立住前不做。详见 [`phase0-pillars.md`](phase0-pillars.md)）
- ❌ 多屏 / 关卡 / 真实美术资源（原型期免费 CC0 占位）

---

## 缺口记录纪律

引擎缺口**撞到时**记、不预测性记。流程：本 spike 的 OrangeGames session 撞到 → 先记本仓 prototype 笔记（同子仓允许）→ **单独** OrangeEngine session 转录进 `OrangeEngine/docs/engine-known-gaps.md`（per-session 单子仓，一个 session 不碰两子仓的代码）。

## 下一步

**写 spike 代码须新开 OrangeGames code session**（per-session 单子仓纪律）。届时按本文件"技术方案 + 引擎接线参考"直接搭，按"执行方法论 + 成功判据"调参。OrangeEngine 仓 `pre-game-design/character-forms.md` 可拉来当角色设计参考。

spike 跑出"软体手感结论 + apex 分叉临界值"后，沉淀回 Orange-Wiki `case-studies/orange-games/` 档案（source-driven 记结果）。
