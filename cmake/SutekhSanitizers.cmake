# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
#
# Sanitizer builds (issue 148). SUTEKH_SANITIZE is a -fsanitize= list for GCC and Clang, for example
# "address,undefined"; empty (the default) leaves the build unchanged. Every target, C and C++, is compiled and
# linked with it, with frame pointers and with no recovery, so a UBSan report fails the process like an ASan one.
# A build that passes -fsanitize=address in CMAKE_C_FLAGS by hand gets the same test environment.
#
# Under AddressSanitizer the test environment (sutekh_sanitizer_test_environment, deferred until every test is
# registered) is adjusted for two instrumentation limits:
#   SUTEKH_WORKER_AS_BYTES=0   ASan reserves about 15 TiB of virtual shadow memory at start-up, so a worker or
#                              disposable child under the default 4 GiB RLIMIT_AS dies before main. 0 removes the
#                              worker address-space limit (sutekh_main.c); containment's RLIMIT_AS
#                              check then expects unlimited (its other limits are unchanged). The 4 GiB
#                              limit stays covered by every non-sanitizer job.
#   SUTEKH_WORKER_SANDBOX=0    LeakSanitizer forks and ptrace-attaches at exit; the worker self-sandbox denies
#                              fork, so leak checking in a worker aborts with "LeakSanitizer has encountered a
#                              fatal error" and the worker exits nonzero. The override is honoured only in
#                              sanitizer builds (su_sandbox.c). It is not set for containment, which keeps the
#                              self-sandbox on and checks that it reports "on".
# The determinism matrix runs a reduced workload in these builds (cmake/SutekhDeterminism.cmake, SUTEKH_DET_MAX_TICKS).
# Disposable children under the containment seccomp filter (ptrace denied) still cannot run LeakSanitizer; that is
# a leak-coverage gap, not a failure, and the CI job counts it.

set(SUTEKH_SANITIZE "" CACHE STRING
    "Sanitizers for every target, as a -fsanitize= list (e.g. address,undefined); empty = none")

if(SUTEKH_SANITIZE)
    if(MSVC)
        message(FATAL_ERROR "SUTEKH_SANITIZE supports GCC and Clang only")
    endif()
    add_compile_options(-fsanitize=${SUTEKH_SANITIZE} -fno-sanitize-recover=all -fno-omit-frame-pointer)
    # Clang's -fsanitize=function (part of undefined) checks a type signature stored just before the callee. AsmJit
    # code has none, so every call into a JIT kernel reads the bytes before it (a SEGV when the kernel starts a
    # page). With the native tier the check is turned off for every target; the default build keeps it.
    if(SUTEKH_NATIVE AND CMAKE_C_COMPILER_ID MATCHES "Clang"
       AND SUTEKH_SANITIZE MATCHES "(^|,)(undefined|function)(,|$)")
        add_compile_options(-fno-sanitize=function)
    endif()
    add_link_options(-fsanitize=${SUTEKH_SANITIZE})
    message(STATUS "Sutekh sanitizers: ${SUTEKH_SANITIZE}")
endif()

if(SUTEKH_SANITIZE MATCHES "(^|,)address(,|$)" OR CMAKE_C_FLAGS MATCHES "-fsanitize=[^ ]*address")
    set(SUTEKH_ASAN_BUILD ON)
else()
    set(SUTEKH_ASAN_BUILD OFF)
endif()

function(sutekh_sanitizer_test_environment)
    if(NOT SUTEKH_ASAN_BUILD)
        return()
    endif()
    get_property(_su_tests DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" PROPERTY TESTS)
    foreach(_su_test IN LISTS _su_tests)
        set_property(TEST ${_su_test} APPEND PROPERTY ENVIRONMENT "SUTEKH_WORKER_AS_BYTES=0")
        if(NOT _su_test STREQUAL "containment")
            set_property(TEST ${_su_test} APPEND PROPERTY ENVIRONMENT "SUTEKH_WORKER_SANDBOX=0")
        endif()
    endforeach()
endfunction()
