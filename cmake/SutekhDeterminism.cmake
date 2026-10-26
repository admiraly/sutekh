# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
# Determinism matrix (docs/DETERMINISM_MATRIX.md, issue #141): the matrix workloads through the public host API on
# every tier this build has, each compared with the committed cross-build expectation
# tests/determinism/expected_digests.txt. Included after cmake/SutekhHostOps.cmake (su_host, sutekh_host exist).
set(SU_DET_DIR "${CMAKE_CURRENT_SOURCE_DIR}/tests/determinism")
set(SU_DET_FIXTURES "${CMAKE_BINARY_DIR}/det_fixtures")
set(SU_DET_EXPECT "${SU_DET_DIR}/expected_digests.txt")
foreach(variant "" "_dll")
    add_executable(det_matrix${variant} "${SU_DET_DIR}/det_matrix.c" "${CMAKE_CURRENT_SOURCE_DIR}/src/ir/su_sha256.c")
    target_include_directories(det_matrix${variant} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    if(variant STREQUAL "_dll")
        target_link_libraries(det_matrix${variant} PRIVATE sutekh_host)
    else()
        target_link_libraries(det_matrix${variant} PRIVATE su_host)
    endif()
    sutekh_apply_warnings(det_matrix${variant})
endforeach()

# Committed capsules equal their generator's output.
add_test(NAME determinism_generated COMMAND ${SUTEKH_PYTHON} -I "${SU_DET_DIR}/gen_workloads.py" --check)
if(TARGET su_agent)
    # Scene fixtures: the locked RTS scenes (regenerated and checked against bench/rts/scenes.lock.json) and the
    # 16-racer scene of racer_microcosm (defined later, deferred; add_test resolves the name at generation time).
    add_test(NAME determinism_fixtures_rts COMMAND ${SUTEKH_PYTHON} -I "${SU_DET_DIR}/gen_workloads.py" fixtures
             --out "${SU_DET_FIXTURES}")
    add_test(NAME determinism_fixtures_racer COMMAND racer_microcosm --root "${CMAKE_CURRENT_SOURCE_DIR}" --n 16
             --ticks 1 --warmup 0 --dump-fixture "${SU_DET_FIXTURES}/racer16.json")
    set_tests_properties(determinism_fixtures_rts determinism_fixtures_racer PROPERTIES FIXTURES_SETUP det_fixtures)
    set_tests_properties(determinism_fixtures_racer PROPERTIES DEPENDS determinism_fixtures_rts)

    set(SU_DET_TIERS reference planned)
    if(SUTEKH_NATIVE)
        list(APPEND SU_DET_TIERS native)
    endif()
    foreach(tier ${SU_DET_TIERS})
        add_test(NAME determinism_matrix_${tier} COMMAND det_matrix --fixtures "${SU_DET_FIXTURES}" --tier ${tier}
                 --expect "${SU_DET_EXPECT}" --out "${CMAKE_BINARY_DIR}/det_manifest_${tier}.txt"
                 WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
        set_tests_properties(determinism_matrix_${tier} PROPERTIES FIXTURES_REQUIRED det_fixtures TIMEOUT 600)
    endforeach()
    # The same through the DLL, and a recording made by the static library played back through the DLL.
    add_test(NAME determinism_matrix_dll COMMAND det_matrix_dll --fixtures "${SU_DET_FIXTURES}" --tier planned
             --expect "${SU_DET_EXPECT}" --replay-dir "${SU_DET_FIXTURES}" WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME determinism_play_static COMMAND det_matrix --fixtures "${SU_DET_FIXTURES}" --tier reference
             --play-dir "${SU_DET_FIXTURES}" WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    set_tests_properties(determinism_matrix_dll determinism_play_static PROPERTIES FIXTURES_REQUIRED det_fixtures
                         TIMEOUT 600)
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
