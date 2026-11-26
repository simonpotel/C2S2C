# Presets asan/tsan set C2S2C_SANITIZE. AddressSanitizer+UBSan and ThreadSanitizer cannot be
# combined in one binary, hence two separate presets.
set(C2S2C_SANITIZE "" CACHE STRING "Sanitizers: address;undefined or thread")
if(C2S2C_SANITIZE AND NOT MSVC)
    string(REPLACE ";" "," _san "${C2S2C_SANITIZE}")
    # -fno-sanitize-recover makes any finding abort the test, so it cannot be missed in a log.
    add_compile_options(-fsanitize=${_san} -fno-omit-frame-pointer -fno-sanitize-recover=all)
    add_link_options(-fsanitize=${_san})
endif()
