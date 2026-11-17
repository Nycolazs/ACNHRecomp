# The recompiler of the game folder's preparation (prepare/README.md): recomp/*.py ported to C++ (nx_recomp), over
# Capstone 5.0.7 (third_party/capstone/, the version the Python recompiler runs with), its command line (acnh_recomp,
# with python -m recomp's arguments) and its tests. Included by the top CMakeLists.txt on desktop; nx_recomp itself is
# plain C++20 with no desktop-only code, so it builds for Android too.
set(NX_RECOMP_DIR "${CMAKE_CURRENT_LIST_DIR}")
get_filename_component(NX_RECOMP_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)  # the repository
set(NX_CAPSTONE_DIR "${NX_RECOMP_ROOT}/third_party/capstone")
find_package(Threads REQUIRED)

file(GLOB NX_CAPSTONE_SRC "${NX_CAPSTONE_DIR}/*.c" "${NX_CAPSTONE_DIR}/arch/AArch64/*.c")
add_library(nx_capstone STATIC ${NX_CAPSTONE_SRC})
target_include_directories(nx_capstone PUBLIC "${NX_CAPSTONE_DIR}/include" PRIVATE "${NX_CAPSTONE_DIR}")
target_compile_definitions(nx_capstone PRIVATE CAPSTONE_HAS_ARM64 CAPSTONE_USE_SYS_DYN_MEM)
set_target_properties(nx_capstone PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    target_compile_options(nx_capstone PRIVATE /O2 /W0)
else()
    target_compile_options(nx_capstone PRIVATE -O2 -w)
endif()

# games/acnh/versions.txt and hooks*.txt (which hooks the recompiled main gets per build) and tmouh's SpeedFix for main
# (acnh/speedfix-*.pchtxt, see acnh/SPEEDFIX.md), embedded (acnh_data.cpp) in pieces of 8000 characters: MSVC takes no
# single string literal over 16380 bytes (adjacent ones are joined).
file(GLOB NX_ACNH_DATA "${NX_RECOMP_ROOT}/games/acnh/versions.txt" "${NX_RECOMP_ROOT}/games/acnh/hooks*.txt"
     "${NX_RECOMP_DIR}/acnh/*.pchtxt")
set(NX_ACNH_INC "${CMAKE_CURRENT_BINARY_DIR}/prepare/acnh_data.inc")
set(NX_ACNH_TEXT "")
foreach(f ${NX_ACNH_DATA})
    get_filename_component(n "${f}" NAME)
    file(READ "${f}" t)
    string(APPEND NX_ACNH_TEXT "{\"${n}\",\n")
    string(LENGTH "${t}" len)
    set(i 0)
    while(i LESS len)
        string(SUBSTRING "${t}" ${i} 8000 piece)
        string(APPEND NX_ACNH_TEXT "R\"NXDATA(${piece})NXDATA\"\n")
        math(EXPR i "${i} + 8000")
    endwhile()
    string(APPEND NX_ACNH_TEXT "},\n")
endforeach()
file(WRITE "${NX_ACNH_INC}.tmp" "${NX_ACNH_TEXT}")
configure_file("${NX_ACNH_INC}.tmp" "${NX_ACNH_INC}" COPYONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${NX_ACNH_DATA})

file(GLOB NX_RECOMP_SRC "${NX_RECOMP_DIR}/*.cpp")
list(REMOVE_ITEM NX_RECOMP_SRC "${NX_RECOMP_DIR}/recomp_cli.cpp" "${NX_RECOMP_DIR}/recomp_test.cpp"
     "${NX_RECOMP_DIR}/recomp_source_test.cpp")
add_library(nx_recomp STATIC ${NX_RECOMP_SRC})
target_include_directories(nx_recomp PUBLIC "${NX_RECOMP_DIR}" PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/prepare")
target_link_libraries(nx_recomp PUBLIC nx_capstone Threads::Threads)
target_compile_features(nx_recomp PUBLIC cxx_std_20)
set_target_properties(nx_recomp PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    target_compile_options(nx_recomp PRIVATE /O2 /W3 /utf-8)
else()
    target_compile_options(nx_recomp PRIVATE -O2 -Wall -Wextra)
endif()

add_executable(acnh_recomp "${NX_RECOMP_DIR}/recomp_cli.cpp")
target_link_libraries(acnh_recomp PRIVATE nx_recomp)
# its command line as UTF-8 (prepare/io/args_utf8.h: CommandLineToArgvW on Windows)
target_include_directories(acnh_recomp PRIVATE "${NX_RECOMP_DIR}/../io")
if(WIN32)
    target_link_libraries(acnh_recomp PRIVATE shell32)
endif()

# No /arch:AVX2 (the top CMakeLists.txt adds it on Windows): the recompiler only reads and writes text, and runs on any
# x86-64, as ACNHRecomp-Importer's build of it did.
foreach(t nx_capstone nx_recomp acnh_recomp)
    get_target_property(opts ${t} COMPILE_OPTIONS)
    if(opts)
        list(REMOVE_ITEM opts /arch:AVX2)
        set_target_properties(${t} PROPERTIES COMPILE_OPTIONS "${opts}")
    endif()
endforeach()

# recomp_test: the analysis on a tiny hand-made module (no game code). recomp_source_test: recomp/*.py is still the
# version this port was made from (SOURCE: tools/setup_acnh.py's recomp_hash()).
if(NX_TESTS)
    add_executable(recomp_test "${NX_RECOMP_DIR}/recomp_test.cpp")
    target_link_libraries(recomp_test PRIVATE nx_recomp)
    add_test(NAME recomp_test COMMAND recomp_test)
    add_executable(recomp_source_test "${NX_RECOMP_DIR}/recomp_source_test.cpp")
    target_include_directories(recomp_source_test PRIVATE "${NX_RECOMP_ROOT}/runtime/src")
    add_test(NAME recomp_source_test COMMAND recomp_source_test "${NX_RECOMP_ROOT}" "${NX_RECOMP_DIR}/SOURCE")
endif()
