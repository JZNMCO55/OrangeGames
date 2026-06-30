# Spike 1 · Loop A —— 引擎缺口记录

> 记录日期：2026-07-01。归属：OrangeGames 首游 Spike 1 · Loop A（control point
> 平台跳跃手感）实现 session。
>
> 纪律（spec "缺口记录纪律" + umbrella CLAUDE.md）：引擎缺口**撞到时**记本仓
> prototype 笔记，**不在本 session 实现、不改 OrangeEngine 代码**。后续由
> **单独的 OrangeEngine session** 把下列条目转录进 `OrangeEngine/docs/engine-known-gaps.md`
> （per-session 单子仓纪律：一个 session 不碰两子仓的代码）。

---

## GAP-2026-07-01-physics-no-contact-or-raycast-query（本 session 新撞）

**现象**：`Orange::Engine::Physics::PhysicsWorld` 公共面（`PhysicsWorld.h`）只有
body 级的读写（`GetBodyTransform` / `GetLinearVelocity` / `Set*` / `ReplaceFixture`
/ `SetBodyEnabled`），**没有任何碰撞查询**：

- 无 `Raycast` / `RayCastClosest`
- 无 `OverlapAABB` / `OverlapShape` / point query
- 无 contact / sensor 事件回调或本帧 contact 列表查询
- 无 "body A 是否正与 body B 接触 / 接触法线是什么" 的查询

**为什么平台跳跃需要它**：标准平台跳跃手感的三件套全依赖"碰撞查询"：

1. **接地判定（grounded）** —— coyote time、落地检测、空中/地面切换都要知道"脚下
   有没有踩实"。教科书做法是脚底向下射一条短 ray（或一个薄 overlap box）问物理世界。
2. **天花板/墙判定** —— corner correction（顶到角落小幅推移让玩家滑过去）要知道
   "头顶撞到的是什么、撞点在哪、左右哪边能让开"。
3. **可变跳跃 / 贴墙** 等后续机制同样要 contact 法线。

**本 session 的 workaround（够用但不通用）**：因为 spike 的关卡几何是游戏侧自持的
一份 `LevelBox` AABB 列表（同一份既喂 Box2D 静态体、又作 analytic 探针几何真相），
所以 grounded / ceiling / corner-correction 全部用**自写的 analytic AABB 探针**
（`ProbeGrounded` / `HeadClearAt` / `TryCornerCorrect`，见 `main.cpp`）绕过去了。
ceiling-bonk 用"上升中 Box2D 把 vy 打没了"间接探测。

**workaround 的局限（为什么仍要登记为引擎缺口）**：
- 只在"游戏侧手里就有全部碰撞几何且形状简单（AABB）"时成立。一旦关卡来自 tilemap /
  导入的 polygon collider / 斜坡 / 旋转体，游戏侧没法再自持一份可探针的几何真相。
- 等于把碰撞查询在游戏侧**重新实现了一遍**，与 Box2D 内部已有的 broadphase / 形状
  测试重复，且两份几何（Box2D 体 vs analytic 列表）有漂移风险。
- Loop B 软体 blob 的质点 vs 环境碰撞、context-sensitive 环境逻辑（挤缝 / 贴墙 /
  边沿吸附）几乎一定会再撞这条。

**建议的引擎侧最小补面**（仅建议，不在本 session 实现）：在 `PhysicsWorld` 公共面
加 Box2D 3.x 已原生支持的查询封装：
- `RaycastClosest(from, to) -> {hit, point, normal, fraction, BodyHandle}`
- `OverlapAabb(min, max) -> 命中 BodyHandle 列表` 或回调
- （可选）本帧 contact 查询 / `BeginContact` 事件，给 sensor / 贴地法线用

Box2D 3.x 的 `b2World_CastRayClosest` / `b2World_OverlapAABB` 直接对应，封装代价低；
公共面继续只暴露中性 `BodyHandle` + glm 类型，不破 header isolation 不变量。

**Loop B 复核确认（2026-07-01，无新缺口）**：Loop B 软体 blob 的每质点 vs 环境
碰撞如本条"局限"第 3 点所预测，**确实再次撞到同一缺口**——blob 没有走 Box2D
（blob 是纯游戏侧 Verlet/PBD 质点，不是刚体），其碰撞用与 control point 同一份
`mLevel` AABB 做 analytic 投影（`Blob::SatisfyCollision`，把钻进盒内的点推到最浅
穿透面）。**未新增引擎缺口**：blob 碰撞需求被现有 workaround 完全覆盖。同样的
"游戏侧自持 AABB 几何才成立 / 来了 tilemap / polygon collider 就失效"局限照旧适用，
故本条 GAP 维持登记。

---

## 设计记录（非缺口，仅备忘 control point 的 body 选型）

control point 选 **Dynamic** 刚体（velocity 驱动 + 自管重力，`gravityScale=0` +
世界重力归零），**不是** Kinematic。原因：Box2D 中 **kinematic 体不与 static 体
产生碰撞响应**（kinematic 只推 dynamic，碰 static 直接穿过）。而 control point 必须
踩平台 / 撞墙 / 钻窄缝——只能用 dynamic 体，每个固定子步 `SetLinearVelocity` 后
`Step`，再回读 `GetLinearVelocity` 让自管积分器与 Box2D 解出的碰撞结果对齐。
这不是引擎缺口，是 Box2D 固有语义，记此备忘以免后人误用 kinematic。

---

## 既有缺口（已登记，本 session 仅复用其 workaround，不重复登记）

- **GAP-2026-05-27-builtin-shaders-not-installed-for-consumers**：`cmake --install`
  不把 builtin `.spv` 装进 SDK；本仓 `CMakeLists.txt` 的 `ORANGE_ENGINE_SHADER_DIR`
  post-build copy 仍是 workaround。运行期实测：shader copy 到 `.exe` 旁后
  `Pipeline::Initialize` + `EnableImGui` 均正常（run.log 干净）。
- **GAP-2026-05-27-consumer-imgui-tuning-hook**：**已补完**。本 session 实测
  `Pipeline::EnableImGui` + `SetImGuiSubmit([h]{h->DispatchImGui();})` + `Layer::OnImGui()`
  端到端可用，调参面板 slider 正常接通（spec 地基 #1 解锁）。
