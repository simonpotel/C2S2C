cmake_minimum_required(VERSION 3.20)
project(Duplex VERSION 0.1.0 LANGUAGES C)

# The C++ binding is optional: it is built and tested only if a C++ compiler exists.
include(CheckLanguage)
check_language(CXX)
if(CMAKE_CXX_COMPILER)
    enable_language(CXX)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF) # strict ISO C11: no accidental GNU-only code
set(CMAKE_C_VISIBILITY_PRESET hidden) # export only DX_API symbols (see dx_export.h)
set(CMAKE_POSITION_INDEPENDENT_CODE ON) # the static lib can also be linked into shared objects

# Strict ISO C11 hides POSIX functions (clock_gettime, sockets options) on glibc unless asked.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_compile_definitions(_GNU_SOURCE)
endif()

list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/CMake)
include(compiler_warnings)
include(sanitizers)

# One source list feeds both libraries so they can never diverge.
set(DX_SOURCES Source/Core/version.c Source/Core/header.c Source/Codec/encode.c
    Source/Codec/decode.c Source/Codec/utf8.c Source/Core/registry.c Source/Memory/ring.c Source/Memory/pool.c
    Source/IO/io.c Source/IO/io_poll.c Source/IO/net.c
    Source/Core/Ctx/context.c Source/Core/Ctx/queue.c Source/Core/Ctx/listener.c
    Source/Core/Ctx/poll.c Source/Core/Conn/lifecycle.c Source/Core/Conn/sending.c
    Source/Core/Conn/receiving.c Source/Core/Conn/events.c Source/Core/Conn/maintenance.c
    Source/Core/Hooks/hooks.c Source/Core/Hooks/dispatch.c Source/Core/Hooks/tunnel.c
    Generated/C/dx_registry.c)

# Native I/O backends exist only on their own platform.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND DX_SOURCES Source/IO/io_epoll.c)
elseif(APPLE OR CMAKE_SYSTEM_NAME MATCHES "BSD")
    list(APPEND DX_SOURCES Source/IO/io_kqueue.c)
endif()

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
    # Private sources and internal headers (Source/Internal) are not visible to users.
    target_include_directories(${t} PRIVATE Source Source/Internal)
    dx_enable_warnings(${t})
endforeach()

add_subdirectory(Examples)

enable_testing()
add_subdirectory(Tests)
