// M7 §②：slime.dll 的编辑器 schema 注册通道导出（DLL 组件 Inspector authoring）。
//
// 共享 OrangeEditor 运行时加载 slime.dll 后 GetProcAddress 这对可选的 extern"C" 入口，
// 用**编辑器自己的** ComponentSchemaRegistry 指针（void*）调它们——DLL 侧 cast 回真实
// 类型，把 SlimeTuningComponent 的 schema 注册进编辑器 registry（Inspector / Add-Component
// 菜单据此 authoring 史莱姆手感组件）。热重载 FreeLibrary 前编辑器调 Unregister 摘掉本
// dll 的 PropertyDescriptor（其函数指针指向即将消失的 dll 代码）。
//
// 只在 slime_game_module（slime.dll）target 里编译；SlimeEditor.exe 走 SlimeTuningSchema.cpp
// 的静态 onRegisterSchemas 路径，不编本文件——否则 exe 也导出、语义重复。
//
// void* 而非 typed 引用：跨 DLL 边界与引擎层 GameModuleLibrary 的 SchemaProc 签名
// （void(*)(void*)）对齐——引擎层刻意不知编辑器类型（header isolation）。

#include "SlimeTuningSchema.h"

#include <schema/ComponentSchemaRegistry.h>

#if defined(_WIN32)
    #define SLIME_DLL_EXPORT __declspec(dllexport)
#else
    #define SLIME_DLL_EXPORT
#endif

extern "C" SLIME_DLL_EXPORT void OrangeRegisterEditorSchemas(void* pRegistry)
{
    auto& reg = *static_cast<Orange::Editor::Schema::ComponentSchemaRegistry*>(pRegistry);
    spike01::RegisterSlimeTuningSchemaInto(reg);
}

extern "C" SLIME_DLL_EXPORT void OrangeUnregisterEditorSchemas(void* pRegistry)
{
    auto& reg = *static_cast<Orange::Editor::Schema::ComponentSchemaRegistry*>(pRegistry);
    spike01::UnregisterSlimeTuningSchema(reg);
}
