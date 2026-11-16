# The rest of the game folder's preparation (prepare/README.md), included by the top CMakeLists.txt after
# recomp/recomp.cmake (nx_recomp) and with NX_GAME_ABI computed:
#   nx_gamelib    the game's library for each platform, compiled with zig from the recompiled code (gamelib/)
# nx_gamelib is plain C++20 and the OS's process calls: it builds for Android too.
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

foreach(t nx_gamelib)
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

