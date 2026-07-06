// SlimeTuningComponent —— 史莱姆手感调参组件（ECS 组件化，PIE M4，ADR-021）。
//
// 把原先内置在 SlimeGameModule::mParams / mBlobParams 里的手感常量提到一个
// ECS 组件上，让编辑器 Inspector 可视化调参：Edit 世界种一个挂本组件的实体，
// 用户拖字段，Play 时 SlimeGameModule::OnEnterPlay 读回覆盖内置默认值。
//
// 分层纪律：本头 + .cpp 是**引擎-only**（只 include 引擎公共头，不碰编辑器头），
// 故两个 target（standalone spike-01-blob 与 SlimeEditor）都能编。Inspector schema
// 注册（需编辑器头）拆到 SlimeTuningSchema.h/.cpp，只进 SlimeEditor。
//
// 字段是"最直观的手感旋钮"子集，不是 FeelParams 全量——只暴露真机微调时最常动的几个。

#ifndef SPIKE01_SLIME_TUNING_COMPONENT_H
#define SPIKE01_SLIME_TUNING_COMPONENT_H

#include <orange/engine/scene/ComponentSerializerEntry.h>

namespace spike01
{

    struct SlimeTuningComponent
    {
        float blobRadius  = 0.42f; // 史莱姆半径（最直观：改 blob rest 几何 + SDF 渲染半径）
        float jumpSpeed   = 16.0f; // 跳跃初速度
        float maxRunSpeed = 9.0f;  // 最大跑速
        float riseGravity = 45.0f; // 上升重力
        float fallGravity = 70.0f; // 下落重力
    };

    // 场景序列化器 accessor（引擎-only，两 target 共用）。填入宿主 extraSerializers
    // 令本组件随场景 / Play-Stop 快照 round-trip（否则进 Play 时组件丢失）。
    // 返回的 entry 是 program 生命周期的 static，可直接取址填 span。
    const Orange::Engine::Scene::ComponentSerializerEntry& GetSlimeTuningSerializerEntry();

} // namespace spike01

#endif // SPIKE01_SLIME_TUNING_COMPONENT_H
