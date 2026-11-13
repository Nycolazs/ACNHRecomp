# The first-run launcher (launcher/, README "Installation"), included by the top CMakeLists.txt. No game code: SDL3
# (window, file dialogs, processes) and Dear ImGui drawn with SDL_Renderer, so it needs neither the Vulkan SDK nor
# MoltenVK to show its pages. Dear ImGui comes from NX_IMGUI_DIR, or is downloaded here on a fresh checkout.
find_package(SDL3 QUIET CONFIG)
if(NOT SDL3_FOUND)
    message(STATUS "SDL3 not found: no launcher (install SDL3, or -DCMAKE_PREFIX_PATH=<its install prefix>)")
    return()
endif()
find_package(Threads REQUIRED)

set(NX_LAUNCHER_IMGUI "${NX_IMGUI_DIR}")
if(NOT EXISTS "${NX_LAUNCHER_IMGUI}/backends/imgui_impl_sdlrenderer3.cpp")
    include(FetchContent)
    FetchContent_Declare(nx_imgui URL https://github.com/ocornut/imgui/archive/refs/tags/v1.91.9.tar.gz)
    FetchContent_MakeAvailable(nx_imgui)  # no CMakeLists.txt in it: only downloaded
    set(NX_LAUNCHER_IMGUI "${nx_imgui_SOURCE_DIR}")
endif()

add_executable(acnhrecomp_launcher WIN32
    launcher/main.cpp launcher/pages.cpp launcher/files.cpp launcher/pipeline.cpp launcher/desktop.cpp
    launcher/cpu_check.cpp
    runtime/src/ldn_upnp.cpp
    runtime/src/nextendo.cpp
    runtime/src/nextendo_http.cpp
    launcher/save_log_stub.cpp
    runtime/src/ui/theme.cpp runtime/src/ui/i18n.cpp runtime/src/ui/ui_cjk_font.cpp runtime/src/app_icon.cpp
    $<$<PLATFORM_ID:Windows>:${CMAKE_SOURCE_DIR}/assets/acnhrecomp.rc>
    $<$<PLATFORM_ID:Windows>:${CMAKE_SOURCE_DIR}/assets/acnhrecomp.manifest>
    ${NX_LAUNCHER_IMGUI}/imgui.cpp ${NX_LAUNCHER_IMGUI}/imgui_draw.cpp ${NX_LAUNCHER_IMGUI}/imgui_tables.cpp
    ${NX_LAUNCHER_IMGUI}/imgui_widgets.cpp ${NX_LAUNCHER_IMGUI}/backends/imgui_impl_sdl3.cpp
    ${NX_LAUNCHER_IMGUI}/backends/imgui_impl_sdlrenderer3.cpp)
target_include_directories(acnhrecomp_launcher PRIVATE launcher runtime/src ${NX_LAUNCHER_IMGUI})
# No /arch:AVX2 (the top CMakeLists.txt adds it on Windows) in the launcher and the libraries it links: on a CPU
# without AVX2 it must start, and tell the player why the game cannot run (cpu_check.cpp), instead of crashing.
foreach(t acnhrecomp_launcher nx_save_storage nx_mod_manager)
    get_target_property(opts ${t} COMPILE_OPTIONS)
    if(opts)
        list(REMOVE_ITEM opts /arch:AVX2)
        set_target_properties(${t} PROPERTIES COMPILE_OPTIONS "${opts}")
    endif()
endforeach()
target_link_libraries(acnhrecomp_launcher PRIVATE SDL3::SDL3 Threads::Threads nx_save_storage nx_mod_manager)
if(WIN32)
    target_link_libraries(acnhrecomp_launcher PRIVATE ole32 shell32 uuid ws2_32 bcrypt)  # desktop + Nextendo PKCE
endif()
# A release build (.github/workflows/build.yml) names its version: git describe would give the previous tag when the
# packages are built before the release's tag exists, and 0.8.0 then called itself 0.7.0 (issue #76).
if("$ENV{NX_RELEASE_VERSION}" MATCHES "^v?[0-9]+\\.[0-9]+")
    set(NX_LAUNCHER_DESCRIBE "$ENV{NX_RELEASE_VERSION}")
else()
    execute_process(COMMAND git describe --always --dirty WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                    OUTPUT_VARIABLE NX_LAUNCHER_DESCRIBE OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
endif()
if(NOT NX_LAUNCHER_DESCRIBE)
    set(NX_LAUNCHER_DESCRIBE "dev")
endif()
# NX_LAUNCHER_SOURCE_DIR: where tools/setup_acnh.py is when the launcher runs from its build folder.
target_compile_definitions(acnhrecomp_launcher PRIVATE NX_HAVE_SDL3 NX_LAUNCHER_UI NX_VERSION_STRING="${NX_LAUNCHER_DESCRIBE}"
                           NX_LAUNCHER_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
if(WIN32)
    # SDL3.dll next to the launcher, so it starts from a double click (as the game, CMakeLists.txt).
    add_custom_command(TARGET acnhrecomp_launcher POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
                       $<TARGET_FILE:SDL3::SDL3> $<TARGET_FILE_DIR:acnhrecomp_launcher>)
endif()
if(MSVC)
    target_compile_options(acnhrecomp_launcher PRIVATE /utf-8)
    target_compile_definitions(acnhrecomp_launcher PRIVATE _CRT_SECURE_NO_WARNINGS
                               _SILENCE_CXX20_U8PATH_DEPRECATION_WARNING)
endif()
