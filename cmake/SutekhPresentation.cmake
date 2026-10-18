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

# ---- presentation (M5 first slice): portable helpers always, the Vulkan backend when headers exist ----
add_library(su_render_util STATIC src/render/su_render_util.c)
target_include_directories(su_render_util PUBLIC "${SUTEKH_CONTRACT_INCLUDE}" PRIVATE src)
if(NOT WIN32)
    target_link_libraries(su_render_util PUBLIC m)
endif()
sutekh_apply_warnings(su_render_util)
add_executable(test_render_util tests/render/test_render_util.c)
target_link_libraries(test_render_util PRIVATE su_render_util)
sutekh_apply_warnings(test_render_util)
add_test(NAME render_util COMMAND test_render_util WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")
# The embedded SPIR-V (src/render/su_render_shaders.h) must match the GLSL sources (hash check; no compiler needed).
add_test(NAME render_shaders_fresh COMMAND ${SUTEKH_PYTHON} -I "${CMAKE_CURRENT_SOURCE_DIR}/tools/compile_shaders.py" --check)

# Vulkan headers: the pinned bootstrap checkout (tools/bootstrap_vulkan_headers.py), else an installed SDK. The
# loader is opened at run time, so nothing links against a Vulkan library. SUTEKH_RENDER=OFF skips the backend.
set(SUTEKH_RENDER "AUTO" CACHE STRING "Build the Vulkan renderer: AUTO (when headers are found), ON, OFF")
set(SUTEKH_VULKAN_INCLUDE "")
foreach(cand "${CMAKE_CURRENT_SOURCE_DIR}/third_party/vulkan-headers/include" "$ENV{VULKAN_SDK}/Include" "$ENV{VULKAN_SDK}/include")
    if(SUTEKH_VULKAN_INCLUDE STREQUAL "" AND EXISTS "${cand}/vulkan/vulkan_core.h")
        set(SUTEKH_VULKAN_INCLUDE "${cand}")
    endif()
endforeach()
if(SUTEKH_RENDER STREQUAL "ON" AND SUTEKH_VULKAN_INCLUDE STREQUAL "")
    message(FATAL_ERROR "SUTEKH_RENDER=ON but no Vulkan headers: run tools/bootstrap_vulkan_headers.py or set VULKAN_SDK")
endif()
if(NOT SUTEKH_RENDER STREQUAL "OFF" AND NOT SUTEKH_VULKAN_INCLUDE STREQUAL "")
    message(STATUS "Sutekh renderer: Vulkan headers ${SUTEKH_VULKAN_INCLUDE}")
    add_library(su_render STATIC src/render/su_render_vk.c)
    target_include_directories(su_render PUBLIC "${SUTEKH_CONTRACT_INCLUDE}" PRIVATE src "${SUTEKH_VULKAN_INCLUDE}")
    target_link_libraries(su_render PUBLIC su_render_util ${CMAKE_DL_LIBS})
    sutekh_apply_warnings(su_render)
    # Offscreen rendering with readback, checked pixel by pixel; skipped (77) without a usable Vulkan device.
    add_executable(test_render_offscreen tests/render/test_render_offscreen.c)
    target_link_libraries(test_render_offscreen PRIVATE su_render)
    sutekh_apply_warnings(test_render_offscreen)
    add_test(NAME render_offscreen COMMAND test_render_offscreen "${CMAKE_BINARY_DIR}/render_offscreen.ppm")
    set_tests_properties(render_offscreen PROPERTIES SKIP_RETURN_CODE 77)
    if(WIN32)
        add_executable(space_demo examples/presentation/space_demo.c)
        target_link_libraries(space_demo PRIVATE su_render su_input su_host user32)
        sutekh_apply_warnings(space_demo)
    endif()
else()
    message(STATUS "Sutekh renderer: skipped (no Vulkan headers; run tools/bootstrap_vulkan_headers.py)")
endif()
