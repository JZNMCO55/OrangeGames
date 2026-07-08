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

#include <glm/glm.hpp>

namespace spike01
{

    struct SlimeTuningComponent
    {
        // —— 手感（OnEnterPlay 一次性读取；改后需重进 Play）——
        float blobRadius  = 0.42f; // 史莱姆半径（最直观：改 blob rest 几何 + SDF 渲染半径）
        float jumpSpeed   = 16.0f; // 跳跃初速度
        float maxRunSpeed = 9.0f;  // 最大跑速
        float riseGravity = 45.0f; // 上升重力
        float fallGravity = 70.0f; // 下落重力

        // —— 视觉（Tier2 SDF 渲染；Play 期每帧重读 + schema 标 PlaySafe = 实时调）——
        // 默认值 = 2026-07-08 编辑器 2D 机位 + 暗场下 MCP live-tuning 对参考图收敛值
        //（与 SlimeMetaballPass::Tunables 默认值保持同步）。
        glm::vec3 bodyColor  = {0.14f, 0.72f, 0.23f}; // 主体绿（薄处 / 边亮端）
        glm::vec3 deepColor  = {0.03f, 0.22f, 0.07f}; // 核心深绿（厚处 / SSS 暗端）
        glm::vec3 rimColor   = {0.62f, 1.00f, 0.50f}; // Fresnel 亮边
        glm::vec3 eyeColor   = {1.00f, 0.92f, 0.45f}; // 发光眼
        glm::vec3 speckColor = {0.85f, 1.00f, 0.42f}; // 体内光斑
        glm::vec3 glowColor  = {0.35f, 0.95f, 0.30f}; // 底部接触辉光
        float     domeScale  = 1.38f;                 // 半球鼓度（盖住剪影侧带，贴合参考图）
        float     rimGain    = 0.95f;                 // 亮边强度（>1.2 会被 bloom 吹成肥晕）
        float     specGain   = 1.15f;                 // 湿润高光强度
        float     ambient    = 0.45f;                 // 环境光基线
        float     eyeGain    = 3.0f;                  // 眼 emissive 强度
        float     speckGain  = 1.8f;                  // 光斑 emissive 强度
        float     glowGain   = 1.3f;                  // 接触辉光 emissive 强度
    };

    // 场景序列化器 accessor（引擎-only，两 target 共用）。填入宿主 extraSerializers
    // 令本组件随场景 / Play-Stop 快照 round-trip（否则进 Play 时组件丢失）。
    // 返回的 entry 是 program 生命周期的 static，可直接取址填 span。
    const Orange::Engine::Scene::ComponentSerializerEntry& GetSlimeTuningSerializerEntry();

} // namespace spike01

#endif // SPIKE01_SLIME_TUNING_COMPONENT_H
