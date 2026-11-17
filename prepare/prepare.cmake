# The rest of the game folder's preparation (prepare/README.md), included by the top CMakeLists.txt after
# recomp/recomp.cmake (nx_recomp) and with NX_GAME_ABI computed:
#   nx_gamelib    the game's library for each platform, compiled with zig from the recompiled code (gamelib/)
#   nx_prepare    the stages on a worker thread (prepare.h: prep::start / status / cancel)
#   acnh_prepare  the command line (prepare_cli.cpp), on desktop
#   nx_prepare_jni  Android: libacnh_prepare.so, the app's preparation (prepare_jni.cpp; android/java Prepare)
# nx_gamelib and nx_prepare are plain C++20 and the OS's process calls: they build for Android too.
if(NOT TARGET nx_prepare_io)
    include(${CMAKE_CURRENT_LIST_DIR}/io/io.cmake)
endif()
set(NX_PREPARE_DIR "${CMAKE_CURRENT_LIST_DIR}")
get_filename_component(NX_PREPARE_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)  # the repository

# What the game's library is compiled against, built into nx_gamelib (gamelib_data.inc): ACNHRecomp's own runtime
# headers and game_abi.cpp, the same files the player is built from, so the library's NX_GAME_ABI is the player's.
# In pieces of 8000 characters: MSVC takes no single string literal over 16380 bytes (adjacent ones are joined).
function(nx_embed out_var)  # nx_embed(<var> <name> <file> [<name> <file>]...)
    set(text "")
    set(args ${ARGN})
    list(LENGTH args n)
    set(i 0)
    while(i LESS n)
        list(GET args ${i} name)
        math(EXPR j "${i} + 1")
        list(GET args ${j} file)
        file(READ "${file}" t)
        string(APPEND text "{\"${name}\",\n")
        string(LENGTH "${t}" len)
        set(k 0)
        while(k LESS len)
            string(SUBSTRING "${t}" ${k} 8000 piece)
            string(APPEND text "R\"NXDATA(${piece})NXDATA\"\n")
            math(EXPR k "${k} + 8000")
        endwhile()
        string(APPEND text "},\n")
        set_property(DIRECTORY "${CMAKE_SOURCE_DIR}" APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${file}")
        math(EXPR i "${i} + 2")
    endwhile()
    set(${out_var} "${text}" PARENT_SCOPE)
endfunction()

nx_embed(NX_GL_DATA
    nx/cpu.h "${NX_PREPARE_ROOT}/runtime/include/nx/cpu.h"
    nx/module.h "${NX_PREPARE_ROOT}/runtime/include/nx/module.h"
    nx/runtime.h "${NX_PREPARE_ROOT}/runtime/include/nx/runtime.h"
    game_abi.cpp "${NX_PREPARE_ROOT}/runtime/src/game_abi.cpp"
    cpu_model.c "${NX_PREPARE_DIR}/gamelib/cpu_model.c")
set(NX_GL_INC "${CMAKE_BINARY_DIR}/prepare_gamelib")
file(WRITE "${NX_GL_INC}/gamelib_data.inc.tmp" "${NX_GL_DATA}")
configure_file("${NX_GL_INC}/gamelib_data.inc.tmp" "${NX_GL_INC}/gamelib_data.inc" COPYONLY)

# gamelib/hot/<build id>.txt: the functions each game spends its time in (gamelib.cpp: kHotLists; none so far).
file(GLOB NX_GL_HOT_FILES "${NX_PREPARE_DIR}/gamelib/hot/*.txt")
set(NX_GL_HOT_ARGS "")
foreach(f ${NX_GL_HOT_FILES})
    get_filename_component(id "${f}" NAME_WE)
    list(APPEND NX_GL_HOT_ARGS "${id}" "${f}")
endforeach()
nx_embed(NX_GL_HOT ${NX_GL_HOT_ARGS})
file(WRITE "${NX_GL_INC}/gamelib_hot.inc.tmp" "${NX_GL_HOT}")
configure_file("${NX_GL_INC}/gamelib_hot.inc.tmp" "${NX_GL_INC}/gamelib_hot.inc" COPYONLY)
set_property(DIRECTORY "${CMAKE_SOURCE_DIR}" APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${NX_PREPARE_DIR}/gamelib/hot")

find_package(Threads REQUIRED)
add_library(nx_gamelib STATIC "${NX_PREPARE_DIR}/gamelib/gamelib.cpp")
target_include_directories(nx_gamelib PUBLIC "${NX_PREPARE_DIR}/gamelib" PRIVATE "${NX_GL_INC}")
target_compile_definitions(nx_gamelib PRIVATE GL_GAME_ABI="${NX_GAME_ABI}")
target_compile_features(nx_gamelib PUBLIC cxx_std_20)
target_link_libraries(nx_gamelib PUBLIC Threads::Threads)
set_target_properties(nx_gamelib PROPERTIES POSITION_INDEPENDENT_CODE ON)

# The hash of recomp/*.py the C++ recompiler matches (recomp/SOURCE): gen/recomp_source.txt.
file(STRINGS "${NX_PREPARE_DIR}/recomp/SOURCE" NX_RECOMP_SOURCE LIMIT_COUNT 1)
set_property(DIRECTORY "${CMAKE_SOURCE_DIR}" APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${NX_PREPARE_DIR}/recomp/SOURCE")
add_library(nx_prepare STATIC "${NX_PREPARE_DIR}/prepare.cpp")
target_include_directories(nx_prepare PUBLIC "${NX_PREPARE_DIR}")
target_compile_definitions(nx_prepare PRIVATE NX_RECOMP_SOURCE="${NX_RECOMP_SOURCE}")
target_link_libraries(nx_prepare PUBLIC nx_prepare_io nx_recomp nx_gamelib Threads::Threads)
set_target_properties(nx_prepare PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(ANDROID)
    # The phone's own preparation, apart from the player's libacnh.so (preparing loads no runtime and no game): JNI
    # calls for PrepareNative.java, the static libraries' symbols kept inside. 16 KB page alignment, as the app's other
    # libraries.
    add_library(nx_prepare_jni SHARED "${NX_PREPARE_DIR}/prepare_jni.cpp")
    set_target_properties(nx_prepare_jni PROPERTIES OUTPUT_NAME acnh_prepare CXX_VISIBILITY_PRESET hidden)
    target_link_libraries(nx_prepare_jni PRIVATE nx_prepare log)
    target_link_options(nx_prepare_jni PRIVATE -Wl,-z,max-page-size=16384 -Wl,--gc-sections -Wl,--exclude-libs,ALL)
    set(NX_PREPARE_TARGETS nx_gamelib nx_prepare nx_prepare_jni)
else()
    add_executable(acnh_prepare "${NX_PREPARE_DIR}/prepare_cli.cpp")
    target_link_libraries(acnh_prepare PRIVATE nx_prepare)
    if(WIN32)
        target_link_libraries(acnh_prepare PRIVATE shell32)  # CommandLineToArgvW (io/args_utf8.h)
    endif()
    set(NX_PREPARE_TARGETS nx_gamelib nx_prepare acnh_prepare)
    # The launcher prepares the game folder itself (launcher/prepare_job.cpp): the stages on their worker thread.
    if(TARGET acnhrecomp_launcher)
        target_link_libraries(acnhrecomp_launcher PRIVATE nx_prepare)
        target_compile_definitions(acnhrecomp_launcher PRIVATE NX_LAUNCHER_PREPARE)
    endif()
endif()

foreach(t ${NX_PREPARE_TARGETS})
    if(MSVC)
        target_compile_options(${t} PRIVATE /O2 /W3 /utf-8)
        target_compile_definitions(${t} PRIVATE _CRT_SECURE_NO_WARNINGS _SILENCE_CXX20_U8PATH_DEPRECATION_WARNING)
    else()
        target_compile_options(${t} PRIVATE -O2 -Wall -Wextra)
    endif()
    # No /arch:AVX2 (the top CMakeLists.txt adds it on Windows): the preparation runs on any x86-64, and tells the
    # player about a CPU the game cannot run on rather than crashing.
    get_target_property(opts ${t} COMPILE_OPTIONS)
    if(opts)
        list(REMOVE_ITEM opts /arch:AVX2)
        set_target_properties(${t} PROPERTIES COMPILE_OPTIONS "${opts}")
    endif()
endforeach()

# Tests (-DNX_TESTS=ON), synthetic inputs only (ctest -R prepare):
#   prepare_e2e           the stages and acnh_prepare end to end, the recompile skipped and a fake shader tool
#   prepare_e2e_zig       the Compile stage with a real zig (NX_PREPARE_ZIG_DIR, fetched there by tools/fetch_zig.py
#                         when missing; skipped when it cannot be downloaded) on tools/make_fake_gen.py's stand-in
#   prepare_launcher      the folders written, checked by launcher/files.cpp (with the player beside it, find_build)
if(NX_TESTS AND NOT ANDROID)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    set(NX_PREPARE_ZIG_DIR "${CMAKE_BINARY_DIR}/zig" CACHE PATH "zig for prepare_e2e_zig (tools/fetch_zig.py)")
    # zig's name for this computer (tools/fetch_zig.py's hosts)
    string(TOLOWER "${CMAKE_HOST_SYSTEM_PROCESSOR}" nx_cpu)
    set(NX_ZIG_HOST "")
    if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux" AND nx_cpu MATCHES "^(x86_64|amd64)$")
        set(NX_ZIG_HOST x86_64-linux)
    elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux" AND nx_cpu MATCHES "^(aarch64|arm64)$")
        set(NX_ZIG_HOST aarch64-linux)
    elseif(CMAKE_HOST_WIN32 AND nx_cpu MATCHES "^(x86_64|amd64)$")
        set(NX_ZIG_HOST x86_64-windows)
    elseif(CMAKE_HOST_APPLE AND nx_cpu MATCHES "^(arm64|aarch64)$")
        set(NX_ZIG_HOST aarch64-macos)
    endif()

    add_executable(fake_shader_tool "${NX_PREPARE_DIR}/fake_shader_tool.cpp")
    add_executable(prepare_e2e_test "${NX_PREPARE_DIR}/prepare_e2e_test.cpp")
    target_link_libraries(prepare_e2e_test PRIVATE nx_prepare ${CMAKE_DL_LIBS})
    target_include_directories(prepare_e2e_test PRIVATE "${NX_PREPARE_ROOT}/runtime/include" "${NX_PREPARE_ROOT}/runtime/src")
    target_compile_definitions(prepare_e2e_test PRIVATE NX_GAME_ABI="${NX_GAME_ABI}")
    add_test(NAME prepare_e2e COMMAND prepare_e2e_test base "${NX_PREPARE_ROOT}" "${CMAKE_BINARY_DIR}/prepare_e2e"
             $<TARGET_FILE:acnh_prepare> $<TARGET_FILE:fake_shader_tool>)
    set_tests_properties(prepare_e2e PROPERTIES TIMEOUT 300 FIXTURES_SETUP prepare_folder)
    add_test(NAME prepare_e2e_zig COMMAND prepare_e2e_test zig "${NX_PREPARE_ROOT}" "${CMAKE_BINARY_DIR}/prepare_e2e_zig"
             $<TARGET_FILE:fake_shader_tool> "${NX_PREPARE_ZIG_DIR}" "${Python3_EXECUTABLE}" "${NX_ZIG_HOST}")
    set_tests_properties(prepare_e2e_zig PROPERTIES TIMEOUT 1800 SKIP_RETURN_CODE 77 RUN_SERIAL ON)

    if(TARGET acnhrecomp_launcher)  # SDL3 found: launcher/files.cpp builds
        add_executable(prepare_launcher_test "${NX_PREPARE_DIR}/prepare_launcher_test.cpp"
                       "${NX_PREPARE_ROOT}/launcher/files.cpp" "${NX_PREPARE_ROOT}/runtime/src/ui/i18n.cpp")
        target_include_directories(prepare_launcher_test PRIVATE "${NX_PREPARE_ROOT}/launcher" "${NX_PREPARE_ROOT}/runtime/src")
        target_compile_definitions(prepare_launcher_test PRIVATE NX_HAVE_SDL3 NX_LAUNCHER_UI)
        target_link_libraries(prepare_launcher_test PRIVATE SDL3::SDL3 Threads::Threads nx_prepare_io)
        if(WIN32)
            target_link_libraries(prepare_launcher_test PRIVATE ole32 shell32 uuid)
        endif()
        if(MSVC)
            target_compile_options(prepare_launcher_test PRIVATE /utf-8)
            target_compile_definitions(prepare_launcher_test PRIVATE _CRT_SECURE_NO_WARNINGS)
        endif()
        # in the build folder, beside the player (acnh): find_build takes the folder as a release does
        set_target_properties(prepare_launcher_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
        add_test(NAME prepare_launcher COMMAND prepare_launcher_test "${CMAKE_BINARY_DIR}/prepare_e2e/out1"
                 "${CMAKE_BINARY_DIR}/prepare_e2e_zig/out_zig" "${CMAKE_BINARY_DIR}/prepare_e2e/in/ACNH")
        set_tests_properties(prepare_launcher PROPERTIES FIXTURES_REQUIRED prepare_folder DEPENDS prepare_e2e_zig)
    endif()
endif()
