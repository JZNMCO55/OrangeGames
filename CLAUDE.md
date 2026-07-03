# CLAUDE.md — OrangeGames

Orange 生态的游戏 / demo monorepo（consumer 仓），经 `find_package(OrangeEngine CONFIG)` 消费引擎，不在 tree 内持有引擎源码。生态拓扑 / 构建前置 / 跨仓 per-session 纪律见 [umbrella CLAUDE.md](../CLAUDE.md)。

## 回答语言

面向用户的解释 / 说明 / 分析用**中文**；代码标识符、API 名、库名、命令行参数保留英文。

## 代码风格（2026-07-03 用户约定）

- **注释精炼**：只写"为什么 / 非显然的坑 / 决策依据"，删掉复述代码在做什么的行；一个点 ≤2-3 行。冗长注释严重影响读代码。
- **变量名达意**：沿用文件既有命名习惯，不用无意义缩写。
- **大括号 Allman**：`{` / `}` 各自独立成行、与控制语句同列，随嵌套逐层缩进（勿混 K&R / 行尾大括号）。
- **namespace 体缩进一级**：内容随 namespace 缩进（保留嵌入阶级）；嵌套用 `namespace A::B::C {}` 合并避免过深。有别于 Google/LLVM「不缩进 namespace」主流惯例，属用户明确选择。
- 只保证**新写 / 改动代码**遵循，不主动全量回填既有代码。

## 结构 / 命令

- `prototypes/spike-01-blob/` —— 首游 Spike 1（流体史莱姆 context-sensitive 手感）。执行规格 `prototypes/spike-01-blob-feel.md`、设计宪法 `prototypes/phase0-pillars.md`。
- 构建见 `docs/building.md`（引擎须先 install 成可 `find_package` 的 SDK）。
- 测试：`ctest --test-dir build` —— header-only 游戏逻辑的 headless 回归（如 `spike-01-blob.blob_collision`），只连 glm 不拉引擎运行期。
