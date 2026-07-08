// SlimeEditor.exe —— 史莱姆 per-game editor（PIE M4，ADR-021 双语言玩法宿主）。
//
// 瘦 main：把 SlimeGameModule 注入 orange_editor 宿主（EditorAppConfig.modules），
// 编辑器里 Play → 史莱姆可操控、SDF metaball 在视口渲染、Stop 还原。全部装配 /
// 主循环 / 关停在 orange_editor lib 的 RunEditorApp 里（find_package(OrangeEditor)
// 消费），本 exe 只负责注入自家玩法模块——不复制编辑器 main。
//
// 与 spike-01-blob.exe（standalone runtime）共用同一份 SlimeGameModule.cpp +
// SlimeMetaballPass.cpp + shaders；差别仅宿主（编辑器 vs 独立窗口）。
#include <EditorApp.h>              // orange_editor SDK 公共头（含 EditorAppConfig）
#include <project/ProjectConfig.h>  // M6：slime.orangeproject → EditorAppConfig 路径解析桥

#include "SlimeGameModule.h"
#include "SlimeTuningComponent.h"
#include "SlimeTuningSchema.h"

#include <schema/ComponentSchemaRegistry.h> // 静态宿主注册进编辑器全局 Instance()

#include <orange/engine/scene/NameComponent.h>
#include <orange/engine/scene/TransformComponent.h>
#include <orange/engine/scene/World.h>

#include <filesystem>
#include <memory>
#include <string>
#include <utility>

#if defined(_WIN32)
#include <windows.h> // GetModuleFileNameW —— 定位 exe 旁 slime.orangeproject
#endif

int main(int argc, char** argv)
{
    Orange::Editor::EditorAppConfig cfg;
    // 玩法侧代码注入（与项目文件正交）：SlimeGameModule + Inspector schema + Edit 世界 seed。
    cfg.modules.push_back(std::make_unique<spike01::SlimeGameModule>());

    // Inspector schema：让 SlimeTuningComponent 能画字段 + "+ Add Component" 菜单可加。
    // 静态宿主注册进编辑器全局 Instance()（DLL 宿主走 slime.dll 导出的 schema proc）。
    cfg.onRegisterSchemas = []
    { spike01::RegisterSlimeTuningSchemaInto(Orange::Editor::Schema::ComponentSchemaRegistry::Instance()); };

    // Edit 世界 seed：一个名为 "Slime" 的实体，带默认手感 SlimeTuningComponent。
    // 用户在 Entity Tree 选中它 → Inspector 拖手感字段 → Play 时 SlimeGameModule
    // 读回覆盖内置默认值。Transform + Name 让它在 Entity Tree 正常显示 / 可选中。
    cfg.onSeedWorld = [](Orange::Engine::World& w)
    {
        using namespace Orange::Engine;
        const Entity e = w.CreateEntity();
        w.AddComponent<Scene::TransformComponent>(e, Scene::TransformComponent{});
        w.AddComponent<Scene::NameComponent>(e, Scene::NameComponent{"Slime"});
        w.AddComponent<spike01::SlimeTuningComponent>(e, spike01::SlimeTuningComponent{});
    };

    // M6：从 exe 旁的 slime.orangeproject 解析 name / projectRoot / 模块引用填 config——
    // windowTitle="Slime"（项目名驱动，取代硬编码 "SlimeEditor"）+ projectRoot=exe 目录
    //（chdir 过去，取代原空 projectRoot 触发的 ChdirToRepoRoot 探测）。CMake POST_BUILD
    // 把 slime.orangeproject 拷到 exe 旁。ApplyProjectFileToConfig 只填空字段，故上面
    // 代码注入的 modules / onRegisterSchemas / onSeedWorld 不受影响；无 startupScene →
    // isPerGameEditor 空世界 + onSeedWorld 种 Slime（行为不变）。
#if defined(_WIN32)
    {
        wchar_t     exeW[MAX_PATH] = {};
        const DWORD n              = GetModuleFileNameW(nullptr, exeW, MAX_PATH);
        if (n > 0 && n < MAX_PATH)
        {
            const std::filesystem::path projPath =
                std::filesystem::path(exeW).parent_path() / "slime.orangeproject";
            Orange::Editor::Project::ApplyProjectFileToConfig(projPath.string(), cfg);
            // SlimeEditor 静态注入 SlimeGameModule（上文 cfg.modules）——slime.orangeproject
            // 现声明 kind="dll"（M7 主路径 = 共享 SHARED OrangeEditor 热重载加载 slime.dll）。
            // 本 static per-game editor 须清掉解析出的 DLL 路径，否则静态 + DLL 双份
            // SlimeGameModule 并存驱动。SlimeEditor 自 M7 起降级为静态备用。
            cfg.dllGameModulePaths.clear();
        }
    }
#endif

    return Orange::Editor::RunEditorApp(argc, argv, std::move(cfg));
}
