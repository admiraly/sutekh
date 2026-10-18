# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
# Host operators and execution tiers in the in-process host API (include/sutekh/host.h, "host operators",
# "execution tiers"; planning/amendments/2026-10-18-sim-runtime.md). Included right after the host block of
# CMakeLists.txt, when su_host, sutekh_host and su_space3d exist.
foreach(host_lib su_host sutekh_host)
    target_sources(${host_lib} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/host/su_host_operators.c")
endforeach()
target_link_libraries(su_host PUBLIC su_space3d)
target_link_libraries(sutekh_host PRIVATE su_space3d)
set_target_properties(su_space3d PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(SUTEKH_NATIVE)
    # SU_TIER_NATIVE through the host API: the native artifact registry and the AsmJit backend are linked into the
    # host library. SuSysEntry's layout depends on SU_HAVE_NATIVE, so it is PUBLIC on the static library (tests that
    # read the internal world see the same layout) and PRIVATE on the DLL (its users see host.h only).
    foreach(host_lib su_host sutekh_host)
        target_sources(${host_lib} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/native_versions.c")
    endforeach()
    target_compile_definitions(su_host PUBLIC SU_HAVE_NATIVE)
    target_compile_definitions(sutekh_host PRIVATE SU_HAVE_NATIVE)
    target_link_libraries(su_host PUBLIC su_native)
    target_link_libraries(sutekh_host PRIVATE su_native)
    set_target_properties(su_native su_asmjit PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

# The racer microcosm through the host API (static and DLL): host records (exact and schema-relative), digests
# against the pinned in-process run, in-place restore + re-simulation, replay, two worlds on two threads.
# The fixture is dumped by racer_microcosm (the scene generator) before the test runs.
if(TARGET su_agent)
    # racer_microcosm is defined later (deferred, cmake/SutekhNumeric3D.cmake); add_test resolves the name at
    # generation time.
    set(SU_HOST_OPS_FIXTURE "${CMAKE_BINARY_DIR}/host_ops_racer_n8.json")
    add_test(NAME host_ops_fixture COMMAND racer_microcosm --root "${CMAKE_CURRENT_SOURCE_DIR}" --n 8 --ticks 1
             --warmup 0 --dump-fixture "${SU_HOST_OPS_FIXTURE}")
    set_tests_properties(host_ops_fixture PROPERTIES FIXTURES_SETUP host_ops_fixture)
    foreach(variant "" "_dll")
        add_executable(test_host_ops${variant} "${CMAKE_CURRENT_SOURCE_DIR}/tests/host/test_host_ops.c")
        if(variant STREQUAL "_dll")
            # The DLL exports host.h only; the test hashes the snapshot's state section with its own SHA-256 copy.
            target_sources(test_host_ops${variant} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/ir/su_sha256.c")
            target_link_libraries(test_host_ops${variant} PRIVATE sutekh_host)
        else()
            target_link_libraries(test_host_ops${variant} PRIVATE su_host)
        endif()
        target_include_directories(test_host_ops${variant} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
        sutekh_apply_warnings(test_host_ops${variant})
        add_test(NAME host_ops${variant} COMMAND test_host_ops${variant} "${SU_HOST_OPS_FIXTURE}"
                 WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
        set_tests_properties(host_ops${variant} PROPERTIES FIXTURES_REQUIRED host_ops_fixture TIMEOUT 300)
    endforeach()
endif()

# Embedded space3d example: a game schema of its own with space3d.query bound schema-relatively, run through the DLL.
add_executable(arena_host "${CMAKE_CURRENT_SOURCE_DIR}/examples/host_space3d/arena_host.c")
target_link_libraries(arena_host PRIVATE sutekh_host)
sutekh_apply_warnings(arena_host)
add_test(NAME example_arena_host COMMAND arena_host --root "${CMAKE_CURRENT_SOURCE_DIR}" --ticks 900 --rollback-check
         --quiet --expect-digest "3b1f7e1dc74d79f9fd0d86bddd1478e614cd717c8b9184956322b564c3f8eac9")
if(SUTEKH_NATIVE)
    add_test(NAME example_arena_host_native COMMAND arena_host --root "${CMAKE_CURRENT_SOURCE_DIR}" --ticks 900
             --rollback-check --native --quiet --expect-digest "3b1f7e1dc74d79f9fd0d86bddd1478e614cd717c8b9184956322b564c3f8eac9")
endif()
if(Python3_Interpreter_FOUND)
    add_test(NAME example_arena_generated COMMAND "${Python3_EXECUTABLE}" -I
             "${CMAKE_CURRENT_SOURCE_DIR}/examples/host_space3d/gen_arena.py" --check)
endif()
