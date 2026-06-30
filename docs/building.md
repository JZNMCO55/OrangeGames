# 构建 OrangeGames

> 状态：2026-05-27。本仓 = consumer 仓，经 `find_package(OrangeEngine CONFIG)`
> 消费引擎。**与子仓单独 clone 不能直接 build 同理**——必须先有一个 install
> 出来的 OrangeEngine SDK。新人请走 Ecosystem umbrella 仓 `--recursive` clone。

## 前置

- Windows 11 + MSVC 2022 + CMake 3.28+
- LunarG Vulkan SDK（`VULKAN_SDK` 已设）—— 经 OrangeRender 传递依赖
- 第三方依赖 prefix（第三方实际 install 在 `D:\3rdparty\install`，其 build/源在 `D:\3rdparty`；
  cmake config 如 `glfw3Config.cmake` 在 `D:\3rdparty\install\lib\cmake\`）+ OrangeRender SDK
  （典型 `D:\sdk\orange-render`）。这两样由 OrangeEngine 的 `scripts/build_all.py` 产出，见引擎仓 CLAUDE.md。

## 关键前提：先 install OrangeEngine SDK

本仓经 `find_package(OrangeEngine CONFIG)` 消费，build 前必须先 install 出
OrangeEngine SDK 到 `D:\sdk\orange-engine`（**2026-07-01 已 install 一次**；引擎
代码更新后需重新 install 才能让消费者拿到新 API）：

```powershell
# 在 OrangeEngine 子仓内（引擎自己的 build + install）
cd ..\OrangeEngine
cmake -S . -B build -DCMAKE_PREFIX_PATH="D:/3rdparty/install;D:/3rdparty;D:/sdk/orange-render"
cmake --build build --config Debug -j
cmake --install build --config Debug --prefix D:/sdk/orange-engine
```

> ⚠️ 已知缺口（OrangeEngine `docs/engine-known-gaps.md`）：
> - **`GAP-2026-05-27-builtin-shaders-not-installed-for-consumers`**：`cmake --install`
>   **不会**把 builtin shader（`shaders/orange_engine/*.spv`）装进 SDK。游戏能编过，
>   但运行期 `Pipeline::Initialize` 会因找不到 shader 失败。临时 workaround 见下方
>   配置命令里的 `ORANGE_ENGINE_SHADER_DIR`。
> - ~~**`GAP-2026-05-27-consumer-imgui-tuning-hook`**：引擎公共面暂无消费者 ImGui hook~~
>   **✅ 已补完**：Dear ImGui 已从 PRIVATE 升 PUBLIC，消费者经 `Layer::OnImGui()` +
>   `Pipeline::EnableImGui()` 写 debug-UI；Spike 1 的热重载调参面板已基于此落地。

## 配置 + 构建本仓

```powershell
# 在 OrangeGames 仓根
cmake -S . -B build `
  -DCMAKE_PREFIX_PATH="D:/sdk/orange-engine;D:/sdk/orange-render;D:/3rdparty/install;D:/3rdparty" `
  -DORANGE_ENGINE_SHADER_DIR="../OrangeEngine/build/bin/Debug/shaders/orange_engine"
cmake --build build --config Debug -j
```

- `CMAKE_PREFIX_PATH`：依次让 `find_package` 找到 OrangeEngine / OrangeRender / 第三方。
- `ORANGE_ENGINE_SHADER_DIR`：**shader 缺口的 workaround**——指向引擎 build tree 里
  编出的 `shaders/orange_engine` 目录，build 时 post-build copy 到游戏 `.exe` 旁。
  引擎修好 shader install 后即可删掉此参数。

## 运行

```powershell
build\bin\Debug\spike-01-blob.exe
```

当前 `spike-01-blob` 是**可编骨架**：正交 2D 视角，debug draw 勾出 Spike 1 场景轮廓
（平地 / 平台 / 窄缝 / 边沿）+ 一个占位方块。手感 Loop A / 软体 blob / 调参面板尚未落地，
路线见 `../prototypes/spike-01-blob-feel.md`。

## 目录约定

```
OrangeGames/
├─ CMakeLists.txt                 # 根构建：find_package(OrangeEngine) + add_subdirectory
├─ docs/building.md               # 本文件
├─ prototypes/                    # 一次性手感 / 技术 spike（设计 + 代码同处）
│  ├─ phase0-pillars.md           # 首游设计宪法
│  ├─ spike-01-blob-feel.md       # Spike 1 执行规格
│  └─ spike-01-blob/              # Spike 1 代码
│     ├─ CMakeLists.txt
│     └─ main.cpp
└─ games/                         # （留给后续真正的游戏子项目，尚未开工）
```
