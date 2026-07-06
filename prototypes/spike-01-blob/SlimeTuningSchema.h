// SlimeTuningSchema —— 把 SlimeTuningComponent 注册进编辑器 Inspector schema（editor-only）。
//
// 与 SlimeTuningComponent.h/.cpp（引擎-only）分开：schema 注册需要编辑器头
// <schema/ComponentSchemaRegistry.h>，只能在链 orange_editor 的 SlimeEditor 里编，
// standalone spike-01-blob 编不了。ADR-021 定 schema 是编辑器层概念（不进 IGameModule）。

#ifndef SPIKE01_SLIME_TUNING_SCHEMA_H
#define SPIKE01_SLIME_TUNING_SCHEMA_H

namespace spike01
{

    // 经 EditorAppConfig::onRegisterSchemas 在 main 启动期调一次：把
    // SlimeTuningComponent 的字段注册进全局 ComponentSchemaRegistry，令 Inspector
    // 能画字段 + "+ Add Component" 菜单能加。
    void RegisterSlimeTuningSchema();

} // namespace spike01

#endif // SPIKE01_SLIME_TUNING_SCHEMA_H
