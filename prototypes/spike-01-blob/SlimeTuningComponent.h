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
        glm::vec3 bodyColor  = {0.13f, 0.66f, 0.20f}; // 主体绿（下半通透亮端；梯度结构撑住后可抬亮）
        glm::vec3 deepColor  = {0.03f, 0.26f, 0.08f}; // 上半深绿（垂直梯度深端 + 边缘吸收带色）
        glm::vec3 rimColor   = {0.62f, 1.00f, 0.50f}; // Fresnel 亮边
        glm::vec3 eyeColor   = {1.00f, 0.92f, 0.45f}; // 发光眼
        glm::vec3 speckColor = {1.00f, 0.86f, 0.34f}; // 体内金斑（参考图偏金非黄绿）
        glm::vec3 glowColor  = {0.35f, 0.95f, 0.30f}; // 底部接触辉光 + 体内底部光池
        float     domeScale  = 1.38f;                 // 半球鼓度（盖住剪影侧带，贴合参考图）
        float     rimGain    = 0.85f;                 // 亮边强度（全透体后轮廓靠边线+bloom；>1.0 晕过肥）
        float     specGain   = 1.50f;                 // 湿润高光强度（锐点需更亮才可读）
        float     ambient    = 0.40f;                 // 环境光基线（压低让垂直梯度对比可读）
        float     eyeGain    = 3.0f;                  // 眼 emissive 强度
        float     speckGain  = 2.3f;                  // 光斑 emissive 强度
        float     glowGain   = 1.0f;                  // 接触辉光 emissive 强度（与 core/pool 叠加，>1.2 白洗）
        float     opacity    = 0.40f;                 // 体内不透明度（透明是"透亮"主体；边缘吸收带回实）
        float     coreGain   = 1.0f;                  // 内芯发光核强度（暖色主要走乘性梯度，additive 只点睛）
    };

    // 场景序列化器 accessor（引擎-only，两 target 共用）。填入宿主 extraSerializers
    // 令本组件随场景 / Play-Stop 快照 round-trip（否则进 Play 时组件丢失）。
    // 返回的 entry 是 program 生命周期的 static，可直接取址填 span。
    const Orange::Engine::Scene::ComponentSerializerEntry& GetSlimeTuningSerializerEntry();

} // namespace spike01

#endif // SPIKE01_SLIME_TUNING_COMPONENT_H
