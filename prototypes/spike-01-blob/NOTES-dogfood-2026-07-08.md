# NOTES · M7 DLL 热重载 dogfood（2026-07-08）

PIE M7「共享 OrangeEditor + slime.dll 热重载」真机 dogfood 记录。**GUI dogfood 需交互桌面**——背景 agent 会话无 Vulkan 窗口 surface（`Renderer::Initialize failed`），下述可视化步骤留真人。

## 背景会话已验证（headless / 构建 / 加载路径）

- `dll_game_module_test` PASS（ctest 120/120）：load/unload/**reload×3**/多实例/错误路径/宿主集成/**热重载后 pass 数不累积**（旧注销+新注册=1）/**stale 检测**（推 dll mtime 未来→IsSourceStale）。
- slime.dll 构建成功（112KB，SHARED engine+render SDK，Release）；`dumpbin /EXPORTS` 确认导出 `OrangeCreateGameModule` + `OrangeDestroyGameModule`。
- 共享 SHARED OrangeEditor.exe **真机解析 slime.orangeproject 的 DLL-ref + 项目 'Slime' 加载**（日志「项目声明 DLL 游戏模块：slime.dll」+「项目 'Slime' 加载 projectRoot=...」）；仅卡在无窗口 surface 的 Renderer 初始化（装配顺序 Renderer 先于 gameModules，故 slime.dll 实际 LoadLibrary 未到达）。

## 构建 slime.dll（SHARED SDK，Release）

⚠️ **SDK config**：SHARED 链路全 Release。OG configure 需 `find_package(OrangeEditor)`，故先把 orange_editor 装进 SHARED engine SDK：

```
# 1. build-shared 开 editor install + reinstall SHARED engine SDK（含 orange_editor lib）
cmake -S OrangeEngine -B OrangeEngine/build-shared -DORANGE_EDITOR_INSTALL=ON
cmake --install OrangeEngine/build-shared --config Release --prefix D:/sdk/orange-engine-shared
# 2. OG build-shared configure（SHARED SDK）+ build slime.dll
cmake -S OrangeGames -B OrangeGames/build-shared -DSPIKE01_BUILD_SLIME_DLL=ON \
  -DOrangeEngine_DIR=D:/sdk/orange-engine-shared/lib/cmake/OrangeEngine \
  -DOrangeEditor_DIR=D:/sdk/orange-engine-shared/lib/cmake/OrangeEditor \
  -DOrangeRender_DIR=D:/sdk/orange-render-shared/lib/cmake/OrangeRender \
  -DCMAKE_PREFIX_PATH="D:/3rdparty/install;D:/3rdparty"
cmake --build OrangeGames/build-shared --config Release --target slime_game_module
```

## 部署到共享 OrangeEditor 旁

共享 OrangeEditor = `OrangeEngine/build-shared/bin/Release/OrangeEditor.exe`（SHARED，Release）。部署到 exe 旁：
- `slime.dll`（`OrangeGames/build-shared/prototypes/spike-01-blob/Release/slime.dll`）
- `slime.orangeproject`（ref="slime.dll" 相对）
- slime SDF shaders → exe 旁 `shaders/`（RegisterRenderPasses 时 SlimeMetaballPass 加载；build POST_BUILD 已编到 slime.dll 旁）
- `orange_engine.dll` + `orange_render.dll`（SHARED，slime.dll 依赖）已在 exe 旁

## dogfood 步骤（真人，交互桌面）

1. `OrangeEditor.exe --project slime.orangeproject` → 编辑器起窗口，标题 "Slime"，日志「[GameModuleLibrary] 已加载游戏模块 'SlimeGameModule'」。
2. **Play** → 绿色 SDF 史莱姆在视口渲染（M4 同款画面，但经 DLL 加载而非静态链）。
3. **Stop** → 史莱姆消失、还原空世界。
4. 改一行手感代码（如 `SlimeGameModule.cpp` 的手感参数、或 `SlimeMetaballPass` 着色）→ 重编 slime.dll（上述 build 命令）→ 覆盖 exe 旁 slime.dll。
5. 编辑器**不重启**：工具栏 `[R]` 按钮应变 accent 橙（file watcher 检测原 dll mtime 变新 = "已过期"）。
6. 点 `[R]` → 日志「[OrangeEditor] DLL 游戏模块热重载完成：1/1 库成功」。
7. 再 **Play** → 新手感 / 新渲染生效，全程编辑器未重启。

**验收 = 通过**：改一行手感代码重编 DLL → 编辑器不重启重载再 Play 生效。

## 剩余（非本 dogfood 范畴）

- **game.dll schema 注册通道**（ADR-023 第二条正交硬工作，未做）：DLL slime 的 `SlimeTuningComponent` 在共享 OrangeEditor 下**无 Inspector schema**——schema 现靠 per-game `SlimeEditor` 的 `onRegisterSchemas`，DLL 组件需 `extern "C"` schema 入口 + 卸载前 registry 清空。故本 dogfood 验的是热重载**手感代码**，不含 Inspector 调参。
- **SlimeEditor.exe 静态备用**（Debug）构建须钉 Debug render SDK：`-DOrangeRender_DIR=D:/3rdparty/install/lib/cmake/OrangeRender`（避 Debug obj 撞 `D:/sdk/orange-render` 的 Release CRT，LNK2038；LNK1319「N mismatches」会掩盖真错，`/INCREMENTAL:NO` 可暴露）。
