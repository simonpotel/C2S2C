# Declares one unit-test executable per Tests/Unit/<name>.c and registers it with ctest.
# Linked statically against the library, and given Source/ on the include path so tests can
# also reach internal headers.
# The label lets run_all_tests.py select just the unit tests.
function(c2s2c_unit_test name)
    add_executable(${name} Unit/${name}.c)
    target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/Source ${CMAKE_SOURCE_DIR}/Source/Internal)
    target_link_libraries(${name} PRIVATE c2s2c_static)
    c2s2c_enable_warnings(${name})
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES LABELS Unit)
endfunction()

c2s2c_unit_test(test_header)
c2s2c_unit_test(test_payload)
c2s2c_unit_test(test_registry)
c2s2c_unit_test(test_memory)
c2s2c_unit_test(test_simd)

# Reference vectors: one ctest entry per Vectors/*.bin, so a failure names the exact file.
add_executable(test_vectors Vectors/test_vectors.c)
target_link_libraries(test_vectors PRIVATE c2s2c_static)
c2s2c_enable_warnings(test_vectors)
file(GLOB C2S2C_VECTORS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/Vectors/*.bin)
foreach(bin ${C2S2C_VECTORS})
    get_filename_component(vec ${bin} NAME_WE)
    add_test(NAME vector_${vec} COMMAND test_vectors ${CMAKE_CURRENT_SOURCE_DIR}/Vectors/${vec})
    set_tests_properties(vector_${vec} PROPERTIES LABELS Vectors)
endforeach()

# Repository checks, run by ctest like any other test (see the scripts for what they verify).
# They need a POSIX shell and nm, so they run everywhere except Windows.
if(NOT WIN32)
add_test(NAME naming COMMAND sh ${CMAKE_SOURCE_DIR}/Tools/Scripts/check_naming.sh)
set_tests_properties(naming PROPERTIES LABELS Naming)
add_test(NAME abi COMMAND sh ${CMAKE_SOURCE_DIR}/Tools/Scripts/check_abi.sh $<TARGET_FILE:c2s2c>)
set_tests_properties(abi PROPERTIES LABELS Abi)
endif()

# Randomized round trips (seeded, replayable with C2S2C_SEED).
add_executable(test_property Property/test_property.c)
target_link_libraries(test_property PRIVATE c2s2c_static)
c2s2c_enable_warnings(test_property)
add_test(NAME property COMMAND test_property)
set_tests_properties(property PROPERTIES LABELS Property)

# Fuzzing needs clang's libFuzzer: configure with the `fuzz` preset (C2S2C_FUZZ=ON).
# Each target runs for C2S2C_FUZZ_SECONDS. New inputs it finds go to the build directory (first
# folder); Fuzz/Corpus only holds committed seed files, which are read but never modified.
option(C2S2C_FUZZ "Build libFuzzer targets" OFF)
set(C2S2C_FUZZ_SECONDS 10 CACHE STRING "Seconds per fuzz target")
if(C2S2C_FUZZ)
    add_executable(fuzz_payload Fuzz/fuzz_payload.c)
    target_link_libraries(fuzz_payload PRIVATE c2s2c_static)
    target_compile_options(fuzz_payload PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(fuzz_payload PRIVATE -fsanitize=fuzzer,address,undefined)
    add_test(NAME fuzz_payload
             COMMAND fuzz_payload -max_total_time=${C2S2C_FUZZ_SECONDS} -timeout=5
                     ${CMAKE_CURRENT_BINARY_DIR}/fuzz_corpus ${CMAKE_CURRENT_SOURCE_DIR}/Fuzz/Corpus)
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/fuzz_corpus)
    set_tests_properties(fuzz_payload PROPERTIES LABELS Fuzz)

    # Framing: arbitrary bytes arriving on a live connection (uses internal headers).
    add_executable(fuzz_framing Fuzz/fuzz_framing.c)
    target_include_directories(fuzz_framing PRIVATE ${CMAKE_SOURCE_DIR}/Source ${CMAKE_SOURCE_DIR}/Source/Internal)
    target_link_libraries(fuzz_framing PRIVATE c2s2c_static)
    target_compile_options(fuzz_framing PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(fuzz_framing PRIVATE -fsanitize=fuzzer,address,undefined)
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/fuzz_corpus_framing)
    add_test(NAME fuzz_framing
             COMMAND fuzz_framing -max_total_time=${C2S2C_FUZZ_SECONDS} -timeout=5
                     ${CMAKE_CURRENT_BINARY_DIR}/fuzz_corpus_framing ${CMAKE_CURRENT_SOURCE_DIR}/Fuzz/Corpus)
    set_tests_properties(fuzz_framing PROPERTIES LABELS Fuzz)
endif()

# Generated/ must match Registry/protocol.toml. Needs Python 3.11+ (tomllib); skipped if absent.
find_program(C2S2C_PYTHON NAMES python3.14 python3.13 python3.12 python3.11)
if(C2S2C_PYTHON)
    add_test(NAME registry_up_to_date
             COMMAND ${C2S2C_PYTHON} ${CMAKE_SOURCE_DIR}/Tools/Codegen/gen_registry.py --check)
    set_tests_properties(registry_up_to_date PROPERTIES LABELS Registry)
else()
    message(WARNING "Python 3.11+ not found: registry_up_to_date test is not registered")
endif()

# One test per generated language: its tables must equal Generated/registry.txt.
# C always runs; the others run only if their toolchain is installed.
set(C2S2C_GOLDEN ${CMAKE_SOURCE_DIR}/Generated/registry.txt)
set(C2S2C_GEN_TESTS ${CMAKE_SOURCE_DIR}/Tests/Generated)

add_executable(test_generated_c Generated/C/test_generated.c)
target_link_libraries(test_generated_c PRIVATE c2s2c_static)
c2s2c_enable_warnings(test_generated_c)
add_test(NAME generated_c COMMAND test_generated_c ${C2S2C_GOLDEN})

set_tests_properties(generated_c PROPERTIES LABELS Generated)
if(NOT WIN32)
find_program(C2S2C_CXX NAMES c++ clang++ g++)
if(C2S2C_CXX)
    add_test(NAME generated_cpp COMMAND sh -c
        "${C2S2C_CXX} -std=c++23 -Wall -Wextra -Werror -I${CMAKE_SOURCE_DIR}/Generated/Cpp ${C2S2C_GEN_TESTS}/Cpp/test_generated.cpp -o test_generated_cpp && ./test_generated_cpp ${C2S2C_GOLDEN}")
endif()
find_program(C2S2C_RUSTC rustc)
if(C2S2C_RUSTC)
    add_test(NAME generated_rust COMMAND sh -c
        "${C2S2C_RUSTC} --edition 2024 -D warnings ${C2S2C_GEN_TESTS}/Rust/test_generated.rs -o test_generated_rust && ./test_generated_rust ${C2S2C_GOLDEN}")
endif()
foreach(lang cpp rust)
    if(TEST generated_${lang})
        set_tests_properties(generated_${lang} PROPERTIES LABELS Generated)
    endif()
endforeach()
endif()

# Tests that run real sockets over loopback. Each one is a C program with its own label.
function(c2s2c_net_test name dir label)
    add_executable(${name} ${dir}/${name}.c)
    target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/Source ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(${name} PRIVATE c2s2c_static)
    c2s2c_enable_warnings(${name})
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES LABELS ${label} TIMEOUT 120)
endfunction()

c2s2c_net_test(test_integration Integration Integration)
c2s2c_net_test(test_api Integration Integration)
c2s2c_net_test(test_noalloc Memory Memory)

# Windows validates the library, the C tests, integration and memory tests. Everything below needs
# a POSIX system (raw sockets, pthreads, sh, the other languages' toolchains).
if(WIN32)
    return()
endif()

c2s2c_net_test(test_faults Faults Faults)
c2s2c_net_test(test_tunnel Integration Integration)
c2s2c_net_test(test_threads Concurrency Concurrency)
c2s2c_net_test(test_stress Stress Stress)
# Short by default; `C2S2C_STRESS_SECONDS=600 ctest -L Stress` runs the long version.
find_package(Threads REQUIRED)
target_link_libraries(test_threads PRIVATE Threads::Threads)

# Rules the compiler cannot check: allocation only in context.c, no printing in the library.
add_test(NAME source_rules COMMAND sh ${CMAKE_SOURCE_DIR}/Tools/Scripts/check_source_rules.sh)
set_tests_properties(source_rules PROPERTIES LABELS Memory)

# C++ binding: its own tests.
if(CMAKE_CXX_COMPILER)
    add_executable(test_cpp Bindings/Cpp/test_cpp.cpp)
    target_include_directories(test_cpp PRIVATE ${CMAKE_SOURCE_DIR}/Bindings/Cpp ${CMAKE_SOURCE_DIR}/Generated/Cpp
                                                ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(test_cpp PRIVATE c2s2c_static)
    c2s2c_enable_warnings(test_cpp)
    add_test(NAME test_cpp COMMAND test_cpp)
    set_tests_properties(test_cpp PROPERTIES LABELS Bindings TIMEOUT 60)

endif()

# Python binding: a copy of the package in the layout `pip install` would give, so tests and
# examples import it exactly like a user would (PYTHONPATH + C2S2C_LIBRARY).
# (Not under sanitizers: an instrumented libc2s2c cannot be loaded by uninstrumented programs.)
find_program(C2S2C_PYTHON_ANY NAMES python3)
if(C2S2C_PYTHON_ANY AND NOT C2S2C_SANITIZE)
    set(C2S2C_PYPKG ${CMAKE_BINARY_DIR}/PythonPackage)
    file(GLOB C2S2C_PY_SOURCES ${CMAKE_SOURCE_DIR}/Bindings/Python/Source/*.py)
    foreach(f ${C2S2C_PY_SOURCES})
        get_filename_component(fname ${f} NAME)
        configure_file(${f} ${C2S2C_PYPKG}/c2s2c/${fname} COPYONLY)
    endforeach()
    set(C2S2C_PY_ENV "PYTHONPATH=${C2S2C_PYPKG}" "C2S2C_LIBRARY=$<TARGET_FILE:c2s2c>")

    add_test(NAME test_python COMMAND ${C2S2C_PYTHON_ANY} ${CMAKE_CURRENT_SOURCE_DIR}/Bindings/Python/test_python.py)
    set_tests_properties(test_python PROPERTIES LABELS Bindings TIMEOUT 120 ENVIRONMENT "${C2S2C_PY_ENV}")

    add_test(NAME example_chat_python COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/Examples/chat_example.sh
             "${C2S2C_PYTHON_ANY} ${CMAKE_SOURCE_DIR}/Examples/Python/Server/chat_server.py"
             "${C2S2C_PYTHON_ANY} ${CMAKE_SOURCE_DIR}/Examples/Python/Client/chat_client.py")
    set_tests_properties(example_chat_python PROPERTIES LABELS Examples TIMEOUT 60 ENVIRONMENT "${C2S2C_PY_ENV}")
    set_property(GLOBAL APPEND PROPERTY C2S2C_IMPLS
        "Python|${C2S2C_PYTHON_ANY} ${CMAKE_SOURCE_DIR}/Examples/Python/Server/chat_server.py|${C2S2C_PYTHON_ANY} ${CMAKE_SOURCE_DIR}/Examples/Python/Client/chat_client.py")
endif()

# Rust binding: cargo builds the workspace against this build's libc2s2c, then cargo runs the
# binding's tests and the two chat examples join the cross-language matrix.
find_program(C2S2C_CARGO cargo)
if(C2S2C_CARGO AND NOT C2S2C_SANITIZE)
    set(C2S2C_RUST_DIR ${CMAKE_SOURCE_DIR}/Bindings/Rust)
    # Release builds of the library get an optimized Rust binding too (benchmarks need it).
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        set(C2S2C_CARGO_PROFILE --release)
        set(C2S2C_CARGO_DIR release)
    else()
        set(C2S2C_CARGO_DIR debug)
    endif()
    set(C2S2C_RUST_ENV C2S2C_LIB_DIR=$<TARGET_FILE_DIR:c2s2c>)
    add_custom_target(rust_build ALL
        COMMAND ${CMAKE_COMMAND} -E env ${C2S2C_RUST_ENV} ${C2S2C_CARGO} build ${C2S2C_CARGO_PROFILE} --offline --quiet
        WORKING_DIRECTORY ${C2S2C_RUST_DIR} DEPENDS c2s2c COMMENT "Building the Rust binding")
    add_test(NAME test_rust COMMAND ${CMAKE_COMMAND} -E env ${C2S2C_RUST_ENV} ${C2S2C_CARGO} test ${C2S2C_CARGO_PROFILE} --offline --quiet
             WORKING_DIRECTORY ${C2S2C_RUST_DIR})
    set_tests_properties(test_rust PROPERTIES LABELS Bindings TIMEOUT 300)
    set(C2S2C_RUST_BIN ${CMAKE_SOURCE_DIR}/Build/Rust/${C2S2C_CARGO_DIR})
    add_test(NAME example_chat_rust COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/Examples/chat_example.sh
             ${C2S2C_RUST_BIN}/chat_server_rust ${C2S2C_RUST_BIN}/chat_client_rust)
    set_tests_properties(example_chat_rust PROPERTIES LABELS Examples TIMEOUT 60)
    set_property(GLOBAL APPEND PROPERTY C2S2C_IMPLS "Rust|${C2S2C_RUST_BIN}/chat_server_rust|${C2S2C_RUST_BIN}/chat_client_rust")
endif()

# Node binding: the N-API addon is built here (headers come from the installed Node), then the
# binding's tests and the chat examples run with Node's built-in type stripping (.ts directly).
find_program(C2S2C_NODE_BIN node)
if(C2S2C_NODE_BIN AND NOT C2S2C_SANITIZE)
    execute_process(COMMAND ${C2S2C_NODE_BIN} -p "require('path').resolve(process.execPath,'../../include/node')"
                    OUTPUT_VARIABLE C2S2C_NODE_INCLUDE OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    execute_process(COMMAND ${C2S2C_NODE_BIN} --experimental-strip-types -e "" RESULT_VARIABLE C2S2C_NODE_STRIP
                    OUTPUT_QUIET ERROR_QUIET)
    if(EXISTS "${C2S2C_NODE_INCLUDE}/node_api.h" AND C2S2C_NODE_STRIP EQUAL 0)
        add_library(c2s2c_node MODULE ${CMAKE_SOURCE_DIR}/Bindings/Node/Source/binding.c)
        set_target_properties(c2s2c_node PROPERTIES PREFIX "" SUFFIX ".node" OUTPUT_NAME c2s2c)
        target_include_directories(c2s2c_node SYSTEM PRIVATE ${C2S2C_NODE_INCLUDE})
        target_include_directories(c2s2c_node PRIVATE ${CMAKE_SOURCE_DIR}/Include)
        target_link_libraries(c2s2c_node PRIVATE c2s2c)
        if(APPLE)
            target_link_options(c2s2c_node PRIVATE -undefined dynamic_lookup) # N-API comes from node itself
        endif()
        # (The addon is a MODULE named c2s2c.node; the shared library is libc2s2c: no clash.)
        set(C2S2C_NODE_ENV C2S2C_ADDON=$<TARGET_FILE:c2s2c_node>)
        set(C2S2C_NODE_RUN "${C2S2C_NODE_BIN} --experimental-strip-types --no-warnings") # one string: it is split by the shell script

        add_test(NAME test_node COMMAND ${C2S2C_NODE_BIN} --experimental-strip-types --no-warnings --test
                 ${CMAKE_CURRENT_SOURCE_DIR}/Bindings/Node/test_node.ts)
        set_tests_properties(test_node PROPERTIES LABELS Bindings TIMEOUT 120 ENVIRONMENT "${C2S2C_NODE_ENV}")

        set(C2S2C_NODE_SERVER "${C2S2C_NODE_RUN} ${CMAKE_SOURCE_DIR}/Examples/Node/Server/chat_server.ts")
        set(C2S2C_NODE_CLIENT "${C2S2C_NODE_RUN} ${CMAKE_SOURCE_DIR}/Examples/Node/Client/chat_client.ts")
        add_test(NAME example_chat_node COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/Examples/chat_example.sh
                 "${C2S2C_NODE_SERVER}" "${C2S2C_NODE_CLIENT}")
        set_tests_properties(example_chat_node PROPERTIES LABELS Examples TIMEOUT 60 ENVIRONMENT "${C2S2C_NODE_ENV}")
        set_property(GLOBAL APPEND PROPERTY C2S2C_IMPLS "Node|${C2S2C_NODE_SERVER}|${C2S2C_NODE_CLIENT}")
        set(C2S2C_EXTRA_ENV ${C2S2C_EXTRA_ENV} ${C2S2C_NODE_ENV})
    endif()
endif()

# WebAssembly codec: built with clang's wasm32 target and wasm-ld (no Emscripten needed), tested
# with the shared vectors, and the web chat run end to end (gateway + WebSocket client code).
# The WebAssembly build needs a clang that knows the wasm32 target (Apple's does not, Homebrew's and
# Linux packages' do) and wasm-ld. Take the first clang that can really compile for wasm32.
set(C2S2C_WASM_CLANG "")
foreach(candidate /opt/homebrew/opt/llvm/bin/clang /usr/local/opt/llvm/bin/clang clang-19 clang-18 clang-17 clang)
    if(NOT C2S2C_WASM_CLANG)
        find_program(C2S2C_WASM_CAND_${candidate} ${candidate})
        if(C2S2C_WASM_CAND_${candidate})
            file(WRITE ${CMAKE_BINARY_DIR}/wasm_probe.c "int c2s2c_probe(void) { return 0; }\n")
            execute_process(COMMAND ${C2S2C_WASM_CAND_${candidate}} --target=wasm32 -c ${CMAKE_BINARY_DIR}/wasm_probe.c
                            -o ${CMAKE_BINARY_DIR}/wasm_probe.o RESULT_VARIABLE probe_rc OUTPUT_QUIET ERROR_QUIET)
            if(probe_rc EQUAL 0)
                set(C2S2C_WASM_CLANG ${C2S2C_WASM_CAND_${candidate}})
            endif()
        endif()
    endif()
endforeach()
find_program(C2S2C_WASM_LD NAMES wasm-ld wasm-ld-19 wasm-ld-18 wasm-ld-17
             PATHS /opt/homebrew/bin /opt/homebrew/opt/llvm/bin /usr/local/bin /usr/lib/llvm-19/bin /usr/lib/llvm-18/bin /usr/lib/llvm-17/bin)
if(C2S2C_WASM_CLANG AND C2S2C_WASM_LD AND TEST test_node)
    set(C2S2C_WASM_FILE ${CMAKE_BINARY_DIR}/Wasm/c2s2c_codec.wasm)
    file(GLOB C2S2C_WASM_SOURCES ${CMAKE_SOURCE_DIR}/Bindings/Wasm/Source/*.c ${CMAKE_SOURCE_DIR}/Source/*/*.c)
    add_custom_command(OUTPUT ${C2S2C_WASM_FILE}
        COMMAND ${CMAKE_COMMAND} -E env C2S2C_WASM_CLANG=${C2S2C_WASM_CLANG} C2S2C_WASM_LD=${C2S2C_WASM_LD}
                sh ${CMAKE_SOURCE_DIR}/Bindings/Wasm/build.sh ${C2S2C_WASM_FILE}
        DEPENDS ${CMAKE_SOURCE_DIR}/Bindings/Wasm/build.sh ${C2S2C_WASM_SOURCES} ${CMAKE_SOURCE_DIR}/Generated/C/c2s2c_registry.c
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} COMMENT "Building the WebAssembly codec")
    add_custom_target(wasm ALL DEPENDS ${C2S2C_WASM_FILE})
    set(C2S2C_WASM_ENV C2S2C_WASM=${C2S2C_WASM_FILE})
    add_test(NAME test_wasm COMMAND ${C2S2C_NODE_BIN} --experimental-strip-types --no-warnings --test
             ${CMAKE_CURRENT_SOURCE_DIR}/Wasm/test_wasm.ts)
    set_tests_properties(test_wasm PROPERTIES LABELS Wasm TIMEOUT 120 ENVIRONMENT "${C2S2C_WASM_ENV}")
    add_test(NAME test_web COMMAND ${C2S2C_NODE_BIN} --experimental-strip-types --no-warnings --test
             ${CMAKE_CURRENT_SOURCE_DIR}/Wasm/test_web.ts)
    set_tests_properties(test_web PROPERTIES LABELS Wasm TIMEOUT 120
                         ENVIRONMENT "${C2S2C_WASM_ENV};${C2S2C_NODE_ENV}")
endif()

# Cross-language matrix: every client implementation against every server implementation, same
# scenario (Tests/Examples/chat_example.sh) and same checks. One ctest entry per pair, named
# cross_<client>_to_<server>. Environment variables of every binding are set for all of them.
get_property(C2S2C_IMPLS GLOBAL PROPERTY C2S2C_IMPLS)
set(C2S2C_CROSS_ENV ${C2S2C_PY_ENV} ${C2S2C_EXTRA_ENV})
foreach(server_impl ${C2S2C_IMPLS})
    string(REPLACE "|" ";" server_parts "${server_impl}")
    list(GET server_parts 0 server_name)
    list(GET server_parts 1 server_cmd)
    foreach(client_impl ${C2S2C_IMPLS})
        string(REPLACE "|" ";" client_parts "${client_impl}")
        list(GET client_parts 0 client_name)
        list(GET client_parts 2 client_cmd)
        add_test(NAME cross_${client_name}_to_${server_name}
                 COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/Examples/chat_example.sh "${server_cmd}" "${client_cmd}")
        set_tests_properties(cross_${client_name}_to_${server_name} PROPERTIES
                             LABELS CrossLanguage TIMEOUT 60 ENVIRONMENT "${C2S2C_CROSS_ENV}")
    endforeach()
endforeach()

# Performance: the benchmark programs (see Tools/Scripts/bench_compare.sh) and the regression
# check against Bench/Baselines. Run with `ctest -L Performance` on a quiet machine.
add_executable(bench_codec ${CMAKE_SOURCE_DIR}/Bench/bench_codec.c)
add_executable(bench_poll ${CMAKE_SOURCE_DIR}/Bench/bench_poll.c)
foreach(b bench_codec bench_poll)
    target_include_directories(${b} PRIVATE ${CMAKE_SOURCE_DIR}/Source ${CMAKE_SOURCE_DIR}/Source/Internal)
    target_link_libraries(${b} PRIVATE c2s2c_static)
    c2s2c_enable_warnings(${b})
endforeach()
# The regression check only makes sense on an optimized build (baselines are Release numbers).
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_test(NAME performance COMMAND sh ${CMAKE_SOURCE_DIR}/Tools/Scripts/bench_compare.sh
             $<TARGET_FILE:bench_codec> $<TARGET_FILE:bench_poll>)
    set_tests_properties(performance PROPERTIES LABELS Performance TIMEOUT 300)
endif()

# Cost of each binding (round trips per second through the language's own API). These only
# report; the C benchmarks above carry the baselines.
if(CMAKE_CXX_COMPILER)
    add_executable(bench_ffi_cpp ${CMAKE_SOURCE_DIR}/Bench/Ffi/bench_ffi.cpp)
    target_include_directories(bench_ffi_cpp PRIVATE ${CMAKE_SOURCE_DIR}/Bindings/Cpp ${CMAKE_SOURCE_DIR}/Generated/Cpp)
    target_link_libraries(bench_ffi_cpp PRIVATE c2s2c)
    add_test(NAME ffi_cpp COMMAND bench_ffi_cpp)
    set_tests_properties(ffi_cpp PROPERTIES LABELS Performance TIMEOUT 60)
endif()
if(C2S2C_PYTHON_ANY AND NOT C2S2C_SANITIZE)
    add_test(NAME ffi_python COMMAND ${C2S2C_PYTHON_ANY} ${CMAKE_SOURCE_DIR}/Bench/Ffi/bench_ffi.py)
    set_tests_properties(ffi_python PROPERTIES LABELS Performance TIMEOUT 60 ENVIRONMENT "${C2S2C_PY_ENV}")
endif()
if(C2S2C_CARGO AND NOT C2S2C_SANITIZE)
    add_test(NAME ffi_rust COMMAND ${C2S2C_RUST_BIN}/bench_ffi_rust)
    set_tests_properties(ffi_rust PROPERTIES LABELS Performance TIMEOUT 60)
endif()
if(TEST test_node)
    add_test(NAME ffi_node COMMAND ${C2S2C_NODE_BIN} --experimental-strip-types --no-warnings ${CMAKE_SOURCE_DIR}/Bench/Ffi/bench_ffi.ts)
    set_tests_properties(ffi_node PROPERTIES LABELS Performance TIMEOUT 60 ENVIRONMENT "${C2S2C_NODE_ENV}")
endif()

# Service benchmark: the same ping/pong service written from scratch in each language and on top
# of libc2s2c, driven by one load generator and one hostile-traffic checker (all in C).
# Run it with Tools/Scripts/bench_service.sh <build dir> (see DOCUMENTATION.md, Performance).
add_executable(bench_service_c ${CMAKE_SOURCE_DIR}/Bench/Service/C/server.c)
add_executable(loadgen ${CMAKE_SOURCE_DIR}/Bench/Service/C/loadgen.c)
foreach(b bench_service_c loadgen)
    target_link_libraries(${b} PRIVATE c2s2c)
    c2s2c_enable_warnings(${b})
endforeach()
if(NOT WIN32)
    add_executable(hostile ${CMAKE_SOURCE_DIR}/Bench/Service/C/hostile.c)
    c2s2c_enable_warnings(hostile)
endif()
