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

仓建立于 2026-05-24。第一款游戏（Ori-like 平台跳跃，流体/史莱姆主角）尚未开工。
