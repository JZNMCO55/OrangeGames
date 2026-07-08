# platformer-test 场景（2026-07-09）

`assets/scenes/platformer-test.scene.json` —— 平台跳跃测试场景，把 SlimeGameModule
内置物理关卡（`MakeLevel()` 的 9 个 box：平地 / 左右平台 / 墙跳 chimney / squeeze
隧道）**可视化实体化**：每个物理箱对应一个带 Renderable + RigidBody(Static) +
Collider(Box) 的实体，几何与模块物理逐一重合，另加夜景环境（preller_drive HDR
天空盒）+ 月光平行光 + 游戏相机。

用法：共享 OrangeEditor（`OrangeEngine/build-shared/bin/Release/OrangeEditor.exe
--project slime.orangeproject`）File→Open 本文件（或 MCP `open_scene` 绝对路径）→
Play。史莱姆落在可见地面上，跳跃/墙爬/挤压测试点与模块关卡一致。

注意：
- 模块的隐形物理关卡与本场景共存（同位置静态体重叠无副作用）；场景 Collider
  的 categoryBits=1（默认），墙爬的 kCatWall 判定实际由模块自己的墙体提供。
  后续若模块改为"场景有静态碰撞体则跳过 MakeLevel"，本场景即是唯一关卡源。
- 场景资产引用（cube.mesh / pbr.material / preller_drive_1k.hdr）走 `project://`
  前缀，由编辑器 projectRoot 解析。
