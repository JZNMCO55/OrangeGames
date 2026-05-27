# OrangeGames

基于 OrangeEngine 的游戏与 demo monorepo。

## 定位

- 承载多款基于 `OrangeEngine::orange_engine` 的游戏 / demo 子项目
- 通过 `find_package(OrangeEngine CONFIG)` 消费引擎
- 在 Orange 生态拓扑里属于 **consumer 仓**

## 仓库 constellation

OrangeGames 是 [Orange-Ecosystem](https://github.com/JZNMCO55/Orange-Ecosystem) sibling 拓扑下的 4 个核心仓之一（与 OrangeEngine / OrangeRender / Orange-Wiki 同级）。完整生态结构与拓扑决策见 [ADR-009](https://github.com/JZNMCO55/Orange-Wiki/blob/Orange-Render-Wiki/case-studies/orange-engine/decisions/ADR-009-vendor-topology-inversion.md)。

典型布局：

```
Orange-Ecosystem/
├─ OrangeEngine/
├─ OrangeRender/
├─ Orange-Wiki/
└─ OrangeGames/   ← this repo
```

## 当前状态

仓建立于 2026-05-24。第一款游戏（Ori-like 平台跳跃，流体/史莱姆主角）处于**原型 / Spike 阶段**：

- **设计**：首游宪法 + 脊柱已定（[`prototypes/phase0-pillars.md`](prototypes/phase0-pillars.md)）；Spike 1 手感执行规格见 [`prototypes/spike-01-blob-feel.md`](prototypes/spike-01-blob-feel.md)。
- **代码**：消费骨架已立（`find_package(OrangeEngine)` + 开窗 + debug draw），见 [`prototypes/spike-01-blob/`](prototypes/spike-01-blob/) 与构建说明 [`docs/building.md`](docs/building.md)。手感 Loop A / 软体 blob 尚未落地。
- **当前阻塞**：Spike 调参面板需引擎补完消费者 ImGui hook（OrangeEngine `GAP-2026-05-27-consumer-imgui-tuning-hook`）；运行还需 workaround 引擎 builtin shader 未 install（`GAP-2026-05-27-builtin-shaders-not-installed-for-consumers`）。下一步是独立 OrangeEngine session 补这两条，再回来接真 slider 续 Spike。
