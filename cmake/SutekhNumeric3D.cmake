# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
# numeric-3d stream: IR 0.3 (numeric profile fixed_q32_v1, docs/NUMERIC_FIXED_Q32.md) and space3d.
# Included from CMakeLists.txt after the su_ir and su_vm targets exist.
set(SUTEKH_NUMERIC3D_IR_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/ir/su_num.c" "${CMAKE_CURRENT_SOURCE_DIR}/src/ir/su_ops03.c")
target_sources(su_ir PRIVATE ${SUTEKH_NUMERIC3D_IR_SOURCES})
# Any target that compiles its own copy of the su_ir sources (the Windows allocation-seam build of
# test_lir_oom defines su_ir_oom_objs later in CMakeLists.txt) needs the IR 0.3 sources too.
function(_sutekh_numeric3d_late)
    if(TARGET su_ir_oom_objs)
        target_sources(su_ir_oom_objs PRIVATE ${SUTEKH_NUMERIC3D_IR_SOURCES})
    endif()
endfunction()
cmake_language(DEFER CALL _sutekh_numeric3d_late)
find_package(Python3 COMPONENTS Interpreter)
add_executable(test_num tests/num/test_num.c)
target_link_libraries(test_num PRIVATE su_ir)
sutekh_apply_warnings(test_num)
add_test(NAME num COMMAND test_num)
if(Python3_Interpreter_FOUND)
    # Independent big-integer model of the profile against the C reference (random vectors, fixed seeds).
    add_test(NAME num_model COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_CURRENT_SOURCE_DIR}/tests/num/q32_model.py"
             --binary $<TARGET_FILE:test_num> --count 20000 --seeds 1 2 3)
endif()
add_executable(test_ir03 tests/ir03/test_ir03.c)
target_link_libraries(test_ir03 PRIVATE su_vm)
sutekh_apply_warnings(test_ir03)
add_test(NAME ir03 COMMAND test_ir03 WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
add_executable(ir03_driver tests/ir03/ir03_driver.c)
target_link_libraries(ir03_driver PRIVATE su_vm)
sutekh_apply_warnings(ir03_driver)
if(Python3_Interpreter_FOUND)
    # Seeded random IR 0.3 programs: real verifier + VM against an independent interpreter.
    add_test(NAME ir03_diff COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_CURRENT_SOURCE_DIR}/tests/ir03/ir03_diff.py"
             --driver $<TARGET_FILE:ir03_driver> --work "${CMAKE_BINARY_DIR}/ir03_diff_scratch"
             --programs 300 --rows 48 --seed 1)
endif()
