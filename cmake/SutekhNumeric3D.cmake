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
    # host_registry.c registers space3d.query: every library built from SU_AGENT_SOURCES links the operator.
    foreach(agent_lib su_agent su_agent_hooks su_agent_test)
        if(TARGET ${agent_lib})
            target_link_libraries(${agent_lib} PUBLIC su_space3d)
        endif()
    endforeach()
    # 6dof_racer_microcosm: the engine's schedule executor (su_agent) running two IR 0.3 systems and space3d.query.
    if(TARGET su_agent)
        add_executable(racer_microcosm "${CMAKE_CURRENT_SOURCE_DIR}/bench/space3d/racer_microcosm.c")
        target_link_libraries(racer_microcosm PRIVATE su_agent su_space3d)
        sutekh_apply_warnings(racer_microcosm)
        if(Python3_Interpreter_FOUND)
            add_test(NAME space3d_racer_replay COMMAND "${Python3_EXECUTABLE}" -I
                     "${CMAKE_CURRENT_SOURCE_DIR}/bench/space3d/run_bench.py" --driver $<TARGET_FILE:racer_microcosm> --check)
            set_tests_properties(space3d_racer_replay PROPERTIES TIMEOUT 300)
            add_test(NAME space3d_racer_generated COMMAND "${Python3_EXECUTABLE}" -I
                     "${CMAKE_CURRENT_SOURCE_DIR}/bench/space3d/gen_racer.py" --check)
        endif()
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

# space3d (docs/SPACE3D.md): narrow phase, broadphase and the space3d.query host operator.
add_library(su_space3d STATIC profiles/space3d/native/space3d_geom.c profiles/space3d/native/space3d_ops.c)
target_include_directories(su_space3d PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/profiles/space3d/native")
target_link_libraries(su_space3d PUBLIC su_world su_ir)
sutekh_apply_warnings(su_space3d)
add_executable(test_space3d_geom tests/space3d/test_space3d_geom.c)
target_link_libraries(test_space3d_geom PRIVATE su_space3d)
if(UNIX)
    target_link_libraries(test_space3d_geom PRIVATE m) # the double-precision oracle of the test only
endif()
sutekh_apply_warnings(test_space3d_geom)
add_test(NAME space3d_geom COMMAND test_space3d_geom)
# The operator on a real world, its declaration and host record (host_decl.c compiled in: no control plane).
add_executable(test_space3d_ops tests/space3d/test_space3d_ops.c src/runtime/rts/host_decl.c)
target_link_libraries(test_space3d_ops PRIVATE su_space3d su_ents)
sutekh_apply_warnings(test_space3d_ops)
add_test(NAME space3d_ops COMMAND test_space3d_ops WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
