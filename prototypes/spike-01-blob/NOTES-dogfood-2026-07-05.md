# Spike 1 — dogfood 裁决 + 发现（2026-07-05）

> 首次真机 dogfood（用户手玩，键盘直接操作；windows-mcp 键盘进不去 GLFW 窗口，
> 故由用户玩、Claude facilitate + 记档）。执行规格母文档 [`spike-01-blob-feel.md`](../spike-01-blob-feel.md)。

## 裁决

### Loop A 手感（control point 平台跳跃）—— ✅ PASS
用户实玩判定"操作手感挺不错，落地稍微控制一下也能到达目标位置"。
→ **两层解耦的 gameplay 半边验证通过**：control point 作为精确真相层，落点可控可达。
这是 Spike 1 最关键一问（"精确落点"能否成立），过。

### 尚未逐项 dogfood（本次未覆盖，留后续或按需）
- 手感糖细项：coyote / jump buffer / corner correction 单独验证
- 爬墙 5 点（见 [`NOTES-wallclimb.md`](NOTES-wallclimb.md)）：墙滑不粘 / 墙跳曲线 / 窄缝双壁交替墙跳 / 抓墙攀爬 / 回归自检
- "果冻预算"主观判：blob 果冻追随读起来像流体史莱姆 vs 卡顿团（部分被下方 BUG 盖过）

## 发现：BUG-2026-07-05-blob-stranding（视觉层，**已决定 defer**）

**现象**：blob（视觉）卡在平台顶，control point（gameplay 真相）在正下方地面，橙色
divergence 竖线拉得很长——视觉与真相**永久脱节**。

**根因**（非渲染坐标 bug）：blob 无独立重力，唯一的力是每质点弹簧拉向 `cp + restOffset`
（`Blob.h` Step）；同时每质点对关卡盒做**硬碰撞投影**（`SatisfyCollision`，按入射面出盒），
且碰撞跑在弹簧积分**之后** + 每子步多次，**永远有最后发言权**。当实心平台夹在 blob 与
cp 之间（cp 掉到平台下方地面），弹簧想把 blob 往下拉，底部质点被平台顶面反复顶回 `maxy`
→ blob 永久卡在平台顶。加大 stiffness 无效（碰撞覆盖弹簧）。这是经典 visual/gameplay
desync：视觉一旦被几何体卡住就和真相脱节。

**选定修法**（用户拍板："接近才碰撞"）：把 `SatisfyCollision` 门控在 blob 质心与 cp 的
距离阈值内——离得远（被卡/脱节）时**关碰撞**，让 blob 像史莱姆一样穿过几何体 ooze 回 cp；
靠近后再开碰撞，保留挤窄缝 / 落地贴形。加个 slider 做阈值（= 碰撞 leash / 果冻碰撞最大量程）。
好处：divergence 自然有界、绝不永久脱节、无 leash 硬拽的穿模突跳。

**决策：DEFER 到"实体渲染"阶段一起做。** 理由：①现在 blob 是 debug 线框占位，跟随模型
此刻调是对着占位画面调，等真实 metaball / 剪影渐变实体渲染落地时视觉层本就要重做，届时并进去
最省事；②不影响手感 / gameplay（真相层是 control point，脱的只是视觉）。**但必须在 ship 前
解决**——visual/gameplay desync 破坏沉浸感，此处登记为 known must-fix，勿遗忘。

## 场景迭代 + squeeze 判据也 defer（2026-07-05）

用户 dogfood 反馈：原场景**盖不到**"两壁交替墙跳"（那条 ±0.4 窄缝缝宽 0.8 << 墙跳横向够程
wallJumpX×lock≈1.32，起跳即撞对面壁且 lock 未失效 → 贴死；且墙才 4 高没得爬），也盖不到穿缝隙。
根因：一条窄缝被塞了两个几何要求相反的用途（墙跳要宽缝+高墙，挤缝要窄缝+封顶）。

**已改 `MakeLevel`（拆成专用件）**：
- **墙跳 chimney**：缝宽 1.4（配当前墙跳默认值）、墙高 y=-3→4、左壁底浮起留地面入口、顶部 mantle 平台。→ **可现在 grey-box dogfood**（纯 control-point gameplay，不需渲染）。
- **squeeze 隧道**：封顶成孔、缝宽 0.7（cp 0.36 可过、blob Ø1.0 须形变）。

**squeeze 判据 DEFER**：用户明确"穿缝隙要有实体渲染才看得出，依赖形变"。故 squeeze 的真正判据
（blob 形变挤过去好不好看）**并入实体渲染阶段**，与 blob stranding 修复 + 真视觉一起做。隧道几何
已就位备用。现在只 chimney 墙跳这一项 grey-box 可测。
