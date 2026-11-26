cmake_minimum_required(VERSION 3.25)
project(C2S2C VERSION 1.0.0 LANGUAGES C) # major = ABI major (C2S2C_ABI_VERSION)

# The C++ binding is optional: it is built and tested only if a C++ compiler exists.
include(CheckLanguage)
if(NOT WIN32) # (the C++ binding is not part of the Windows build)
    check_language(CXX)
endif()
if(CMAKE_CXX_COMPILER)
    enable_language(CXX)
    set(CMAKE_CXX_STANDARD 23) # the binding needs C++17; it is built and tested with the latest
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
endif()

set(CMAKE_C_STANDARD 23) # the latest published C standard (ISO/IEC 9899:2024)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF) # strict ISO C11: no accidental GNU-only code
set(CMAKE_C_VISIBILITY_PRESET hidden) # export only C2S2C_API symbols (see c2s2c_export.h)
set(CMAKE_POSITION_INDEPENDENT_CODE ON) # the static lib can also be linked into shared objects

# Strict ISO C11 hides POSIX functions (clock_gettime, sockets options) on glibc unless asked.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_compile_definitions(_GNU_SOURCE)
endif()

# The Windows C library marks fopen, getenv... "unsafe" and prefers its own _s variants; the code
# is portable C and checks its arguments, so those warnings are switched off.
if(WIN32)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()

list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/CMake)
include(compiler_warnings)
include(sanitizers)
include(coverage)

# One source list feeds both libraries so they can never diverge.
set(C2S2C_SOURCES Source/Core/version.c Source/Core/header.c Source/Codec/encode.c
    Source/Codec/decode.c Source/Codec/utf8.c Source/Core/registry.c Source/Memory/ring.c Source/Memory/pool.c
    Source/IO/io.c Source/IO/io_poll.c
    Source/Core/Ctx/context.c Source/Core/Ctx/queue.c Source/Core/Ctx/listener.c
    Source/Core/Ctx/poll.c Source/Core/Conn/lifecycle.c Source/Core/Conn/sending.c
    Source/Core/Conn/receiving.c Source/Core/Conn/events.c Source/Core/Conn/maintenance.c
    Source/Simd/cpu_detect.c Source/Simd/crc32_generic.c Source/Simd/crc32_sse42.c
    Source/Simd/crc32_armv8.c Source/Simd/crc32_none.c
    Source/Core/Ctx/tunnel.c
    Generated/C/c2s2c_registry.c)

# Socket layer: Winsock on Windows, BSD sockets elsewhere.
if(WIN32)
    list(APPEND C2S2C_SOURCES Source/IO/net_win32.c)
else()
    list(APPEND C2S2C_SOURCES Source/IO/net.c)
endif()

# Native I/O backends exist only on their own platform.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND C2S2C_SOURCES Source/IO/io_epoll.c Source/IO/io_uring.c)
elseif(APPLE OR CMAKE_SYSTEM_NAME MATCHES "BSD")
    list(APPEND C2S2C_SOURCES Source/IO/io_kqueue.c)
endif()

# Shared lib: what the bindings load (c2s2c_export.h switches on dllexport on Windows).
add_library(c2s2c SHARED ${C2S2C_SOURCES})
target_compile_definitions(c2s2c PRIVATE C2S2C_BUILDING_SHARED)
# Static lib: used by unit tests, which need to call internal helpers and avoid a loader step.
add_library(c2s2c_static STATIC ${C2S2C_SOURCES})
set_target_properties(c2s2c_static PROPERTIES OUTPUT_NAME c2s2c_static)
# Same strict warnings (-Werror) on both so a warning cannot hide in one variant.
foreach(t c2s2c c2s2c_static)
    # Generated/C holds c2s2c_packets.h (packet ids), which users include too.
    target_include_directories(${t} PUBLIC Include Generated/C)
    # Private sources and internal headers (Source/Internal) are not visible to users.
    target_include_directories(${t} PRIVATE Source Source/Internal)
    c2s2c_enable_warnings(${t})
    if(WIN32)
        target_link_libraries(${t} PUBLIC ws2_32) # Winsock
    endif()
endforeach()

# Shared library version: the file name carries the ABI major (libc2s2c.so.2, libc2s2c.2.dylib).
set_target_properties(c2s2c PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR})

# Installation layout, also the content of the release archives (see .github/workflows/release.yml):
#   include/C2S2C/*.h, include/c2s2c_packets.h   the C interface and the packet ids
#   lib/ (and bin/ on Windows)                      shared and static libraries
#   share/c2s2c/                                    protocol.toml, registry.txt, README.md, LICENSE
#   lib/pkgconfig/c2s2c.pc                          for C/C++ build systems
# Nothing in it is specific to one language: any language that can call a C library can use it.
include(GNUInstallDirs)
install(TARGETS c2s2c c2s2c_static
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY Include/C2S2C DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES Generated/C/c2s2c_packets.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES Registry/protocol.toml Generated/registry.txt LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/c2s2c)
install(FILES Tools/Release/package_readme.md DESTINATION ${CMAKE_INSTALL_DATADIR}/c2s2c RENAME README.md)
configure_file(CMake/c2s2c.pc.in ${CMAKE_BINARY_DIR}/c2s2c.pc @ONLY)
install(FILES ${CMAKE_BINARY_DIR}/c2s2c.pc DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

enable_testing()
add_subdirectory(Tests)
