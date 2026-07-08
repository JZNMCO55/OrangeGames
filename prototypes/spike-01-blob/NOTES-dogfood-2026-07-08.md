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

## 2026-07-08 下午 · 真机 dogfood 实录（用户特批同 session 跨仓 hotfix）

首次真人交互桌面 dogfood 即抓到 3 个真 bug，全部当日修复 + 验证：

1. **史莱姆双视口不可见（本仓，SlimeGameModule.cpp）**：M9.3 双视口后编辑器对 Scene / Game
   两条 pipeline 各调一次 `RegisterRenderPasses`，单成员 `mpSdfPass` 被第二次注册覆盖——
   Scene 视口那份 pass 永远收不到 `SetBlob`/`SetEnabled`，Play 后 Scene 里看不到史莱姆
   （Game 视口又因 slime 世界无 Camera 组件而空）。M4 dogfood（7/6）早于 M9.3（7/7），
   五 session 全 headless 没人看 GUI，首次真机即暴露。修复：`mSdfPasses` per-pipeline 记账
   （`{Pipeline*, Pass*}` 列表），Tick / OnEnterPlay / OnExitPlay / Droplets 喂全部实例，
   `UnregisterRenderPasses(pipeline)` 只摘对应条目。**static SlimeEditor + SHARED OrangeEditor
   （DLL 路径）双双真机截图验证绿史莱姆 Play 渲染恢复。**
2. **SHARED OrangeEditor 真桌面起不来（OrangeRender）**：`orange_engine.dll` 与
   `orange_render.dll` 各静态持一份 GLFW，render 侧副本未 init → `glfwCreateWindowSurface`
   返回 -3。上个 session 误归因为"后台会话无窗口"。修法 = OR platform 层 pNativeWindow 双形态
   （HWND 直走 `vkCreateWin32SurfaceKHR`）+ OE 装配处改传 HWND。见
   `OrangeRender/docs/incoming_bugs.md` BUG-2026-07-08-shared-topology-glfw-state-split（已归档）。
3. **SHARED 编辑器首帧虚表错位崩溃（构建拓扑）**：`D:/3rdparty/install` 藏着 2026-05-30 的
   过期 OrangeRender SDK（pre-c7，`RHIDevice` 33 virtual），与 SHARED 链路的
   `orange-render-shared`（c7 后，34 virtual）同时暴露在 include 顺序里——个别 TU 编到旧头，
   虚调用 `GetCapabilities`（新布局 slot 32）打进 slot 31 的 `SerializePipelineCache`，
   AV 于返回槽写入。修法 = 3rdparty prefix 的 OR 刷到 HEAD（static 链路借此完成拖欠的
   7bc8c57 → c7-HEAD 消费 bump）+ 全量重编两条链路。**教训：同一依赖绝不允许两个版本同时
   出现在任何构建的 prefix 集合里；LNK4098（MSVCRTD 混入 Release）同源，一并留意。**

验证矩阵（全绿）：OE static ctest 122/122、OR static ctest 87/87、OG ctest 1/1、
SHARED OrangeEditor 真机 Play + slime.dll 渲染截图、static SlimeEditor 真机 Play 截图。

**新遗留（已登记 OR incoming_bugs 置顶）**：OR 升 HEAD 后 static Debug 编辑器启动期
约 1/3 概率 AV（c7 VulkanDedup compute pipeline cache key 路径读到垃圾 shader module 指针，
`BUG-2026-07-08-compute-pipeline-dedup-startup-av`）；SHARED Release 链未复现，不阻塞热重载主路径。

## 剩余（非本 dogfood 范畴）

- ~~**game.dll schema 注册通道**（ADR-023 第二条正交硬工作，未做）~~ **已落地**（2026-07-08
  凌晨 session，OE 7c19d7c + OG f67fab9：`extern "C" OrangeRegister/UnregisterEditorSchemas`）；
  当日下午真机验证共享 OrangeEditor 日志「DLL 游戏模块 schema 已注册（库 0）」。
- **SlimeEditor.exe 静态备用**（Debug）构建须钉 Debug render SDK：`-DOrangeRender_DIR=D:/3rdparty/install/lib/cmake/OrangeRender`（避 Debug obj 撞 `D:/sdk/orange-render` 的 Release CRT，LNK2038；LNK1319「N mismatches」会掩盖真错，`/INCREMENTAL:NO` 可暴露）。
