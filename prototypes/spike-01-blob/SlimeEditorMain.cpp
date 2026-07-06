// SlimeEditor.exe —— 史莱姆 per-game editor（PIE M4，ADR-021 双语言玩法宿主）。
//
// 瘦 main：把 SlimeGameModule 注入 orange_editor 宿主（EditorAppConfig.modules），
// 编辑器里 Play → 史莱姆可操控、SDF metaball 在视口渲染、Stop 还原。全部装配 /
// 主循环 / 关停在 orange_editor lib 的 RunEditorApp 里（find_package(OrangeEditor)
// 消费），本 exe 只负责注入自家玩法模块——不复制编辑器 main。
//
// 与 spike-01-blob.exe（standalone runtime）共用同一份 SlimeGameModule.cpp +
// SlimeMetaballPass.cpp + shaders；差别仅宿主（编辑器 vs 独立窗口）。
#include <EditorApp.h> // orange_editor SDK 公共头（含 EditorAppConfig）

#include "SlimeGameModule.h"

#include <memory>

int main(int argc, char** argv)
{
    Orange::Editor::EditorAppConfig cfg;
    cfg.windowTitle = "SlimeEditor";
    // projectRoot 留空：史莱姆关卡由 SlimeGameModule 在 OnEnterPlay 代码生成
    //（spike-01 无 .scene.json），无 OG 场景资产依赖；codicon / editor shader /
    // 引擎 shader 全走 exe-相对 copy-helper（CMake POST_BUILD 拷到 exe 旁），
    // 与 cwd 无关。启动即空世界，Play 出史莱姆。
    cfg.modules.push_back(std::make_unique<spike01::SlimeGameModule>());
    return Orange::Editor::RunEditorApp(argc, argv, std::move(cfg));
}
