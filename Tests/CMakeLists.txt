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

# Reference vectors: one ctest entry per Vectors/header_*.bin, so a failure names the exact file.
add_executable(test_vectors Vectors/test_vectors.c)
target_link_libraries(test_vectors PRIVATE duplex_static)
dx_enable_warnings(test_vectors)
file(GLOB DX_VECTORS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/Vectors/header_*.bin)
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
