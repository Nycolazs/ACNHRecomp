# nx_prepare_io (prepare/io/): the input side of preparing the game folder, included by the top CMakeLists.txt. What
# the player gives (an ExeFS and a RomFS extracted with their emulator, inputs.h), the executables read and made flat
# images (nso.h), and the packs (romfs_pak.h, spirv_pak.h). Plain C++20 and stdio, no game data, no keys: it builds for
# Android as for the desktop.

# games/acnh/versions.txt into the library (the known build ids of main), rewritten only when it changes.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/games/acnh/versions.txt")
file(READ "${CMAKE_SOURCE_DIR}/games/acnh/versions.txt" NX_PREPARE_VERSIONS)
file(CONFIGURE OUTPUT "${CMAKE_BINARY_DIR}/prepare_io/prepare_versions.inc"
     CONTENT "static const char kVersionsTxt[] = R\"nxv(@NX_PREPARE_VERSIONS@)nxv\";\n" @ONLY)

add_library(nx_prepare_io STATIC
    prepare/io/nso.cpp prepare/io/inputs.cpp prepare/io/pak.cpp prepare/io/romfs_pak.cpp prepare/io/spirv_pak.cpp)
target_include_directories(nx_prepare_io PUBLIC prepare/io PRIVATE runtime/src "${CMAKE_BINARY_DIR}/prepare_io")
target_compile_features(nx_prepare_io PUBLIC cxx_std_20)
if(MSVC)
    target_compile_options(nx_prepare_io PRIVATE /utf-8)
    target_compile_definitions(nx_prepare_io PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
# No /arch:AVX2 (the top CMakeLists.txt adds it on Windows): the launcher links it (the probe of the player's picks), and
# it must start on any x86-64 to tell the player about a CPU the game cannot run on.
get_target_property(nx_io_opts nx_prepare_io COMPILE_OPTIONS)
if(nx_io_opts)
    list(REMOVE_ITEM nx_io_opts /arch:AVX2)
    set_target_properties(nx_prepare_io PROPERTIES COMPILE_OPTIONS "${nx_io_opts}")
endif()
if(TARGET acnhrecomp_launcher)
    target_link_libraries(acnhrecomp_launcher PRIVATE nx_prepare_io)
endif()

if(NX_TESTS)
    # Synthetic ExeFS and RomFS only. The packs are compared with tools/pack_romfs.py's and tools/pack_spirv.py's, and
    # the flat images with tools/nso2bin.py's (it needs Python's lz4), when Python is there.
    find_package(Python3 COMPONENTS Interpreter QUIET)
    add_executable(prepare_io_test prepare/io/prepare_io_test.cpp)
    target_link_libraries(prepare_io_test PRIVATE nx_prepare_io)
    target_include_directories(prepare_io_test PRIVATE runtime/src)
    add_test(NAME prepare_io_test COMMAND prepare_io_test "${CMAKE_SOURCE_DIR}" "$<$<BOOL:${Python3_FOUND}>:${Python3_EXECUTABLE}>")
endif()
