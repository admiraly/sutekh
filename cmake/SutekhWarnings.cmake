# Strict C17 warning policy shared by all native targets.
# Warnings are errors by default. SUTEKH_WERROR=OFF keeps the warnings but stops them failing the build, for a
# compiler newer than the ones CI checks (its new diagnostics should still be reported and fixed).
option(SUTEKH_WERROR "Treat compiler warnings as errors" ON)

function(sutekh_apply_warnings target)
    target_compile_options(${target} PRIVATE
        $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -pedantic>
        $<$<C_COMPILER_ID:MSVC>:/W4>)
    if(SUTEKH_WERROR)
        target_compile_options(${target} PRIVATE
            $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-Werror>
            $<$<C_COMPILER_ID:MSVC>:/WX>)
    endif()
endfunction()
