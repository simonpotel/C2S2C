# Line and branch coverage with clang (see Tools/Scripts/coverage.sh). Off by default.
option(C2S2C_COVERAGE "Instrument the code for llvm-cov" OFF)
if(C2S2C_COVERAGE)
    add_compile_options(-fprofile-instr-generate -fcoverage-mapping)
    add_link_options(-fprofile-instr-generate)
endif()
