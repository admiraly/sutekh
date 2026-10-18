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
        # The demo's world, scripted input and rollback drills, headless and offscreen: both runs must reach the
        # same pinned digest (480 ticks of IR 0.3 at a fixed 60 Hz frame time). Exit 3 (no Vulkan) skips the
        # offscreen run; the headless run needs no GPU.
        set(SUTEKH_DEMO_DIGEST fa02b76e1269693a5f4daa5c7139818c0fff106bd5f57c656556e1336aa1defd)
        add_test(NAME demo_headless_rollback COMMAND space_demo --headless --script --frames 240 --rollback-check
                 --expect-digest ${SUTEKH_DEMO_DIGEST} WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
        add_test(NAME demo_offscreen_rollback COMMAND space_demo --offscreen "${CMAKE_BINARY_DIR}/space_demo.ppm"
                 --frames 240 --rollback-check --expect-digest ${SUTEKH_DEMO_DIGEST}
                 WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
        set_tests_properties(demo_offscreen_rollback PROPERTIES SKIP_RETURN_CODE 3)
    endif()
else()
    message(STATUS "Sutekh renderer: skipped (no Vulkan headers; run tools/bootstrap_vulkan_headers.py)")
endif()

# ---- assets (M5): cooked packs (always built) and the glTF importer (needs tools/bootstrap_asset_deps.py) ----
add_library(su_asset STATIC src/asset/su_asset_pak.c)
target_include_directories(su_asset PUBLIC "${SUTEKH_CONTRACT_INCLUDE}" PRIVATE src)
target_link_libraries(su_asset PUBLIC su_platform PRIVATE su_ir)
if(NOT WIN32)
    target_link_libraries(su_asset PUBLIC m)
endif()
sutekh_apply_warnings(su_asset)

set(SUTEKH_ASSET_IMPORT "AUTO" CACHE STRING "Build the glTF importer: AUTO (when the pinned cgltf and stb are present), ON, OFF")
set(SUTEKH_ASSET_DEPS_FOUND OFF)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/cgltf/cgltf.h" AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb/stb_image.h")
    set(SUTEKH_ASSET_DEPS_FOUND ON)
endif()
if(SUTEKH_ASSET_IMPORT STREQUAL "ON" AND NOT SUTEKH_ASSET_DEPS_FOUND)
    message(FATAL_ERROR "SUTEKH_ASSET_IMPORT=ON but cgltf/stb are missing: run tools/bootstrap_asset_deps.py")
endif()
if(SUTEKH_ASSET_DEPS_FOUND AND NOT SUTEKH_ASSET_IMPORT STREQUAL "OFF")
    message(STATUS "Sutekh glTF importer: cgltf and stb_image from third_party")
    # Third-party implementation unit: compiled without the project's warning flags.
    add_library(su_asset_thirdparty OBJECT src/asset/su_asset_thirdparty.c)
    target_include_directories(su_asset_thirdparty PRIVATE third_party/cgltf third_party/stb)
    target_compile_options(su_asset_thirdparty PRIVATE $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-w> $<$<C_COMPILER_ID:MSVC>:/w>)
    set_target_properties(su_asset_thirdparty PROPERTIES POSITION_INDEPENDENT_CODE ON)
    add_library(su_asset_import STATIC src/asset/su_asset_gltf.c $<TARGET_OBJECTS:su_asset_thirdparty>)
    target_include_directories(su_asset_import PUBLIC "${SUTEKH_CONTRACT_INCLUDE}" PRIVATE src third_party/cgltf third_party/stb)
    target_link_libraries(su_asset_import PUBLIC su_asset)
    sutekh_apply_warnings(su_asset_import)
    set(SUTEKH_HAVE_ASSET_IMPORT ON)
else()
    message(STATUS "Sutekh glTF importer: skipped (run tools/bootstrap_asset_deps.py)")
    add_library(su_asset_import STATIC src/asset/su_asset_noimport.c)
    target_include_directories(su_asset_import PUBLIC "${SUTEKH_CONTRACT_INCLUDE}")
    target_link_libraries(su_asset_import PUBLIC su_asset)
    sutekh_apply_warnings(su_asset_import)
    set(SUTEKH_HAVE_ASSET_IMPORT OFF)
endif()

add_executable(test_asset_pak tests/asset/test_asset_pak.c)
target_link_libraries(test_asset_pak PRIVATE su_asset su_ir)
target_include_directories(test_asset_pak PRIVATE src)
sutekh_apply_warnings(test_asset_pak)
add_test(NAME asset_pak COMMAND test_asset_pak WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")
# Cook tool: glTF -> pack, and pack inspection (examples/asset/asset_cook.c).
add_executable(asset_cook examples/asset/asset_cook.c)
target_link_libraries(asset_cook PRIVATE su_asset_import)
sutekh_apply_warnings(asset_cook)
if(SUTEKH_HAVE_ASSET_IMPORT)
    # Procedurally generated glTF/GLB/PNG/KTX2 files written by the test itself; no downloaded models.
    add_executable(test_asset_gltf tests/asset/test_asset_gltf.c)
    target_link_libraries(test_asset_gltf PRIVATE su_asset_import)
    sutekh_apply_warnings(test_asset_gltf)
    add_test(NAME asset_gltf COMMAND test_asset_gltf "${CMAKE_BINARY_DIR}/asset_gltf_work")
endif()
