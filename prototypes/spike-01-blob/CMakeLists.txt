# Spike 1 —— 流体史莱姆 context-sensitive 手感原型。
# 设计规格见 ../spike-01-blob-feel.md + ../phase0-pillars.md。
#
# 状态：**可编骨架**。当前 main.cpp 只验证"游戏仓能 find_package 引擎 +
# 开窗 + debug draw"这条消费链路；手感 Loop A / 软体 blob / 调参面板尚未
# 落地——调参面板待引擎补完 GAP-2026-05-27-consumer-imgui-tuning-hook
# （消费者 ImGui hook）后再接。
add_executable(spike-01-blob main.cpp SlimeMetaballPass.cpp SlimeTuningComponent.cpp)

# Tier 2 M0：自定义 SDF fragment pass 直接下沉到 RHI（IRenderPass 是引擎明确开放的
# 逃生舱口）。只链公共 SDK OrangeRender::orange_render，绝不碰 internals / backend 私有头。
find_package(OrangeRender CONFIG REQUIRED)
target_link_libraries(spike-01-blob PRIVATE
    OrangeEngine::orange_engine
    OrangeRender::orange_render)

if (MSVC)  # 中文注释源、无 BOM → MSVC 须 /utf-8 避免按码页 936 误读
    target_compile_options(spike-01-blob PRIVATE /utf-8)
endif ()

# ---- builtin shader 运行期依赖（走 SDK copy-helper）----
# M6：删除原 ORANGE_ENGINE_SHADER_DIR 手动 copy workaround（依赖手传引擎 build
# tree 路径的临时件）。引擎 install 已装 builtin .spv（GAP-2026-05-27 已修），
# 直接用 OrangeEngine SDK 的 orange_engine_copy_builtin_shaders helper 把
# shaders/orange_engine/*.spv 拷到 exe 旁——与 SlimeEditor 同款正规 SDK 机制。
orange_engine_copy_builtin_shaders(spike-01-blob)

# ---- Tier 2 SDF shader 编译（GLSL → SPIR-V，落 .exe 旁 shaders/）----
# fullscreen.vert + slime_metaball.frag 由 glslangValidator 编成 .spv，SlimeMetaballPass
# 运行期按 exe 相对路径 shaders/*.spv 加载（照抄 particle_field CMakeLists）。
find_program(SPIKE01_GLSLANG
    NAMES glslangValidator
    HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin")
if (NOT SPIKE01_GLSLANG)
    message(FATAL_ERROR "spike-01-blob: glslangValidator not found under VULKAN_SDK.")
endif ()
add_custom_command(TARGET spike-01-blob POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:spike-01-blob>/shaders"
    COMMAND "${SPIKE01_GLSLANG}" -V
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/fullscreen.vert"
            -o "$<TARGET_FILE_DIR:spike-01-blob>/shaders/fullscreen.vert.spv"
    COMMAND "${SPIKE01_GLSLANG}" -V
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/slime_metaball.frag"
            -o "$<TARGET_FILE_DIR:spike-01-blob>/shaders/slime_metaball.frag.spv"
    VERBATIM
    COMMENT "Compiling slime_metaball shaders")

# ===========================================================================
# slime_game_module（slime.dll）—— SlimeGameModule 的 game.dll 形态（PIE M7，ADR-023）。
#
# 与 SlimeEditor（静态链 SlimeGameModule）并存的 DLL 宿主路径：共享 OrangeEditor
# 运行时经 GameModuleLibrary::Load(slime.dll) 动态加载 → Play 驱动 → Stop 卸载 →
# 重编 → 重载（热重载）。SlimeGameModuleDllExport.cpp 用 ORANGE_EXPORT_GAME_MODULE
# 宏导出 extern"C" OrangeCreateGameModule / OrangeDestroyGameModule。
#
# opt-in（默认 OFF）：只需在针对 **SHARED 引擎 SDK**（-DBUILD_SHARED_LIBS=ON 构建的
# OrangeEngine/OrangeRender）消费时开——那样 slime.dll 经 dllimport 与宿主共享单份
# 引擎（GImGui/Log/EnTT），产物小。对 STATIC 引擎 SDK 开会嵌入整份引擎（巨大且与
# SlimeEditor 双静态），故默认不建。只链 OrangeEngine + OrangeRender，不碰 orange_editor。
# ===========================================================================
option(SPIKE01_BUILD_SLIME_DLL "构建 slime.dll 游戏模块（PIE M7 DLL 宿主；需 SHARED 引擎 SDK）" OFF)
if (SPIKE01_BUILD_SLIME_DLL)
    add_library(slime_game_module MODULE
        SlimeGameModule.cpp
        SlimeMetaballPass.cpp
        SlimeTuningComponent.cpp
        SlimeGameModuleDllExport.cpp)
    set_target_properties(slime_game_module PROPERTIES OUTPUT_NAME "slime")
    target_link_libraries(slime_game_module PRIVATE
        OrangeEngine::orange_engine
        OrangeRender::orange_render)
    if (MSVC)  # 中文注释源、无 BOM → /utf-8
        target_compile_options(slime_game_module PRIVATE /utf-8)
    endif ()

    # SDF pass shaders 拷到 slime.dll 旁（宿主编辑器按 exe-相对加载，standalone 验证也备）。
    add_custom_command(TARGET slime_game_module POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:slime_game_module>/shaders"
        COMMAND "${SPIKE01_GLSLANG}" -V
                "${CMAKE_CURRENT_SOURCE_DIR}/shaders/fullscreen.vert"
                -o "$<TARGET_FILE_DIR:slime_game_module>/shaders/fullscreen.vert.spv"
        COMMAND "${SPIKE01_GLSLANG}" -V
                "${CMAKE_CURRENT_SOURCE_DIR}/shaders/slime_metaball.frag"
                -o "$<TARGET_FILE_DIR:slime_game_module>/shaders/slime_metaball.frag.spv"
        VERBATIM
        COMMENT "Compiling slime_metaball shaders for slime.dll")
endif ()

# ===========================================================================
# SlimeEditor.exe —— 史莱姆 per-game editor（PIE M4，ADR-021）。
#
# orange_editor SDK（瘦编辑器宿主）+ SlimeGameModule 注入：编辑器里 Play →
# 史莱姆可操控、SDF metaball 在视口渲染、Stop 还原、（后续）Inspector 拖手感。
# 与 spike-01-blob（standalone runtime）共用 SlimeGameModule.cpp +
# SlimeMetaballPass.cpp + shaders，差别仅宿主。
#
# 消费链：find_package(OrangeEditor) 从已 install 的 SDK 拿 orange_editor lib +
# 公共头（EditorApp.h / EditorAppConfig.h）+ shader/theme copy-helper。
# OrangeEditor 的 Config 会 find_dependency(OrangeEngine)，级联拉起引擎 +
# imgui + OrangeRender + copy-helper，故无需显式再 find_package(OrangeEngine)。
# ===========================================================================
find_package(OrangeEditor 1.3 CONFIG REQUIRED)
add_executable(SlimeEditor
    SlimeEditorMain.cpp
    SlimeGameModule.cpp
    SlimeMetaballPass.cpp
    SlimeTuningComponent.cpp
    SlimeTuningSchema.cpp)
target_link_libraries(SlimeEditor PRIVATE
    OrangeEditor::orange_editor
    OrangeEngine::orange_engine
    OrangeRender::orange_render)
if (MSVC)  # 中文注释源、无 BOM → /utf-8
    target_compile_options(SlimeEditor PRIVATE /utf-8)
endif ()

# 运行期资产拷到 exe 旁（编辑器 + pass 全走 exe-相对加载）：
#   * 引擎 builtin shaders（Pipeline）—— OrangeEngine SDK copy-helper
#     （install GAP-2026-05-27 已修，取代 spike 的 ORANGE_ENGINE_SHADER_DIR workaround）
#   * editor grid shaders + theme（codicon 图标字体）—— OrangeEditor SDK copy-helper
orange_engine_copy_builtin_shaders(SlimeEditor)
orange_editor_copy_editor_shaders(SlimeEditor)
orange_editor_copy_theme(SlimeEditor)
# C# 脚本运行时（nethost + SDK/runtimeconfig）—— dotnet-enabled SDK 下 orange_editor 内置
# ScriptGameModule PUBLIC 链 nethost，SlimeEditor 启动即需 nethost.dll（M10 级别 1 redist）。
# 纯 C++ SDK 下本 helper 是 no-op。取代此前手动部署 dotnet 运行时的缺口。
orange_editor_copy_dotnet_runtime(SlimeEditor)

# M6：slime.orangeproject 拷到 exe 旁——SlimeEditorMain 用 GetModuleFileNameW 定位
# 后经 ApplyProjectFileToConfig 解析（项目名 → windowTitle、projectRoot → chdir）。
add_custom_command(TARGET SlimeEditor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/slime.orangeproject"
        "$<TARGET_FILE_DIR:SlimeEditor>/slime.orangeproject"
    COMMENT "Copying slime.orangeproject next to SlimeEditor.exe")

# 史莱姆 SDF pass shaders（fullscreen.vert + slime_metaball.frag → exe 旁 shaders/）——
# 与 spike-01-blob 同款 glslangValidator 编译（SlimeMetaballPass 按 exe-相对
# shaders/*.spv 加载）。注意落点 shaders/（非 shaders/orange_editor/），与编辑器
# grid shader 不冲突。
add_custom_command(TARGET SlimeEditor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:SlimeEditor>/shaders"
    COMMAND "${SPIKE01_GLSLANG}" -V
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/fullscreen.vert"
            -o "$<TARGET_FILE_DIR:SlimeEditor>/shaders/fullscreen.vert.spv"
    COMMAND "${SPIKE01_GLSLANG}" -V
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/slime_metaball.frag"
            -o "$<TARGET_FILE_DIR:SlimeEditor>/shaders/slime_metaball.frag.spv"
    VERBATIM
    COMMENT "Compiling slime_metaball shaders for SlimeEditor")

# ---- 软体 Blob 碰撞回归测试 ----
# Blob.h header-only 只依赖 glm，故只连 glm::glm 不拉引擎（纯 headless 数值验）。
find_package(glm CONFIG QUIET)
if (glm_FOUND)
    add_executable(spike-01-blob-tests tests/blob_collision_test.cpp)
    target_link_libraries(spike-01-blob-tests PRIVATE glm::glm)
    if (MSVC)  # 中文注释源、无 BOM → MSVC 须 /utf-8 避免按码页 936 误读
        target_compile_options(spike-01-blob-tests PRIVATE /utf-8)
    endif ()
    add_test(NAME spike-01-blob.blob_collision COMMAND spike-01-blob-tests)
else ()
    message(STATUS "glm CONFIG 未找到 → 跳过 spike-01-blob 软体碰撞测试")
endif ()
