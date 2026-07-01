# Spike 1 — 爬墙 wall-climb（Loop A 扩展）

> 状态：**代码落地 + 编译/启动 smoke 通过；手感未真机 dogfood**（2026-07-02 自主 session 落地）。
> 归属：Loop A control-point 手感层的扩展（分层纪律不变：爬墙只作用在隐藏 control point，
> 可见 blob 照旧软软追随）。执行规格母文档见 [`../spike-01-blob-feel.md`](../spike-01-blob-feel.md)。

## 加了什么

在既有平台跳跃手感（加/减速 + 可变跳跃 + apex + coyote/buffer/corner）之上，补三件贴墙动作：

| 动作 | 触发 | 效果 |
|---|---|---|
| **墙滑 wall-slide** | 空中 + 按住朝墙方向（A/D 顶向墙）+ 下落 | 下落限速到 `wallSlideSpeed`（比自由落体慢 = "抓住墙"感） |
| **墙跳 wall-jump** | 贴墙（或离墙 `wallCoyoteTime` 内）+ 跳跃键 | 离墙方向水平初速 `wallJumpSpeedX` + 竖直 `wallJumpSpeedY`；起跳后 `wallJumpLockTime` 内抑制"回墙"输入（防立刻粘回） |
| **抓墙攀爬 wall-grab** | 贴墙 + 按住 **Shift** | 吸附墙面（重力挂起）；**↑/↓** 以 `wallClimbSpeed` 上下攀爬；`wallStamina>0` 时耐力耗尽自动松手（默认 0=无限） |

侧向探墙 = 从 control point 中心朝左右各投三点水平 raycast（引擎 `RaycastClosest`，filter `kCatWall`，
复用 Loop A 迁移到引擎空间查询的同款过滤）。判据 overlay：贴墙侧一根竖线（grey=触墙 / cyan=下滑 /
yellow=抓墙）+ 墙跳瞬间一根离墙方向短线；面板顶部有 `wall LEFT/RIGHT | SLIDE/GRAB | wallCoyote | lock` 读数。

## 操作

`A/D`（或←/→）移动 · `Space/W` 跳跃 · **按住朝墙** = 墙滑 · **贴墙时跳** = 墙跳 · **Shift** 抓墙 + **↑/↓** 攀爬。
（原 `Up` 键从"跳跃"移给"攀爬 ↑"；跳跃仍是 Space/W。）

## dogfood 待验（feel 未验，全部旋钮在面板 "爬墙 wall-climb" 段可现场调，不重编）

按 [`../spike-01-blob-feel.md`](../spike-01-blob-feel.md) 的循环 A 方法论（一次动一个旋钮、抄参考值再微调）：

1. **墙滑跟手 / 不粘**：按向墙能稳定下滑、松开方向立刻脱离；`wallSlideSpeed` 太小=像黏住、太大=没抓住感。
2. **墙跳离墙曲线**：`wallJumpSpeedX/Y` + `wallJumpLockTime` 三者耦合——锁太短会立刻粘回、太长会"断控制"。抄 Celeste/HK 墙跳数值再调。
3. **窄缝双壁反复墙跳**（关卡中间 x=±0.4 两壁）：能否在两壁间交替墙跳往上爬 = 墙跳是否成立的硬判据。
4. **抓墙攀爬**：Shift 吸附是否跟手、↑/↓ 攀爬速度、到墙顶 `wallDir` 变 0 自动松手能否顺畅 mantle。
5. **不破坏 Loop A base 手感**：关掉 `wall enabled` 应逐字回到原平台跳跃手感（回归自检）。

## 已知待观察点（读代码判断，未实证）

- **探墙三点采样**：上/中/下三点，边沿/薄墙可能漏报或过报（同 Loop A `HeadClearAt` 的 broad-phase 取舍）。
- **墙滑要求"按住朝墙"**（非任意贴墙即滑）：更接近 Hollow Knight，若期望 Super-Meat-Boy 式"贴墙即滑"需改 `pressingIntoWall` 条件。
- **无墙滑/抓墙时的贴墙下落无摩擦**（Box2D 侧墙不产生额外阻力）——若显得太滑再补。
- **默认值是抄来的起点**（wallSlide 3.5 / wallJumpX 11 / wallJumpY 15 / lock 0.12 / climb 4），未经手感校准。
