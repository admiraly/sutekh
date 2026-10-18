# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
#
# Platform settings shared by every target. The POSIX build is unchanged by this file.
#
# SUTEKH_PYTHON is the interpreter that runs the Python tests. It stays "python3" on POSIX (as before);
# on Windows, where python3 is usually an App Execution Alias stub, it is the interpreter CMake finds
# (the project .venv when tools/env-windows.ps1 put it first on PATH).
if(WIN32)
    # The UCRT marks the ISO C and POSIX-named functions (fopen, strcpy, open, unlink) as deprecated.
    # They are the functions the code uses on every platform; the warning is noise here, not a defect.
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX)
    # Static release CRT in every configuration: binaries need no CRT DLLs, the debug CRT's assertion and
    # abort() dialogs never block an unattended test run, and the host DLL carries its own allocator
    # (memory it returns is released through its own functions).
    if(NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
    endif()
    # MSVC's cl implements C11 <stdatomic.h> (used by su_cmdq, su_jobs and their tests) behind this switch
    # (VS 2022 17.5 and later); clang and clang-cl support it as is.
    add_compile_options($<$<AND:$<COMPILE_LANGUAGE:C>,$<C_COMPILER_ID:MSVC>>:/experimental:c11atomics>)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(SUTEKH_PYTHON "${Python3_EXECUTABLE}")
else()
    set(SUTEKH_PYTHON python3)
endif()

# Compiles every source of target with header force-included first (-include for GCC and Clang, /FI for MSVC's cl,
# which ignores -include with only a command-line warning). Used by the allocation and accounting seams.
function(sutekh_force_include target header)
    target_compile_options(${target} PRIVATE "$<$<C_COMPILER_ID:MSVC>:/FI${header}>"
                           "$<$<NOT:$<C_COMPILER_ID:MSVC>>:SHELL:-include \"${header}\">")
endfunction()

# Registers a test that cannot run on this platform. It runs, prints the reason and is reported by ctest as
# skipped (never as passed), so the default suite keeps an explicit record of what was not exercised.
function(sutekh_unsupported_test name reason)
    add_test(NAME ${name} COMMAND "${CMAKE_COMMAND}" -E echo "UNSUPPORTED on ${CMAKE_SYSTEM_NAME}: ${reason}")
    set_tests_properties(${name} PROPERTIES SKIP_REGULAR_EXPRESSION "UNSUPPORTED on ")
endfunction()
