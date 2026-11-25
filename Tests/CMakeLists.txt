# Declares one unit-test executable per Tests/Unit/<name>.c and registers it with ctest.
# Linked statically against the library, and given Source/ on the include path so tests can
# also reach internal headers.
# The label lets run_all_tests.py select just the unit tests.
function(dx_unit_test name)
    add_executable(${name} Unit/${name}.c)
    target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/Source)
    target_link_libraries(${name} PRIVATE duplex_static)
    dx_enable_warnings(${name})
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES LABELS Unit)
endfunction()

dx_unit_test(test_header)
dx_unit_test(test_payload)
dx_unit_test(test_registry)

# Reference vectors: one ctest entry per Vectors/*.bin, so a failure names the exact file.
add_executable(test_vectors Vectors/test_vectors.c)
target_link_libraries(test_vectors PRIVATE duplex_static)
dx_enable_warnings(test_vectors)
file(GLOB DX_VECTORS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/Vectors/*.bin)
foreach(bin ${DX_VECTORS})
    get_filename_component(vec ${bin} NAME_WE)
    add_test(NAME vector_${vec} COMMAND test_vectors ${CMAKE_CURRENT_SOURCE_DIR}/Vectors/${vec})
    set_tests_properties(vector_${vec} PROPERTIES LABELS Vectors)
endforeach()

# Repository checks, run by ctest like any other test (see the scripts for what they verify).
add_test(NAME naming COMMAND sh ${CMAKE_SOURCE_DIR}/Tools/Scripts/check_naming.sh)
set_tests_properties(naming PROPERTIES LABELS Naming)
add_test(NAME abi COMMAND sh ${CMAKE_SOURCE_DIR}/Tools/Scripts/check_abi.sh $<TARGET_FILE:duplex>)
set_tests_properties(abi PROPERTIES LABELS Abi)

# Randomized round trips (seeded, replayable with DX_SEED).
add_executable(test_property Property/test_property.c)
target_link_libraries(test_property PRIVATE duplex_static)
dx_enable_warnings(test_property)
add_test(NAME property COMMAND test_property)
set_tests_properties(property PROPERTIES LABELS Property)

# Fuzzing needs clang's libFuzzer: configure with the `fuzz` preset (DX_FUZZ=ON).
# Each target runs for DX_FUZZ_SECONDS. New inputs it finds go to the build directory (first
# folder); Fuzz/Corpus only holds committed seed files, which are read but never modified.
option(DX_FUZZ "Build libFuzzer targets" OFF)
set(DX_FUZZ_SECONDS 10 CACHE STRING "Seconds per fuzz target")
if(DX_FUZZ)
    add_executable(fuzz_payload Fuzz/fuzz_payload.c)
    target_link_libraries(fuzz_payload PRIVATE duplex_static)
    target_compile_options(fuzz_payload PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(fuzz_payload PRIVATE -fsanitize=fuzzer,address,undefined)
    add_test(NAME fuzz_payload
             COMMAND fuzz_payload -max_total_time=${DX_FUZZ_SECONDS} -timeout=5
                     ${CMAKE_CURRENT_BINARY_DIR}/fuzz_corpus ${CMAKE_CURRENT_SOURCE_DIR}/Fuzz/Corpus)
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/fuzz_corpus)
    set_tests_properties(fuzz_payload PROPERTIES LABELS Fuzz)
endif()

# Generated/ must match Registry/protocol.toml. Needs Python 3.11+ (tomllib); skipped if absent.
find_program(DX_PYTHON NAMES python3.14 python3.13 python3.12 python3.11)
if(DX_PYTHON)
    add_test(NAME registry_up_to_date
             COMMAND ${DX_PYTHON} ${CMAKE_SOURCE_DIR}/Tools/Codegen/gen_registry.py --check)
    set_tests_properties(registry_up_to_date PROPERTIES LABELS Registry)
else()
    message(WARNING "Python 3.11+ not found: registry_up_to_date test is not registered")
endif()

# One test per generated language: its tables must equal Generated/registry.txt.
# C always runs; the others run only if their toolchain is installed.
set(DX_GOLDEN ${CMAKE_SOURCE_DIR}/Generated/registry.txt)
set(DX_GEN_TESTS ${CMAKE_SOURCE_DIR}/Tests/Generated)

add_executable(test_generated_c Generated/C/test_generated.c)
target_link_libraries(test_generated_c PRIVATE duplex_static)
dx_enable_warnings(test_generated_c)
add_test(NAME generated_c COMMAND test_generated_c ${DX_GOLDEN})

find_program(DX_CXX NAMES c++ clang++ g++)
if(DX_CXX)
    add_test(NAME generated_cpp COMMAND sh -c
        "${DX_CXX} -std=c++17 -Wall -Wextra -Werror -I${CMAKE_SOURCE_DIR}/Generated/Cpp ${DX_GEN_TESTS}/Cpp/test_generated.cpp -o test_generated_cpp && ./test_generated_cpp ${DX_GOLDEN}")
endif()
if(DX_PYTHON)
    add_test(NAME generated_python COMMAND ${DX_PYTHON} ${DX_GEN_TESTS}/Python/test_generated.py ${DX_GOLDEN})
endif()
find_program(DX_RUSTC rustc)
if(DX_RUSTC)
    add_test(NAME generated_rust COMMAND sh -c
        "${DX_RUSTC} --edition 2021 -D warnings ${DX_GEN_TESTS}/Rust/test_generated.rs -o test_generated_rust && ./test_generated_rust ${DX_GOLDEN}")
endif()
find_program(DX_NODE node)
if(DX_NODE)
    add_test(NAME generated_typescript COMMAND ${DX_NODE} ${DX_GEN_TESTS}/TypeScript/test_generated.ts ${DX_GOLDEN})
endif()
set_tests_properties(generated_c PROPERTIES LABELS Generated)
foreach(lang cpp python rust typescript)
    if(TEST generated_${lang})
        set_tests_properties(generated_${lang} PROPERTIES LABELS Generated)
    endif()
endforeach()
