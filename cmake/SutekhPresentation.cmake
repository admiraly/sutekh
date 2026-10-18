# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
# presentation-input stream (planning/amendments/2026-10-18-downstream-host-work.md, extension): native input
# (include/sutekh/input.h) and the first presentation slice. Included from CMakeLists.txt after the host block.

# ---- native input: portable mapping, plus Raw Input / XInput on Windows ----
add_library(su_input STATIC src/input/su_input_map.c src/input/su_input_json.c)
target_include_directories(su_input PUBLIC "${SUTEKH_CONTRACT_INCLUDE}" PRIVATE src)
target_link_libraries(su_input PRIVATE su_ir)
if(WIN32)
    target_sources(su_input PRIVATE src/platform/input_win32.c)
    target_link_libraries(su_input PRIVATE su_platform user32)
endif()
sutekh_apply_warnings(su_input)

add_executable(test_input tests/input/test_input.c)
target_link_libraries(test_input PRIVATE su_input su_host)
sutekh_apply_warnings(test_input)
add_test(NAME input_map COMMAND test_input WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
if(WIN32)
    add_executable(test_input_win32 tests/input/test_input_win32.c)
    target_link_libraries(test_input_win32 PRIVATE su_input user32)
    sutekh_apply_warnings(test_input_win32)
    add_test(NAME input_win32 COMMAND test_input_win32)
    # Manual smoke tool (not a test): reads real devices, or injects input with SendInput (--inject).
    add_executable(input_probe examples/input/input_probe.c)
    target_link_libraries(input_probe PRIVATE su_input user32)
    sutekh_apply_warnings(input_probe)
endif()
