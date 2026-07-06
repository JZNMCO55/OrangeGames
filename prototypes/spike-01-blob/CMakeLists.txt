# Spike 1 —— 流体史莱姆 context-sensitive 手感原型。
# 设计规格见 ../spike-01-blob-feel.md + ../phase0-pillars.md。
#
# 状态：**可编骨架**。当前 main.cpp 只验证"游戏仓能 find_package 引擎 +
# 开窗 + debug draw"这条消费链路；手感 Loop A / 软体 blob / 调参面板尚未
# 落地——调参面板待引擎补完 GAP-2026-05-27-consumer-imgui-tuning-hook
# （消费者 ImGui hook）后再接。
add_executable(spike-01-blob main.cpp SlimeMetaballPass.cpp)

# Tier 2 M0：自定义 SDF fragment pass 直接下沉到 RHI（IRenderPass 是引擎明确开放的
# 逃生舱口）。只链公共 SDK OrangeRender::orange_render，绝不碰 internals / backend 私有头。
find_package(OrangeRender CONFIG REQUIRED)
target_link_libraries(spike-01-blob PRIVATE
    OrangeEngine::orange_engine
    OrangeRender::orange_render)

if (MSVC)  # 中文注释源、无 BOM → MSVC 须 /utf-8 避免按码页 936 误读
    target_compile_options(spike-01-blob PRIVATE /utf-8)
endif ()

# ---- builtin shader 运行期依赖（临时 workaround）----
# 引擎 install 暂未装 builtin .spv（OrangeEngine
# GAP-2026-05-27-builtin-shaders-not-installed-for-consumers）：外部消费者
# find_package 后能编过，但运行期 Pipeline::Initialize 按 ".exe 相对
# shaders/orange_engine/*.spv" 加载会扑空。在引擎修好前，把引擎 build tree
# 里编出的 shaders 目录 copy 到本 .exe 旁。
#
# 用法：配置时传
#   -DORANGE_ENGINE_SHADER_DIR=<engine-build>/bin/<config>/shaders/orange_engine
# 留空则跳过 copy（届时游戏能编但运行期缺 shader）。
set(ORANGE_ENGINE_SHADER_DIR "" CACHE PATH
    "引擎 build tree 内编出的 shaders/orange_engine 目录；copy 到本 .exe 旁")
if (ORANGE_ENGINE_SHADER_DIR)
    add_custom_command(TARGET spike-01-blob POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${ORANGE_ENGINE_SHADER_DIR}"
            "$<TARGET_FILE_DIR:spike-01-blob>/shaders/orange_engine"
        COMMENT "Copying OrangeEngine builtin shaders next to spike-01-blob.exe")
endif ()

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
