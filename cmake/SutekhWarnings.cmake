# Strict C17 warning policy shared by all native targets.
function(sutekh_apply_warnings target)
    target_compile_options(${target} PRIVATE
        $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Werror -pedantic>
        $<$<C_COMPILER_ID:MSVC>:/W4 /WX>)
endfunction()
