// Slime.exe —— 史莱姆发布瘦 runtime（PIE M10 级别 1，ADR-021）。
//
// 不带编辑器的发布宿主：RuntimeHost::Run(SlimeGameModule, cfg) 建窗 + 主循环 + 装配
// Physics/Audio + 驱动 SlimeGameModule 直接跑游戏。**与 SlimeEditor 的 Play 装配收敛
// 同源**（两宿主共用引擎层 PlayAssembly helper：PopulatePhysicsFromWorld /
// StepSimulation / InstantiateAudioSources），消灭"编辑器能跑、发布行为不同"漂移。
//
// 与 spike-01-blob.exe（bespoke standalone main，含 demo reel / showcase）区别：本 exe
// 是 M10 收敛同源的**正式**瘦 runtime——游戏逻辑全在 SlimeGameModule，main 只填 config +
// seed 世界。onSeedWorld 与 SlimeEditorMain 同款（"Slime" 实体 + 默认 SlimeTuningComponent），
// 保证编辑器里调好的手感在发布态一致生效（SlimeGameModule::ApplyTuningOverrides 读它）。

#include "SlimeGameModule.h"
#include "SlimeTuningComponent.h"

#include <orange/engine/runtime/RuntimeHost.h>
#include <orange/engine/scene/NameComponent.h>
#include <orange/engine/scene/TransformComponent.h>
#include <orange/engine/scene/World.h>

int main()
{
    spike01::SlimeGameModule module;

    Orange::Engine::Runtime::RuntimeConfig cfg;
    cfg.windowTitle = "Slime";

    // 世界 seed：与 SlimeEditorMain 同款——名为 "Slime" 的实体带默认手感组件。
    // SlimeGameModule::OnEnterPlay → ApplyTuningOverrides 读它覆盖内置默认值，
    // 再建 control point / Blob（编辑器调好的手感在发布态一致生效）。
    cfg.onSeedWorld = [](Orange::Engine::World& w)
    {
        using namespace Orange::Engine;
        const Entity e = w.CreateEntity();
        w.AddComponent<Scene::TransformComponent>(e, Scene::TransformComponent{});
        w.AddComponent<Scene::NameComponent>(e, Scene::NameComponent{"Slime"});
        w.AddComponent<spike01::SlimeTuningComponent>(e, spike01::SlimeTuningComponent{});
    };

    return Orange::Engine::Runtime::Run(module, cfg);
}
