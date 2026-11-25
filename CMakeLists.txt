cmake_minimum_required(VERSION 3.20)
project(Duplex VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF) # strict ISO C11: no accidental GNU-only code
set(CMAKE_C_VISIBILITY_PRESET hidden) # export only DX_API symbols (see dx_export.h)
set(CMAKE_POSITION_INDEPENDENT_CODE ON) # the static lib can also be linked into shared objects

list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/CMake)
include(compiler_warnings)
include(sanitizers)

# One source list feeds both libraries so they can never diverge.
set(DX_SOURCES Source/Core/ctx.c Source/Core/header.c Source/Codec/encode.c
    Source/Codec/decode.c Source/Codec/utf8.c Source/Core/registry.c Source/Memory/ring.c Source/Memory/pool.c
    Generated/C/dx_registry.c)

# Shared lib: what the bindings load (dx_export.h switches on dllexport on Windows).
add_library(duplex SHARED ${DX_SOURCES})
target_compile_definitions(duplex PRIVATE DX_BUILDING_SHARED)
# Static lib: used by unit tests, which need to call internal helpers and avoid a loader step.
add_library(duplex_static STATIC ${DX_SOURCES})
set_target_properties(duplex_static PROPERTIES OUTPUT_NAME duplex_static)
# Same strict warnings (-Werror) on both so a warning cannot hide in one variant.
foreach(t duplex duplex_static)
    # Generated/C holds dx_packets.h (packet ids), which users include too.
    target_include_directories(${t} PUBLIC Include Generated/C)
    dx_enable_warnings(${t})
endforeach()

enable_testing()
add_subdirectory(Tests)
