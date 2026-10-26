# Strict C17 warning policy shared by all native targets.
# Warnings are errors by default when Sutekh is the top-level project. SUTEKH_WERROR=OFF keeps the warnings but stops
# them failing the build, for a compiler newer than the ones CI checks (its new diagnostics should still be reported
# and fixed). A project that includes Sutekh with add_subdirectory gets OFF by default: a newer compiler there must not
# break the consumer's build on engine code it cannot edit (set it ON to hold the engine to the policy).
option(SUTEKH_WERROR "Treat compiler warnings as errors" ${PROJECT_IS_TOP_LEVEL})

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
