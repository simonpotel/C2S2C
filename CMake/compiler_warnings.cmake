# Strict, warnings-as-errors: a warning in a network parser is usually a latent bug
# (sign/size conversions, shadowing, bad casts), so it must break the build.
function(dx_enable_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror -Wconversion
            -Wshadow -Wcast-align $<$<COMPILE_LANGUAGE:C>:-Wstrict-prototypes>)
    endif()
endfunction()
