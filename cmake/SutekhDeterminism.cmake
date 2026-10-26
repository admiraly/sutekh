# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
# Determinism matrix (docs/DETERMINISM_MATRIX.md, issue #141): the matrix workloads through the public host API on
# every tier this build has, each compared with the committed cross-build expectations
# tests/determinism/expected_digests.txt (state) and tests/determinism/expected_tiers.txt (per-system placement).
# Included after cmake/SutekhHostOps.cmake (su_host, sutekh_host exist).
set(SU_DET_DIR "${CMAKE_CURRENT_SOURCE_DIR}/tests/determinism")
set(SU_DET_FIXTURES "${CMAKE_BINARY_DIR}/det_fixtures")
set(SU_DET_EXPECT "${SU_DET_DIR}/expected_digests.txt")
set(SU_DET_TIERS_EXPECT "${SU_DET_DIR}/expected_tiers.txt")

# CI legs pin their compiler: SUTEKH_DET_COMPILER_LEG names a line of tests/determinism/ci_compilers.txt
# ("<leg> <CMAKE_C_COMPILER_ID> <version prefix>"), and a different compiler stops the configure step. The list is
# updated deliberately when a runner image changes compilers (see the file's header), never to make a leg pass.
if(SUTEKH_DET_COMPILER_LEG)
    file(STRINGS "${SU_DET_DIR}/ci_compilers.txt" _su_det_legs REGEX "^${SUTEKH_DET_COMPILER_LEG} ")
    if(NOT _su_det_legs)
        message(FATAL_ERROR
                "determinism leg ${SUTEKH_DET_COMPILER_LEG} is not listed in tests/determinism/ci_compilers.txt")
    endif()
    list(GET _su_det_legs 0 _su_det_leg)
    string(REPLACE " " ";" _su_det_leg "${_su_det_leg}")
    list(GET _su_det_leg 1 _su_det_id)
    list(GET _su_det_leg 2 _su_det_ver)
    string(FIND "${CMAKE_C_COMPILER_VERSION}" "${_su_det_ver}" _su_det_at)
    if(NOT CMAKE_C_COMPILER_ID STREQUAL _su_det_id OR NOT _su_det_at EQUAL 0)
        message(FATAL_ERROR "determinism leg ${SUTEKH_DET_COMPILER_LEG}: compiler is ${CMAKE_C_COMPILER_ID} "
                            "${CMAKE_C_COMPILER_VERSION}, tests/determinism/ci_compilers.txt expects ${_su_det_id} "
                            "${_su_det_ver}")
    endif()
    message(STATUS "determinism leg ${SUTEKH_DET_COMPILER_LEG}: ${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}")
endif()

foreach(variant "" "_dll")
    add_executable(det_matrix${variant} "${SU_DET_DIR}/det_matrix.c" "${CMAKE_CURRENT_SOURCE_DIR}/src/ir/su_sha256.c")
    target_include_directories(det_matrix${variant} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
                               "${CMAKE_CURRENT_SOURCE_DIR}/tests/space3d")
    if(variant STREQUAL "_dll")
        target_link_libraries(det_matrix${variant} PRIVATE sutekh_host)
    else()
        target_link_libraries(det_matrix${variant} PRIVATE su_host)
    endif()
    sutekh_apply_warnings(det_matrix${variant})
endforeach()

# Committed capsules equal their generator's output.
add_test(NAME determinism_generated COMMAND ${SUTEKH_PYTHON} -I "${SU_DET_DIR}/gen_workloads.py" --check)
# Every operation of the IR 0.3/0.4 table runs in a matrix workload with a result that reaches state.
add_executable(det_op_list "${SU_DET_DIR}/det_op_list.c")
target_link_libraries(det_op_list PRIVATE su_ir)
sutekh_apply_warnings(det_op_list)
add_test(NAME determinism_op_coverage COMMAND ${SUTEKH_PYTHON} -I "${SU_DET_DIR}/check_op_coverage.py"
         $<TARGET_FILE:det_op_list>)
add_test(NAME determinism_op_coverage_selftest COMMAND ${SUTEKH_PYTHON} -I "${SU_DET_DIR}/check_op_coverage.py"
         --self-test)
if(TARGET su_agent)
    # Scene fixtures: the locked RTS scenes (regenerated and checked against bench/rts/scenes.lock.json). The 16-racer
    # scene is committed (tests/determinism/fixtures/racer16.json); racer_microcosm (defined later, deferred;
    # add_test resolves the name at generation time) must still dump the same bytes.
    add_test(NAME determinism_fixtures_rts COMMAND ${SUTEKH_PYTHON} -I "${SU_DET_DIR}/gen_workloads.py" fixtures
             --out "${SU_DET_FIXTURES}")
    set_tests_properties(determinism_fixtures_rts PROPERTIES FIXTURES_SETUP det_fixtures)
    add_test(NAME determinism_fixture_racer_dump COMMAND racer_microcosm --root "${CMAKE_CURRENT_SOURCE_DIR}" --n 16
             --ticks 1 --warmup 0 --dump-fixture "${CMAKE_BINARY_DIR}/det_racer16_dump.json")
    add_test(NAME determinism_fixture_racer_committed COMMAND ${CMAKE_COMMAND} -E compare_files
             "${CMAKE_BINARY_DIR}/det_racer16_dump.json" "${SU_DET_DIR}/fixtures/racer16.json")
    set_tests_properties(determinism_fixture_racer_committed PROPERTIES DEPENDS determinism_fixture_racer_dump)

    set(SU_DET_TIERS reference planned mixed)
    if(SUTEKH_NATIVE)
        list(APPEND SU_DET_TIERS native)
    endif()
    foreach(tier ${SU_DET_TIERS})
        add_test(NAME determinism_matrix_${tier} COMMAND det_matrix --fixtures "${SU_DET_FIXTURES}" --tier ${tier}
                 --expect "${SU_DET_EXPECT}" --expect-tiers "${SU_DET_TIERS_EXPECT}"
                 --out "${CMAKE_BINARY_DIR}/det_manifest_${tier}.txt" WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
        set_tests_properties(determinism_matrix_${tier} PROPERTIES FIXTURES_REQUIRED det_fixtures TIMEOUT 900)
    endforeach()
    # The same through the DLL, and a recording made by the static library played back through the DLL.
    add_test(NAME determinism_matrix_dll COMMAND det_matrix_dll --fixtures "${SU_DET_FIXTURES}" --tier planned
             --expect "${SU_DET_EXPECT}" --expect-tiers "${SU_DET_TIERS_EXPECT}" --replay-dir "${SU_DET_FIXTURES}"
             WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME determinism_play_static COMMAND det_matrix --fixtures "${SU_DET_FIXTURES}" --tier reference
             --play-dir "${SU_DET_FIXTURES}" WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    set_tests_properties(determinism_matrix_dll determinism_play_static PROPERTIES FIXTURES_REQUIRED det_fixtures
                         TIMEOUT 900)
    set_tests_properties(determinism_play_static PROPERTIES DEPENDS determinism_matrix_dll)
endif()

# Regression fixtures of the matrix findings (D1: space3d box sign products on INT64_MIN half extents).
add_executable(test_det_regress "${SU_DET_DIR}/test_det_regress.c")
target_link_libraries(test_det_regress PRIVATE su_space3d)
sutekh_apply_warnings(test_det_regress)
add_test(NAME determinism_regress COMMAND test_det_regress)

# The full six-system RTS world (native operators rts.spatial and rts.scatter) through `sutekh serve` against the
# committed recording of a Linux GCC build: every tick's engine state digest must match.
add_test(NAME determinism_rts_engine COMMAND ${SUTEKH_PYTHON} -I "${SU_DET_DIR}/rts_engine_golden.py" check
         $<TARGET_FILE:sutekh>)
set_tests_properties(determinism_rts_engine PROPERTIES TIMEOUT 300)
