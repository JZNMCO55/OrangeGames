// SlimeTuningSchema —— 把 SlimeTuningComponent 注册进编辑器 Inspector schema（editor-only）。
//
// 与 SlimeTuningComponent.h/.cpp（引擎-only）分开：schema 注册需要编辑器头
// <schema/ComponentSchemaRegistry.h>，只能在链 orange_editor（或接其头树）的 target 里编，
// standalone spike-01-blob 编不了。ADR-021 定 schema 是编辑器层概念（不进 IGameModule）。
//
// 两条消费路径共用同一 builder 链（DRY）：
//   * SlimeEditor（静态宿主）：main 启动期 EditorAppConfig::onRegisterSchemas → Register()
//     （注册进编辑器全局 Instance()）。
//   * slime.dll（M7 DLL 宿主）：dll 导出的 OrangeRegisterEditorSchemas 收到共享 OrangeEditor
//     传入的 registry 引用 → RegisterInto(reg)；热重载卸载前 → UnregisterSlimeTuningSchema(reg)。

#ifndef SPIKE01_SLIME_TUNING_SCHEMA_H
#define SPIKE01_SLIME_TUNING_SCHEMA_H

namespace Orange::Editor::Schema
{
    class ComponentSchemaRegistry;
}

namespace spike01
{

    // 把 SlimeTuningComponent 字段注册进指定 registry（builder 链的单一真相源）。
    // 两条宿主路径都调它——只是传入的 registry 不同：SlimeEditor 静态宿主传编辑器全局
    // Instance()（onRegisterSchemas，见 SlimeEditorMain.cpp）；slime.dll 传共享 OrangeEditor
    // 经 schema proc 传入的 registry（见 SlimeSchemaDllExport.cpp）。刻意不提供绑死
    // Instance() 的重载——那会让 slime.dll 也引用 Instance()（其符号在 orange_editor lib，
    // slime.dll 不链），链接 unresolved。
    void RegisterSlimeTuningSchemaInto(Orange::Editor::Schema::ComponentSchemaRegistry& reg);

    // 从指定 registry 注销（slime.dll 热重载卸载前调——摘掉即将随 FreeLibrary 消失的
    // dll 代码的 PropertyDescriptor 函数指针）。
    void UnregisterSlimeTuningSchema(Orange::Editor::Schema::ComponentSchemaRegistry& reg);

} // namespace spike01

#endif // SPIKE01_SLIME_TUNING_SCHEMA_H
