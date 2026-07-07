// M7：把 SlimeGameModule 导出为 game.dll 的工厂入口，供共享 OrangeEditor 运行时
// 经 GameModuleLibrary::Load 动态加载（LoadLibrary → OrangeCreateGameModule）。
//
// 只在 slime_game_module（MODULE / slime.dll）target 里编译；SlimeEditor.exe
// 仍静态链 SlimeGameModule.cpp，不编本文件——否则 exe 也导出工厂、语义重复。

#include "SlimeGameModule.h"

#include <orange/engine/game/GameModuleLibrary.h>

ORANGE_EXPORT_GAME_MODULE(spike01::SlimeGameModule)
