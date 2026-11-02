cmake_minimum_required(VERSION 3.25)
project(SwitchGamesRecomp C CXX)
if(WIN32)
    enable_language(RC)  # assets/acnhrecomp.rc: the executables' icon
endif()

set(CMAKE_CXX_STANDARD 20)
if(ANDROID)
    # Thin archives: the generated modules' libraries list their objects instead of copying them (main alone has
    # ~3 GB of objects for Android; a copy doubled the build's disk use).
    set(CMAKE_CXX_ARCHIVE_CREATE "<CMAKE_AR> qcT <TARGET> <LINK_FLAGS> <OBJECTS>")
    set(CMAKE_CXX_ARCHIVE_APPEND "<CMAKE_AR> qT <TARGET> <LINK_FLAGS> <OBJECTS>")
    # The NDK's toolchain adds -g: debug info made main's objects 3.9 GB and lib<game>.so 2.4 GB (224 MB stripped).
    # The runtime symbolizes guest code itself (nx::describe), so none is needed.
    add_compile_options(-g0)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Build speed, same output: ccache when installed (a rebuild in a new build folder, or after an update that leaves
# most generated files unchanged, reuses the objects; the precompiled headers need its sloppiness settings), and
# lld on Linux when installed (the system's ld.bfd takes minutes and GBs of RAM on the game's objects).
find_program(NX_CCACHE ccache)
if(NX_CCACHE AND NOT CMAKE_CXX_COMPILER_LAUNCHER AND NOT MSVC)
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CMAKE_COMMAND};-E;env;CCACHE_SLOPPINESS=pch_defines,time_macros,include_file_mtime,include_file_ctime;${NX_CCACHE}")
    set(CMAKE_C_COMPILER_LAUNCHER "${NX_CCACHE}")
endif()
if(UNIX AND NOT APPLE AND NOT ANDROID)
    find_program(NX_LLD NAMES ld.lld lld)
    if(NX_LLD)
        add_link_options(-fuse-ld=lld)
    endif()
endif()

# The game this build is for: games/<NX_GAME>/ holds what only that game needs (game.cpp: nx/game.h), the shared
# runtime is the rest. One build directory per game.
set(NX_GAME "hades" CACHE STRING "Game to build (a folder under games/)")
set(NX_GAME_DIR "${CMAKE_SOURCE_DIR}/games/${NX_GAME}")
if(NOT EXISTS "${NX_GAME_DIR}/game.cpp")
    message(FATAL_ERROR "games/${NX_GAME}/game.cpp not found")
endif()

# Generated code of the game (python -m recomp <nso> <gen dir>/<module> --name <module> --base ...). Never committed.
# Hades keeps the layout from before there were other games (private/gen); the others use private/<game>/gen.
if(NX_GAME STREQUAL "hades")
    set(NX_GEN_DEFAULT "${CMAKE_SOURCE_DIR}/private/gen")
else()
    set(NX_GEN_DEFAULT "${CMAKE_SOURCE_DIR}/private/${NX_GAME}/gen")
endif()
set(NX_GEN_DIR "${NX_GEN_DEFAULT}" CACHE PATH "Recompiled module sources")

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options(-Wno-unused-label -Wno-unused-value -Wno-parentheses-equality -Wno-unused-variable
                        -Wno-unused-but-set-variable -Wno-constant-logical-operand -Wno-unused-function)
endif()
if(MSVC)
    add_compile_options(/arch:AVX2)
endif()
if(WIN32)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# Unit tests of runtime pieces that build without the game (docs/nvn/WINDOWS-PLAN.md, "Test plan"): ctest.
option(NX_TESTS "Build the standalone unit tests" OFF)
if(NX_TESTS AND NOT ANDROID)
    enable_testing()
    add_executable(gpu_pick_test tests/gpu_pick_test.cpp runtime/src/nvn/nvn_gpu_pick.cpp)
    target_compile_definitions(gpu_pick_test PRIVATE NX_GPU_PICK_STANDALONE)
    target_include_directories(gpu_pick_test PRIVATE runtime/src/nvn)
    add_test(NAME gpu_pick COMMAND gpu_pick_test)
    add_executable(tiling_test tests/tiling_test.cpp runtime/src/nvn/nvn_tiling.cpp)
    target_include_directories(tiling_test PRIVATE runtime/src/nvn)
    add_test(NAME tiling COMMAND tiling_test --selftest)
    add_executable(astc_test tests/astc_test.cpp runtime/src/nvn/nvn_astc.cpp)
    target_include_directories(astc_test PRIVATE runtime/src/nvn)
    add_test(NAME astc COMMAND astc_test --selftest)
    add_executable(shader_cache_test tests/shader_cache_test.cpp runtime/src/nvn/nvn_shaders.cpp
                   runtime/src/nvn/nvn_spirv_reflect.cpp)
    target_compile_definitions(shader_cache_test PRIVATE NX_SHADERS_STANDALONE)
    target_include_directories(shader_cache_test PRIVATE runtime/src/nvn)
    if(EXISTS "${CMAKE_SOURCE_DIR}/private/wonder/spirv")  # the game's own shaders: never in the repository
        add_test(NAME shader_cache COMMAND shader_cache_test "${CMAKE_SOURCE_DIR}/private/wonder")
    endif()
    if(WIN32)
        add_executable(memguard_test tests/memguard_test.cpp)
        target_link_libraries(memguard_test PRIVATE psapi)
        add_test(NAME memguard COMMAND memguard_test)
    endif()
endif()

# Native Metal backend (docs/NATIVE_METAL.md), Apple only: SPIRV-Cross (Apache-2.0) turns the shaders into MSL.
# Fetched at a fixed revision on the first configure. metal_shader_check runs every shader of a game through it
# (cmake --build <dir> --target metal_shader_check; metal_shader_check private/acnh).
if(APPLE)
    include(FetchContent)
    set(SPIRV_CROSS_CLI OFF CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_ENABLE_C_API OFF CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_ENABLE_HLSL OFF CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_ENABLE_CPP OFF CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_ENABLE_REFLECT OFF CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_ENABLE_UTIL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(spirv_cross GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Cross.git
                         GIT_TAG vulkan-sdk-1.4.309.0 GIT_SHALLOW TRUE EXCLUDE_FROM_ALL)
    FetchContent_MakeAvailable(spirv_cross)
    add_library(nx_mtl STATIC runtime/src/mtl/mtl_shaders.cpp)
    target_include_directories(nx_mtl PUBLIC runtime/src)
    target_link_libraries(nx_mtl PUBLIC spirv-cross-msl)
    enable_language(OBJCXX)
    if(EXISTS "${CMAKE_SOURCE_DIR}/tests/metal_shader_check.mm")  # not in the release packages
        add_executable(metal_shader_check EXCLUDE_FROM_ALL tests/metal_shader_check.mm)
        target_link_libraries(metal_shader_check PRIVATE nx_mtl "-framework Metal" "-framework Foundation")
        set_target_properties(metal_shader_check PROPERTIES OBJCXX_STANDARD 20)
    endif()
endif()

# Dear ImGui: the settings screen and the launcher. tools/setup_acnh.py downloads it into private/third_party/imgui.
set(NX_IMGUI_DIR "${CMAKE_SOURCE_DIR}/private/third_party/imgui" CACHE PATH "Dear ImGui sources")

# The launcher (launcher/): the program a player opens first. It has no game code, so it builds from a plain
# checkout (cmake --build <dir> --target acnhrecomp_launcher) and leaves the game's targets as they are.
option(NX_LAUNCHER "Build the first-run launcher (acnhrecomp_launcher)" ON)
if(NX_LAUNCHER AND NOT ANDROID)
    include(launcher/launcher.cmake)
endif()

# Desktop: the game's code from the game folder (game/<platform>/lib<game>_game, which ACNHRecomp-Importer builds)
# instead of linked into the executable: the player then only needs the folder.
option(NX_GAME_FROM_FOLDER "Desktop: load the game's code from the game folder" OFF)

# Without the generated code (a checkout with no game data) only the tests and the launcher above are built.
# Android builds the runtime without it too: the app (lib<game>.so) holds no game code and loads lib<game>_game.so,
# made from the player's own copy on their computer, from the game folder picked on the phone (docs/ANDROID.md). So
# does the desktop player (NX_GAME_FROM_FOLDER): the release's, which loads the game folder's library.
file(GLOB NX_GEN_PROBE "${NX_GEN_DIR}/main/*.cpp")
if(NOT NX_GEN_PROBE AND NOT ANDROID AND NOT NX_GAME_FROM_FOLDER)
    message(WARNING "No generated code in ${NX_GEN_DIR}/main: building the unit tests only")
    return()
endif()

# Extra modules some games recompile too (ACNH: subsdk0 = nn::movie). Present when their generated code is.
set(NX_MODULES)
if(NX_GEN_PROBE)
    set(NX_MODULES main sdk)
endif()
if(EXISTS "${NX_GEN_DIR}/subsdk0/table.cpp")
    list(APPEND NX_MODULES subsdk0)
endif()
set(NX_GAME_LIBS)
foreach(mod ${NX_MODULES})
    list(APPEND NX_GAME_LIBS game_${mod})
    file(GLOB src CONFIGURE_DEPENDS "${NX_GEN_DIR}/${mod}/*.cpp")
    add_library(game_${mod} STATIC ${src})
    target_include_directories(game_${mod} PRIVATE runtime/include "${NX_GEN_DIR}/${mod}")
    # Every generated file starts with #include "funcs.h" (the declarations of the whole module + nx/cpu.h): parsed
    # once into a precompiled header instead of once per file (~1000 files). The code it compiles is the same.
    if(EXISTS "${NX_GEN_DIR}/${mod}/funcs.h")
        target_precompile_headers(game_${mod} PRIVATE "${NX_GEN_DIR}/${mod}/funcs.h")
        # table.cpp is built with other visibility options on Android (below): a header of its own
        set_source_files_properties("${NX_GEN_DIR}/${mod}/table.cpp" TARGET_DIRECTORY game_${mod}
                                    PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    endif()
    if(ANDROID)
        # Hidden visibility: calls between recompiled functions are direct, not through the PLT.
        # armv8.2-a+crypto: cpu.h's AES/SHA paths need __ARM_FEATURE_AES/SHA2 (the NDK default armv8-a has neither).
        target_compile_options(game_${mod} PRIVATE -O2 -fPIC -fvisibility=hidden -fno-semantic-interposition
                               -march=armv8.2-a+crypto)
        # The module's table (nx_module_<name>) is what the app looks up in lib<game>_game.so.
        set_source_files_properties("${NX_GEN_DIR}/${mod}/table.cpp" TARGET_DIRECTORY game_${mod}
                                    PROPERTIES COMPILE_OPTIONS -fvisibility=default)
    elseif(APPLE)
        target_compile_options(game_${mod} PRIVATE -O2 -fvisibility=hidden)
        if(NX_GAME_FROM_FOLDER)  # the module's table is what the player looks up in lib<game>_game.dylib
            set_source_files_properties("${NX_GEN_DIR}/${mod}/table.cpp" TARGET_DIRECTORY game_${mod}
                                        PROPERTIES COMPILE_OPTIONS -fvisibility=default)
        endif()
    elseif(UNIX)
        # Linux x86-64: AVX2/FMA/BMI as the Windows build (/arch:AVX2); no FMA contraction, so a guest fmul + fadd
        # rounds twice, as on the Switch.
        target_compile_options(game_${mod} PRIVATE -O2 -fvisibility=hidden -march=x86-64-v3 -ffp-contract=off)
        if(NX_GAME_FROM_FOLDER)  # the module's table is what the player looks up in lib<game>_game.so
            set_source_files_properties("${NX_GEN_DIR}/${mod}/table.cpp" TARGET_DIRECTORY game_${mod}
                                        PROPERTIES COMPILE_OPTIONS -fvisibility=default)
        endif()
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        # Windows, clang-cl: /arch:AVX2 comes from above; no FMA contraction either (clang fuses within a statement
        # by default).
        target_compile_options(game_${mod} PRIVATE /O2 /clang:-ffp-contract=off)
    endif()
endforeach()

set(NX_RUNTIME_SOURCES
    runtime/src/game_folder.cpp
    runtime/src/main.cpp
    runtime/src/sampler.cpp
    runtime/src/loader.cpp
    runtime/src/bug_report.cpp
    runtime/src/memory.cpp
    runtime/src/kernel.cpp
    runtime/src/kernel_watch.cpp
    runtime/src/ipc.cpp
    runtime/src/svc_am.cpp
    runtime/src/svc_set.cpp
    runtime/src/svc_misc.cpp
    runtime/src/svc_time.cpp
    runtime/src/svc_process.cpp
    runtime/src/hle_fs.cpp
    runtime/src/svc_account.cpp
    runtime/src/users.cpp
    runtime/src/hle_ro.cpp
    runtime/src/hle_vi.cpp
    runtime/src/hle_hid.cpp
    runtime/src/hle_net.cpp
    runtime/src/hle_ldn.cpp
    runtime/src/hle_vsock.cpp
    runtime/src/ldn_link.cpp
    runtime/src/ldn_discovery.cpp
    runtime/src/ldn_upnp.cpp
    runtime/src/hle_audio.cpp
    runtime/src/hle_pl.cpp
    runtime/src/hle_nfp.cpp
    runtime/src/hle_time.cpp
    runtime/src/hle_bcat.cpp
    runtime/src/hle_movie.cpp
    runtime/src/hle_friends.cpp
    runtime/src/hle_ngc.cpp
    runtime/src/config.cpp
    runtime/src/mod_fetch.cpp
    runtime/src/discord_presence.cpp
    runtime/src/glyphs.cpp
    runtime/src/input_script.cpp
    runtime/src/touch_layout.cpp
    runtime/src/perf_hint.cpp
    runtime/src/thermal.cpp
    runtime/src/frame_stats.cpp
    runtime/src/write_watch.cpp
    runtime/src/nvn/nvn_caps.cpp
    runtime/src/nvn/nvn_renderpass.cpp
    runtime/src/nvn/nvn_gpu_busy.cpp
    runtime/src/nvn/nvn_janitor.cpp
    runtime/src/nvn/nvn_trace.cpp
    runtime/src/nvn/nvn_device.cpp
    runtime/src/nvn/nvn_objects.cpp
    runtime/src/nvn/nvn_incarnation.cpp
    runtime/src/nvn/nvn_texture_layout.cpp
    runtime/src/nvn/nvn_program.cpp
    runtime/src/nvn/nvn_queue.cpp
    runtime/src/nvn/nvn_cmd.cpp
    runtime/src/nvn/nvn_state.cpp
    runtime/src/nvn/nvn_format.cpp
    runtime/src/nvn/nvn_enums.cpp
    runtime/src/nvn/nvn_sampler.cpp
    runtime/src/nvn/nvn_sampler_objects.cpp
    runtime/src/nvn/nvn_draw_state.cpp
    runtime/src/nvn/nvn_raster.cpp
    runtime/src/nvn/nvn_worker.cpp
    runtime/src/nvn/nvn_shaders.cpp
    runtime/src/nvn/nvn_spirv_reflect.cpp
    runtime/src/nvn/nvn_tiling.cpp
    runtime/src/nvn/nvn_vk.cpp
    runtime/src/nvn/nvn_gpu_pick.cpp
    runtime/src/nvn/nvn_image.cpp
    runtime/src/nvn/nvn_frames.cpp
    runtime/src/nvn/nvn_dedup.cpp
    runtime/src/nvn/nvn_written.cpp
    runtime/src/nvn/nvn_zero_copy.cpp
    runtime/src/nvn/nvn_replay.cpp
    runtime/src/nvn/nvn_debug.cpp
    runtime/src/nvn/nvn_binds.cpp
    runtime/src/nvn/nvn_copy.cpp
    runtime/src/nvn/nvn_pipeline.cpp
    runtime/src/nvn/nvn_dummy.cpp
    runtime/src/nvn/nvn_pipeline_cache.cpp
    runtime/src/nvn/nvn_upload.cpp
    runtime/src/nvn/nvn_astc.cpp
    runtime/src/nvn/nvn_spirv_patch.cpp
    runtime/src/nvn/nvn_spirv_clip.cpp
    runtime/src/nvn/nvn_spirv_nan.cpp
    runtime/src/nvn/nvn_spirv_fragcoord.cpp
    runtime/src/nvn/nvn_spirv_instance.cpp
    runtime/src/nvn/nvn_spirv_alpha.cpp
    runtime/src/nvn/nvn_vk_defer.cpp
    runtime/src/nvn/nvn_ultrawide.cpp
    runtime/src/nvn/nvn_present.cpp
    runtime/src/nvn/nvn_framegen.cpp
    runtime/src/nvn/nvn_pipeline_cache.cpp
    runtime/src/nvn/nvn_bc.cpp
    runtime/src/nvn/nvn_draw.cpp
    runtime/src/nvn/nvn_sets.cpp
    runtime/src/nvn/nvn_clears.cpp
    runtime/src/nvn/nvn_mask_clear.cpp
    runtime/src/nvn/nvn_draw_trace.cpp
    runtime/src/nvn/nvn_parallel.cpp
    runtime/src/vk_bridge.cpp
    runtime/src/vk_swapchain.cpp
    runtime/src/vk_gen.cpp
    runtime/src/vk_present.cpp
)
file(GLOB NX_GAME_SOURCES CONFIGURE_DEPENDS "${NX_GAME_DIR}/*.cpp")
list(APPEND NX_RUNTIME_SOURCES ${NX_GAME_SOURCES})

# Vulkan headers (the same registry version tools/gen_vk.py generated from). The loader comes with the GPU driver.
if(DEFINED ENV{VULKAN_SDK})
    set(NX_VULKAN_SDK "$ENV{VULKAN_SDK}" CACHE PATH "Vulkan SDK")
else()
    set(NX_VULKAN_SDK "C:/VulkanSDK/1.4.357.0" CACHE PATH "Vulkan SDK")
endif()

# What the player and lib<game>_game must agree on (the CPU context, the module tables, the runtime's calls): a game
# library built from other headers is refused at start, asking for the game folder to be prepared again.
set(NX_ABI_TEXT "")
foreach(h cpu.h module.h runtime.h)
    file(READ "${CMAKE_SOURCE_DIR}/runtime/include/nx/${h}" t)
    string(APPEND NX_ABI_TEXT "${t}")
endforeach()
string(SHA1 NX_GAME_ABI "${NX_ABI_TEXT}")
string(SUBSTRING "${NX_GAME_ABI}" 0 12 NX_GAME_ABI)

if(ANDROID)
    # NativeActivity app: lib<game>.so, entry android_main (android_native_app_glue).
    set(GLUE "${ANDROID_NDK}/sources/android/native_app_glue")
    add_library(${NX_GAME} SHARED ${NX_RUNTIME_SOURCES} runtime/src/platform_android.cpp
        runtime/src/platform_android_input.cpp runtime/src/platform_android_audio.cpp runtime/src/memguard_android.cpp
        "${GLUE}/android_native_app_glue.c")
    target_include_directories(${NX_GAME} PRIVATE runtime/include runtime/src "${NX_GAME_DIR}" "${NX_VULKAN_SDK}/include" "${GLUE}")
    # Our ANativeActivity_onCreate (platform_android.cpp) runs first, on the UI thread, then calls the glue's.
    set_source_files_properties("${GLUE}/android_native_app_glue.c" PROPERTIES
        COMPILE_DEFINITIONS "ANativeActivity_onCreate=glue_ANativeActivity_onCreate")
    target_link_libraries(${NX_GAME} PRIVATE android log aaudio)
    # NX_ANDROID_GAME_IN_APP=ON: the game's code linked into lib<game>.so as before (comparisons; never published).
    option(NX_ANDROID_GAME_IN_APP "Link the game's code into the app's library" OFF)
    if(NX_ANDROID_GAME_IN_APP)
        target_link_libraries(${NX_GAME} PRIVATE ${NX_GAME_LIBS})
        target_compile_definitions(${NX_GAME} PRIVATE NX_GAME_ABI="${NX_GAME_ABI}")
    else()
        target_compile_definitions(${NX_GAME} PRIVATE NX_GAME_DYNAMIC NX_GAME_ABI="${NX_GAME_ABI}")
    endif()
    # libadrenotools (BSD-2-Clause): loads Turnip (tools/build_turnip.sh) instead of the phone's Adreno driver. Its
    # hook libraries (libhook_impl.so, libmain_hook.so, ...) go into the APK next to lib<game>.so.
    include(FetchContent)
    FetchContent_Declare(adrenotools GIT_REPOSITORY https://github.com/bylaws/libadrenotools.git
                         GIT_TAG 8fae8ce254dfc1344527e05301e43f37dea2df80 GIT_SUBMODULES_RECURSE TRUE)
    FetchContent_MakeAvailable(adrenotools)
    target_link_libraries(${NX_GAME} PRIVATE adrenotools)
    add_dependencies(${NX_GAME} hook_impl main_hook file_redirect_hook gsl_alloc_hook)
    target_compile_options(${NX_GAME} PRIVATE -fvisibility=hidden -fno-semantic-interposition -march=armv8.2-a+crypto)
    # The runtime (not the game's code) with ThinLTO and, when a profile is given, profile-guided optimization: the
    # replay is spread over many functions and misses the instruction cache (~9 refills per 1000 instructions on the
    # S20 FE), what code layout by use helps with. NX_PGO=generate: an instrumented build that writes
    # <data folder>/acnh.profraw after NX_PGO_SECONDS of play (platform_android.cpp); NX_PGO=<file.profdata>: used.
    set(NX_PGO "" CACHE STRING "Android runtime PGO: generate, or a .profdata file")
    target_compile_options(${NX_GAME} PRIVATE -flto=thin)
    target_link_options(${NX_GAME} PRIVATE -flto=thin -Wl,--lto-O3)
    if(NX_PGO STREQUAL "generate")
        target_compile_options(${NX_GAME} PRIVATE -fprofile-instr-generate)
        target_link_options(${NX_GAME} PRIVATE -fprofile-instr-generate)
        target_compile_definitions(${NX_GAME} PRIVATE NX_PGO_GENERATE)
    elseif(NX_PGO)
        target_compile_options(${NX_GAME} PRIVATE "-fprofile-instr-use=${NX_PGO}" -Wno-profile-instr-out-of-date
                               -Wno-profile-instr-unprofiled -Wno-backend-plugin)
        target_link_options(${NX_GAME} PRIVATE "-fprofile-instr-use=${NX_PGO}")
    endif()
    # The glue's ANativeActivity_onCreate must be exported. 16 KB page alignment: Android 15+ devices with 16 KB pages.
    target_link_options(${NX_GAME} PRIVATE -u ANativeActivity_onCreate -Wl,-z,max-page-size=16384)
    # The game's code: lib<game>_game.so, linked against the app's library for the few functions it calls (nx::call,
    # svc, mrs, ...: NX_GAME_API). Never in the APK; tools/build_android.sh leaves it beside, for the game folder.
    if(NX_GAME_LIBS AND NOT NX_ANDROID_GAME_IN_APP)
        add_library(${NX_GAME}_game SHARED runtime/src/game_abi.cpp)
        target_include_directories(${NX_GAME}_game PRIVATE runtime/include)
        target_compile_options(${NX_GAME}_game PRIVATE -fvisibility=hidden -march=armv8.2-a+crypto)
        target_compile_definitions(${NX_GAME}_game PRIVATE NX_GAME_ABI="${NX_GAME_ABI}")
        target_link_libraries(${NX_GAME}_game PRIVATE "$<LINK_LIBRARY:WHOLE_ARCHIVE,${NX_GAME_LIBS}>" ${NX_GAME})
        target_link_options(${NX_GAME}_game PRIVATE -Wl,-z,max-page-size=16384)
    endif()
elseif(APPLE)
    # macOS: SDL3 (window, input, audio) and the Vulkan loader + MoltenVK from Homebrew
    # (brew install sdl3 vulkan-headers vulkan-loader molten-vk).
    find_package(SDL3 REQUIRED CONFIG)
    find_path(NX_VULKAN_INCLUDE vulkan/vulkan.h HINTS /opt/homebrew/include /usr/local/include)
    add_executable(${NX_GAME} ${NX_RUNTIME_SOURCES} runtime/src/platform_mac.cpp runtime/src/memguard_mac.cpp runtime/src/cpu_profile.cpp
                   runtime/src/app_icon.cpp)
    target_include_directories(${NX_GAME} PRIVATE runtime/include runtime/src "${NX_GAME_DIR}" "${NX_VULKAN_INCLUDE}")
    target_link_libraries(${NX_GAME} PRIVATE SDL3::SDL3)
    if(NX_GAME_FROM_FOLDER)
        # The runtime's calls (NX_GAME_API) exported to game/macos-arm64/lib<game>_game.dylib, which resolves them at
        # load time (-undefined dynamic_lookup).
        target_compile_definitions(${NX_GAME} PRIVATE NX_GAME_DYNAMIC NX_GAME_ABI="${NX_GAME_ABI}")
        set_target_properties(${NX_GAME} PROPERTIES ENABLE_EXPORTS ON)
        if(NX_GAME_LIBS)
            add_library(${NX_GAME}_game SHARED runtime/src/game_abi.cpp)
            target_include_directories(${NX_GAME}_game PRIVATE runtime/include)
            target_compile_options(${NX_GAME}_game PRIVATE -fvisibility=hidden)
            target_compile_definitions(${NX_GAME}_game PRIVATE NX_GAME_ABI="${NX_GAME_ABI}")
            target_link_libraries(${NX_GAME}_game PRIVATE "$<LINK_LIBRARY:WHOLE_ARCHIVE,${NX_GAME_LIBS}>")
            target_link_options(${NX_GAME}_game PRIVATE -undefined dynamic_lookup)
        endif()
    else()
        target_link_libraries(${NX_GAME} PRIVATE ${NX_GAME_LIBS})
    endif()
    target_compile_options(${NX_GAME} PRIVATE -fvisibility=hidden)
    # The executable loads near 0x1_0000_0000, below the guest ranges (0x40_0000_0000 and up). The game runs on its
    # own thread with a 64 MiB stack (platform_mac.cpp), so the main thread's stack size does not matter.
elseif(UNIX)
    # Linux: SDL3 (window, input, audio) and the system Vulkan loader, as on macOS. SDL3 from the distribution or a
    # local build (-DCMAKE_PREFIX_PATH=<its install prefix>).
    find_package(SDL3 REQUIRED CONFIG)
    find_package(Threads REQUIRED)
    find_path(NX_VULKAN_INCLUDE vulkan/vulkan.h HINTS "${NX_VULKAN_SDK}/include" /usr/include /usr/local/include)
    add_executable(${NX_GAME} ${NX_RUNTIME_SOURCES} runtime/src/platform_linux.cpp runtime/src/memguard_linux.cpp runtime/src/cpu_profile.cpp
                   runtime/src/app_icon.cpp)
    target_include_directories(${NX_GAME} PRIVATE runtime/include runtime/src "${NX_GAME_DIR}" "${NX_VULKAN_INCLUDE}")
    target_link_libraries(${NX_GAME} PRIVATE SDL3::SDL3 Threads::Threads ${CMAKE_DL_LIBS})
    target_compile_options(${NX_GAME} PRIVATE -fvisibility=hidden -march=x86-64-v3 -ffp-contract=off)
    if(NX_GAME_FROM_FOLDER)
        # The runtime's calls (NX_GAME_API) exported to the game's library, which the player loads from the folder.
        target_compile_definitions(${NX_GAME} PRIVATE NX_GAME_DYNAMIC NX_GAME_ABI="${NX_GAME_ABI}")
        set_target_properties(${NX_GAME} PROPERTIES ENABLE_EXPORTS ON)
        if(NX_GAME_LIBS)  # with generated code at hand: the game folder's library too (lib<game>_game.so)
            foreach(lib ${NX_GAME_LIBS})
                set_target_properties(${lib} PROPERTIES POSITION_INDEPENDENT_CODE ON)
            endforeach()
            add_library(${NX_GAME}_game SHARED runtime/src/game_abi.cpp)
            target_include_directories(${NX_GAME}_game PRIVATE runtime/include)
            target_compile_options(${NX_GAME}_game PRIVATE -fvisibility=hidden)
            target_compile_definitions(${NX_GAME}_game PRIVATE NX_GAME_ABI="${NX_GAME_ABI}")
            target_link_libraries(${NX_GAME}_game PRIVATE "$<LINK_LIBRARY:WHOLE_ARCHIVE,${NX_GAME_LIBS}>")
        endif()
    else()
        target_link_libraries(${NX_GAME} PRIVATE ${NX_GAME_LIBS})
    endif()
    # The executable (PIE) loads near 0x55_5555_0000_0000, far above the guest ranges (0x40_0000_0000 and up). The
    # game runs on its own thread with a 64 MiB stack (platform_linux.cpp).
else()
    # Windows: SDL3 (window, input, audio, pickers: platform_sdl.cpp) from SDL's VC development package
    # (-DCMAKE_PREFIX_PATH=<SDL3-x.y.z>, tools/setup_acnh.py fetches it), the OS part in platform_win.cpp, and the
    # Vulkan loader of the GPU driver. Vulkan headers: NX_VULKAN_INCLUDE (the setup's Vulkan-Headers) or the LunarG SDK.
    find_package(SDL3 REQUIRED CONFIG)
    find_path(NX_VULKAN_INCLUDE vulkan/vulkan.h HINTS "${NX_VULKAN_SDK}/Include")
    add_executable(${NX_GAME} ${NX_RUNTIME_SOURCES} runtime/src/platform_win.cpp runtime/src/platform_sdl.cpp
                   runtime/src/app_icon.cpp assets/acnhrecomp.rc assets/acnhrecomp.manifest
                   runtime/src/memguard_win.cpp)
    target_include_directories(${NX_GAME} PRIVATE runtime/include runtime/src "${NX_GAME_DIR}" "${NX_VULKAN_INCLUDE}")
    target_link_libraries(${NX_GAME} PRIVATE SDL3::SDL3 ole32 psapi winmm dxgi ws2_32)
    if(NX_GAME_FROM_FOLDER)
        # The game's code from game/windows-x64/<game>_game.dll, which the player hands its runtime calls to
        # (game_abi.cpp: nx_game_bind).
        target_compile_definitions(${NX_GAME} PRIVATE NX_GAME_DYNAMIC NX_GAME_ABI="${NX_GAME_ABI}")
    else()
        target_link_libraries(${NX_GAME} PRIVATE ${NX_GAME_LIBS})
    endif()
    # SDL3.dll next to the game, so it starts from a double click.
    add_custom_command(TARGET ${NX_GAME} POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
                       $<TARGET_FILE:SDL3::SDL3> $<TARGET_FILE_DIR:${NX_GAME}>)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        # No fused multiply-add the guest code did not ask for (its float results must match).
        target_compile_options(${NX_GAME} PRIVATE /clang:-ffp-contract=off)
        # __builtin_cpu_supports (nx/cpu.h, the AES-NI path) needs compiler-rt's builtins, which clang-cl does not
        # link by itself: lib/clang/<version>/lib/windows next to the compiler.
        get_filename_component(NX_LLVM_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
        file(GLOB NX_CLANG_RT "${NX_LLVM_DIR}/../lib/clang/*/lib/windows/clang_rt.builtins-x86_64.lib")
        if(NX_CLANG_RT)
            list(GET NX_CLANG_RT 0 NX_CLANG_RT)
            target_link_libraries(${NX_GAME} PRIVATE "${NX_CLANG_RT}")
        else()
            message(WARNING "clang_rt.builtins-x86_64.lib not found next to ${CMAKE_CXX_COMPILER}: the link will miss __cpu_model")
        endif()
    endif()
    # The runtime uses the portable C library (getenv, fopen, ...) on every host: no "_s" variants.
    target_compile_definitions(${NX_GAME} PRIVATE _CRT_SECURE_NO_WARNINGS _SILENCE_CXX20_U8PATH_DEPRECATION_WARNING)
    if(MSVC)
        # Keep host allocations low so the guest ranges (0x40_0000_0000..0x70_0000_0000) are always free; a big
        # main-thread stack (recompiled calls nest deeply); a link map for tools/symbolize.py.
        target_link_options(${NX_GAME} PRIVATE /STACK:67108864 /HIGHENTROPYVA:NO /MAP)
    endif()
endif()

# The settings screen (runtime/src/ui): Dear ImGui (NX_IMGUI_DIR, above). Without it the screen is a stub;
# config.ini still works.
if(EXISTS "${NX_IMGUI_DIR}/imgui.cpp")
    target_sources(${NX_GAME} PRIVATE runtime/src/ui/ui.cpp runtime/src/ui/settings.cpp runtime/src/ui/i18n.cpp
        runtime/src/ui/theme.cpp ${NX_IMGUI_DIR}/imgui.cpp ${NX_IMGUI_DIR}/imgui_draw.cpp ${NX_IMGUI_DIR}/imgui_tables.cpp
        ${NX_IMGUI_DIR}/imgui_widgets.cpp ${NX_IMGUI_DIR}/backends/imgui_impl_vulkan.cpp)
    target_include_directories(${NX_GAME} PRIVATE ${NX_IMGUI_DIR})
    target_compile_definitions(${NX_GAME} PRIVATE IMGUI_IMPL_VULKAN_NO_PROTOTYPES $<$<NOT:$<BOOL:${ANDROID}>>:NX_HAVE_SDL3>)
else()
    message(STATUS "Dear ImGui not found in ${NX_IMGUI_DIR}: no settings screen")
    target_sources(${NX_GAME} PRIVATE runtime/src/ui/ui_stub.cpp)
endif()
target_sources(${NX_GAME} PRIVATE runtime/src/saves.cpp)
find_package(ZLIB QUIET)
if(ZLIB_FOUND)
    target_link_libraries(${NX_GAME} PRIVATE ZLIB::ZLIB)
    target_compile_definitions(${NX_GAME} PRIVATE NX_HAVE_ZLIB)
endif()
execute_process(COMMAND git describe --always --dirty WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                OUTPUT_VARIABLE NX_GIT_DESCRIBE OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT NX_GIT_DESCRIBE)
    set(NX_GIT_DESCRIBE "dev")
endif()
target_compile_definitions(${NX_GAME} PRIVATE NX_VERSION_STRING="${NX_GIT_DESCRIBE}")

if("subsdk0" IN_LIST NX_MODULES AND NOT NX_GAME_FROM_FOLDER)
    target_compile_definitions(${NX_GAME} PRIVATE NX_HAS_SUBSDK0)
endif()

# libvpx and libvorbis decode VP8/VP9 + Vorbis movies (runtime/src/hle_movie.cpp; ACNH's opening movie). From the
# system: apt install libvpx-dev libvorbis-dev, brew install libvpx libvorbis. Without libvpx the movies play with
# black frames; without libvorbis the SDK's own Vorbis decoder is used.
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND AND NOT ANDROID)
    pkg_check_modules(VPX QUIET IMPORTED_TARGET vpx)
    pkg_check_modules(VORBIS QUIET IMPORTED_TARGET vorbis)
endif()
set(NX_VPX_TARGET PkgConfig::VPX)
set(NX_VORBIS_TARGET PkgConfig::VORBIS)
# Without pkg-config (a Mac with Homebrew's libvpx and libvorbis but no pkgconf) the libraries are looked up
# directly; before, such a build quietly had black movies.
if(NOT ANDROID AND NOT VPX_FOUND)
    find_path(NX_VPX_INCLUDE vpx/vpx_decoder.h
              HINTS $ENV{HOMEBREW_PREFIX}/include /opt/homebrew/include /opt/homebrew/opt/libvpx/include
                    /usr/local/include /usr/local/opt/libvpx/include
              PATHS /opt/homebrew/include /usr/local/include)
    find_library(NX_VPX_LIBRARY vpx
                 HINTS $ENV{HOMEBREW_PREFIX}/lib /opt/homebrew/lib /opt/homebrew/opt/libvpx/lib
                       /usr/local/lib /usr/local/opt/libvpx/lib
                 PATHS /opt/homebrew/lib /usr/local/lib)
    if(NX_VPX_INCLUDE AND NX_VPX_LIBRARY)
        if(NOT TARGET nx_vpx)
            add_library(nx_vpx INTERFACE IMPORTED)
            set_target_properties(nx_vpx PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${NX_VPX_INCLUDE}"
                                  INTERFACE_LINK_LIBRARIES "${NX_VPX_LIBRARY}")
        endif()
        set(VPX_FOUND TRUE)
        set(NX_VPX_TARGET nx_vpx)
        message(STATUS "libvpx: ${NX_VPX_LIBRARY} (without pkg-config)")
    endif()
endif()
if(NOT ANDROID AND NOT VORBIS_FOUND)
    find_path(NX_VORBIS_INCLUDE vorbis/codec.h
              HINTS $ENV{HOMEBREW_PREFIX}/include /opt/homebrew/include /opt/homebrew/opt/libvorbis/include
                    /usr/local/include /usr/local/opt/libvorbis/include
              PATHS /opt/homebrew/include /usr/local/include)
    find_library(NX_VORBIS_LIBRARY vorbis
                 HINTS $ENV{HOMEBREW_PREFIX}/lib /opt/homebrew/lib /opt/homebrew/opt/libvorbis/lib
                       /usr/local/lib /usr/local/opt/libvorbis/lib
                 PATHS /opt/homebrew/lib /usr/local/lib)
    find_library(NX_OGG_LIBRARY ogg
                 HINTS $ENV{HOMEBREW_PREFIX}/lib /opt/homebrew/lib /opt/homebrew/opt/libogg/lib
                       /usr/local/lib /usr/local/opt/libogg/lib
                 PATHS /opt/homebrew/lib /usr/local/lib)
    if(NX_VORBIS_INCLUDE AND NX_VORBIS_LIBRARY AND NX_OGG_LIBRARY)
        if(NOT TARGET nx_vorbis)
            add_library(nx_vorbis INTERFACE IMPORTED)
            set_target_properties(nx_vorbis PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${NX_VORBIS_INCLUDE}"
                                  INTERFACE_LINK_LIBRARIES "${NX_VORBIS_LIBRARY};${NX_OGG_LIBRARY}")
        endif()
        set(VORBIS_FOUND TRUE)
        set(NX_VORBIS_TARGET nx_vorbis)
        message(STATUS "libvorbis: ${NX_VORBIS_LIBRARY} (without pkg-config)")
    endif()
endif()
if(VORBIS_FOUND)
    target_link_libraries(${NX_GAME} PRIVATE ${NX_VORBIS_TARGET})
else()
    message(STATUS "libvorbis not found: movies use the SDK's Vorbis decoder")
    target_compile_definitions(${NX_GAME} PRIVATE NX_NO_VORBIS)
endif()
if(VPX_FOUND)
    target_link_libraries(${NX_GAME} PRIVATE ${NX_VPX_TARGET})
else()
    message(STATUS "libvpx not found: movies will show black frames")
    target_compile_definitions(${NX_GAME} PRIVATE NX_NO_VPX)
endif()
