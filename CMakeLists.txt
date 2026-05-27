# OrangeGames —— 基于 OrangeEngine 的游戏 / demo monorepo 根构建。
#
# 拓扑：ADR-009 sibling。本仓经 find_package(OrangeEngine CONFIG) 消费引擎，
# 不在 tree 内持有引擎源码。引擎需先 install 成可被 find_package 的 SDK
# —— 这是本仓 build 的前置，详见 docs/building.md。
cmake_minimum_required(VERSION 3.28)

project(OrangeGames
    VERSION 0.0.1
    DESCRIPTION "Games and demos built on OrangeEngine"
    LANGUAGES CXX)

# 外部消费者自设 C++20 —— 引擎的 in-tree cmake helper
# (orange_engine_set_compiler_options) 不在 OrangeEngineConfig 导出，
# 见 OrangeEngine GAP-2026-05-27-builtin-shaders-not-installed-for-consumers。
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 多配置生成器 (VS) 下统一 runtime 输出，方便 shader / dll 落到 .exe 旁。
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# 消费引擎。0.1 + SameMinorVersion：接受 0.1.x。SDK 路径由消费者经
# CMAKE_PREFIX_PATH 指向（典型：D:/sdk/orange-engine;D:/sdk/orange-render;D:/3rdparty）。
find_package(OrangeEngine 0.1 CONFIG REQUIRED)

# 首游 Spike 1 原型（流体史莱姆手感）。后续真正的游戏子项目按
# games/<name>/ 平铺，prototypes/ 留给一次性手感 / 技术 spike。
add_subdirectory(prototypes/spike-01-blob)
