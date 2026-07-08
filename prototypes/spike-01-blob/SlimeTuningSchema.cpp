// SlimeTuningComponent 的 Inspector schema 注册（editor-only，照 HealthComponent 模式）。
// Range/DragSpeed 给每个手感字段 slider 上下限 + 拖拽步进；Addable/Removable 挂
// component 级 +Add / 右键移除。
//
// builder 链是单一真相源（RegisterSlimeTuningSchemaInto），SlimeEditor 静态宿主与
// slime.dll DLL 宿主两条路径共用——只是注册进哪个 registry 不同（见 SlimeTuningSchema.h）。

#include "SlimeTuningSchema.h"
#include "SlimeTuningComponent.h"

#include <schema/ComponentSchemaRegistry.h> // orange_editor SDK 头树（include/orange_editor 在 include path）

namespace spike01
{

    void RegisterSlimeTuningSchemaInto(Orange::Editor::Schema::ComponentSchemaRegistry& reg)
    {
        using namespace Orange::Editor::Schema;

        ComponentSchemaBuilder<SlimeTuningComponent>("SlimeTuningComponent", "Slime Tuning")
            .Field<&SlimeTuningComponent::blobRadius>("blobRadius", "Blob Radius")
            .Range(0.2f, 1.2f)
            .DragSpeed(0.005f)
            .Field<&SlimeTuningComponent::jumpSpeed>("jumpSpeed", "Jump Speed")
            .Range(4.0f, 30.0f)
            .DragSpeed(0.1f)
            .Field<&SlimeTuningComponent::maxRunSpeed>("maxRunSpeed", "Max Run Speed")
            .Range(2.0f, 20.0f)
            .DragSpeed(0.1f)
            .Field<&SlimeTuningComponent::riseGravity>("riseGravity", "Rise Gravity")
            .Range(10.0f, 120.0f)
            .DragSpeed(0.5f)
            .Field<&SlimeTuningComponent::fallGravity>("fallGravity", "Fall Gravity")
            .Range(10.0f, 150.0f)
            .DragSpeed(0.5f)
            .Addable()
            .Removable()
            .RegisterInto(reg);
    }

    void UnregisterSlimeTuningSchema(Orange::Editor::Schema::ComponentSchemaRegistry& reg)
    {
        reg.Unregister<SlimeTuningComponent>();
    }

} // namespace spike01
