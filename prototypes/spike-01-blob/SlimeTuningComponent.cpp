// SlimeTuningComponent 序列化器实现（引擎-only，照 OrangeEditor demo_game/HealthComponent 模式）。
// 5 个 float 字段走 JsonWriter::WriteFloat / JsonReader::ReadFloat（引擎接口按 double 收发，
// 读写各做一次 float↔double 转换）+ SchemaVersion 版本门。不含任何编辑器头。

#include "SlimeTuningComponent.h"

#include <orange/engine/scene/World.h>

#include <string>
#include <string_view>

namespace spike01
{
    namespace
    {

        const Orange::Engine::SchemaVersion kSlimeTuningSchemaVersion{
            "spike01/SlimeTuningComponent", 1, 0};

        // componentPath + "/" + key（如 "components/SlimeTuning" + "jumpSpeed"）。
        std::string Join(std::string_view base, std::string_view key)
        {
            std::string s;
            s.reserve(base.size() + 1 + key.size());
            s.append(base);
            s += '/';
            s.append(key);
            return s;
        }

        bool SlimeTuningHas(const Orange::Engine::World& world, Orange::Engine::Entity entity)
        {
            return world.HasComponent<SlimeTuningComponent>(entity);
        }

        void SlimeTuningWrite(Orange::Engine::JsonWriter&               writer,
                              std::string_view                          componentPath,
                              Orange::Engine::Entity                    entity,
                              const Orange::Engine::Scene::SaveContext& ctx)
        {
            const auto* c = ctx.world.GetComponent<SlimeTuningComponent>(entity);
            if (c == nullptr)
            {
                return;
            }

            writer.WriteSchemaVersion(Join(componentPath, "schema"), kSlimeTuningSchemaVersion);
            writer.WriteFloat(Join(componentPath, "blobRadius"), c->blobRadius);
            writer.WriteFloat(Join(componentPath, "jumpSpeed"), c->jumpSpeed);
            writer.WriteFloat(Join(componentPath, "maxRunSpeed"), c->maxRunSpeed);
            writer.WriteFloat(Join(componentPath, "riseGravity"), c->riseGravity);
            writer.WriteFloat(Join(componentPath, "fallGravity"), c->fallGravity);
        }

        bool SlimeTuningRead(const Orange::Engine::JsonReader&         reader,
                             std::string_view                          componentPath,
                             Orange::Engine::Entity                    entity,
                             const Orange::Engine::Scene::LoadContext& ctx)
        {
            auto svResult = reader.ReadSchemaVersion(Join(componentPath, "schema"));
            if (svResult.IsErr())
            {
                return false;
            }
            if (!kSlimeTuningSchemaVersion.CanRead(svResult.Value()))
            {
                return false;
            }

            // 缺字段保留组件默认值（ReadFloat 失败不改 out）。double 中转后回 float。
            SlimeTuningComponent c{};
            double               blobRadius  = c.blobRadius;
            double               jumpSpeed   = c.jumpSpeed;
            double               maxRunSpeed = c.maxRunSpeed;
            double               riseGravity = c.riseGravity;
            double               fallGravity = c.fallGravity;
            reader.ReadFloat(Join(componentPath, "blobRadius"), blobRadius);
            reader.ReadFloat(Join(componentPath, "jumpSpeed"), jumpSpeed);
            reader.ReadFloat(Join(componentPath, "maxRunSpeed"), maxRunSpeed);
            reader.ReadFloat(Join(componentPath, "riseGravity"), riseGravity);
            reader.ReadFloat(Join(componentPath, "fallGravity"), fallGravity);
            c.blobRadius  = static_cast<float>(blobRadius);
            c.jumpSpeed   = static_cast<float>(jumpSpeed);
            c.maxRunSpeed = static_cast<float>(maxRunSpeed);
            c.riseGravity = static_cast<float>(riseGravity);
            c.fallGravity = static_cast<float>(fallGravity);

            ctx.world.AddComponent<SlimeTuningComponent>(entity, c);
            return true;
        }

    } // namespace

    const Orange::Engine::Scene::ComponentSerializerEntry& GetSlimeTuningSerializerEntry()
    {
        using Orange::Engine::Scene::ComponentKind;
        using Orange::Engine::Scene::ComponentSerializerEntry;
        static const ComponentSerializerEntry kEntry{
            .name  = "SlimeTuning",
            .kind  = ComponentKind::PureData,
            .Has   = SlimeTuningHas,
            .Write = SlimeTuningWrite,
            .Read  = SlimeTuningRead,
        };
        return kEntry;
    }

} // namespace spike01
