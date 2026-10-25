cmake_minimum_required(VERSION 3.20)
project(cod1-port C)

# ccache, when installed: the generated code is large and mostly unchanged between builds.
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# The recompiled code is generated from the user's own copy of the game: the
# single-player executable and the three game DLLs it loads.
set(COD_DIR "${CMAKE_SOURCE_DIR}/gamedata/pc" CACHE PATH "Game directory (CoDSP.exe, gamex86.dll, cgamex86.dll, uix86.dll)")
foreach(f CoDSP.exe gamex86.dll cgamex86.dll uix86.dll)
    if(NOT EXISTS "${COD_DIR}/${f}")
        message(FATAL_ERROR "${f} not found in ${COD_DIR}; pass -DCOD_DIR=/path/to/the/game")
    endif()
endforeach()
# Guest bases of the DLLs: packed after CoDSP.exe's image (0x00400000-0x01864000),
# below the thunks at 0x02000000 (runtime/include/runtime.h).
set(COD_MODULES
    "${COD_DIR}/CoDSP.exe"
    "${COD_DIR}/gamex86.dll@01870000"
    "${COD_DIR}/cgamex86.dll@01B60000"
    "${COD_DIR}/uix86.dll@01DE0000")

set(COD_PORT_VERSION "0.1.0")

find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(GEN_DIR "${CMAKE_BINARY_DIR}/gen")
file(GLOB RECOMP_TOOLS "${CMAKE_SOURCE_DIR}/tools/recomp/*.py")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${RECOMP_TOOLS}
    "${COD_DIR}/CoDSP.exe" "${COD_DIR}/gamex86.dll" "${COD_DIR}/cgamex86.dll" "${COD_DIR}/uix86.dll")
# Diagnostic entry probes (hex guest addresses), e.g. -DCOD_PROBES="452190 4521B6".
set(COD_PROBES "" CACHE STRING "Guest functions to instrument with rt_probe")

message(STATUS "Recompiling ${COD_DIR}")
execute_process(
    COMMAND ${CMAKE_COMMAND} -E env "FF_PROBES=${COD_PROBES}"
            "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/recomp/gen.py" "${GEN_DIR}" ${COD_MODULES}
    RESULT_VARIABLE GEN_RESULT
    OUTPUT_VARIABLE GEN_OUTPUT)
if(NOT GEN_RESULT EQUAL 0)
    message(FATAL_ERROR "recompiler failed:\n${GEN_OUTPUT}")
endif()
message(STATUS "${GEN_OUTPUT}")

file(STRINGS "${GEN_DIR}/files.txt" GEN_FILES)
list(TRANSFORM GEN_FILES PREPEND "${GEN_DIR}/")

add_library(cod_recomp STATIC ${GEN_FILES})
target_include_directories(cod_recomp PUBLIC runtime/include "${GEN_DIR}")
# Guest code relies on x86 semantics: wrapping signed arithmetic, type punning
# and signed char.
set(COD_RECOMP_OPT "-O2" CACHE STRING "Optimization flag of the generated code")
target_compile_options(cod_recomp PRIVATE -Wall -Wextra ${COD_RECOMP_OPT} -fwrapv -fno-strict-aliasing -fsigned-char
                       -Wno-unused-label -Wno-unused-variable -Wno-unused-but-set-variable)
option(COD_X87_CHECK "Check statically derived x87 stack effects at run time" OFF)
if(COD_X87_CHECK)
    target_compile_definitions(cod_recomp PRIVATE FF_X87_CHECK)
endif()

file(GLOB RUNTIME_SOURCES CONFIGURE_DEPENDS runtime/src/*.c)
file(GLOB SHIM_SOURCES CONFIGURE_DEPENDS shim/src/*.c)
if(NOT VITA)
    list(FILTER SHIM_SOURCES EXCLUDE REGEX "vita_[a-z]+\\.c$|platform_vita\\.c$")
endif()

add_executable(cod1 ${RUNTIME_SOURCES} ${SHIM_SOURCES})
target_include_directories(cod1 PRIVATE runtime/include shim/include "${GEN_DIR}")
target_compile_options(cod1 PRIVATE -Wall -Wextra -fwrapv -fno-strict-aliasing -fsigned-char)
target_compile_definitions(cod1 PRIVATE COD_PORT_VERSION="${COD_PORT_VERSION}" FF_PORT_VERSION="${COD_PORT_VERSION}")
option(COD_PROFILE "Build with the sampling profiler" OFF)
if(COD_PROFILE)
    target_compile_definitions(cod_recomp PUBLIC FF_PROFILE)
    target_compile_definitions(cod1 PRIVATE FF_PROFILE)
endif()

find_package(SDL2 REQUIRED)
find_package(OpenGL REQUIRED)
target_link_libraries(cod1 PRIVATE cod_recomp SDL2::SDL2 OpenGL::GL m)
if(APPLE)
    target_link_libraries(cod1 PRIVATE "-framework CoreFoundation")
    # The legacy (compatibility) OpenGL API the game uses is deprecated on macOS.
    target_compile_definitions(cod1 PRIVATE GL_SILENCE_DEPRECATION)
else()
    target_link_libraries(cod1 PRIVATE pthread)
endif()
