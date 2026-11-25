# Line and branch coverage with clang (see Tools/Scripts/coverage.sh). Off by default.
option(DX_COVERAGE "Instrument the code for llvm-cov" OFF)
if(DX_COVERAGE)
    add_compile_options(-fprofile-instr-generate -fcoverage-mapping)
    add_link_options(-fprofile-instr-generate)
endif()
