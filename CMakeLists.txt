cmake_minimum_required(VERSION 3.20)

project(sutekh
    VERSION 0.1.0
    DESCRIPTION "Sutekh M0 headless engine skeleton"
    LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

include(cmake/SutekhWarnings.cmake)

# Frozen contract headers (single owner: M0-00). Consumed read-only.
set(SUTEKH_CONTRACT_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(su_platform STATIC src/platform/su_platform.c)
target_include_directories(su_platform PUBLIC src/platform)
sutekh_apply_warnings(su_platform)

add_executable(sutekh src/platform/sutekh_main.c)
target_include_directories(sutekh PRIVATE "${SUTEKH_CONTRACT_INCLUDE}" src)
target_link_libraries(sutekh PRIVATE su_platform su_agent)
target_compile_definitions(sutekh PRIVATE
    SUTEKH_VERSION_STRING="${PROJECT_VERSION}"
    SUTEKH_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
    SUTEKH_C_COMPILER_ID="${CMAKE_C_COMPILER_ID}"
    SUTEKH_C_COMPILER_VERSION="${CMAKE_C_COMPILER_VERSION}")
sutekh_apply_warnings(sutekh)

enable_testing()

add_library(su_world STATIC src/world/su_world.c)
target_include_directories(su_world PUBLIC src "${SUTEKH_CONTRACT_INCLUDE}")
sutekh_apply_warnings(su_world)

add_executable(test_world tests/world/test_world.c)
target_link_libraries(test_world PRIVATE su_world)
sutekh_apply_warnings(test_world)
add_test(NAME world COMMAND test_world)

add_library(su_ir STATIC src/ir/su_sha256.c src/ir/su_json.c src/ir/su_lir.c)
target_include_directories(su_ir PUBLIC src "${SUTEKH_CONTRACT_INCLUDE}")
sutekh_apply_warnings(su_ir)

add_executable(test_ir tests/ir/test_ir.c)
target_link_libraries(test_ir PRIVATE su_ir)
sutekh_apply_warnings(test_ir)
add_test(NAME ir COMMAND test_ir WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

add_library(su_vm STATIC src/vm/su_vm.c)
target_include_directories(su_vm PUBLIC src "${SUTEKH_CONTRACT_INCLUDE}")
target_link_libraries(su_vm PUBLIC su_world su_ir)
sutekh_apply_warnings(su_vm)

add_executable(test_vm tests/vm/test_vm.c)
target_link_libraries(test_vm PRIVATE su_vm)
sutekh_apply_warnings(test_vm)
add_test(NAME vm COMMAND test_vm WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

file(GLOB SU_AGENT_SOURCES CONFIGURE_DEPENDS src/agent/*.c)
list(APPEND SU_AGENT_SOURCES src/runtime/worker.c src/runtime/registry.c src/runtime/schedule.c src/runtime/reload.c)
add_library(su_agent STATIC ${SU_AGENT_SOURCES})
target_include_directories(su_agent PUBLIC src "${SUTEKH_CONTRACT_INCLUDE}")
target_link_libraries(su_agent PUBLIC su_vm su_world su_ir su_platform)
target_compile_definitions(su_agent PRIVATE _POSIX_C_SOURCE=200809L)
sutekh_apply_warnings(su_agent)

add_executable(worker_stub tests/control/worker_stub.c)
target_link_libraries(worker_stub PRIVATE su_agent)
target_compile_definitions(worker_stub PRIVATE _POSIX_C_SOURCE=200809L)
sutekh_apply_warnings(worker_stub)

add_executable(test_control tests/control/test_control.c)
target_link_libraries(test_control PRIVATE su_agent)
target_compile_definitions(test_control PRIVATE _POSIX_C_SOURCE=200809L)
sutekh_apply_warnings(test_control)
add_test(NAME control COMMAND test_control $<TARGET_FILE:worker_stub> WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

add_library(su_snapshot STATIC src/world/snapshot.c src/runtime/checkpoint.c)
target_include_directories(su_snapshot PUBLIC src "${SUTEKH_CONTRACT_INCLUDE}")
target_link_libraries(su_snapshot PUBLIC su_world su_ir su_platform)
sutekh_apply_warnings(su_snapshot)

add_executable(test_snapshot tests/snapshot/test_snapshot.c)
target_link_libraries(test_snapshot PRIVATE su_snapshot)
sutekh_apply_warnings(test_snapshot)
add_test(NAME snapshot COMMAND test_snapshot WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(test_reload tests/reload/test_reload.c)
target_link_libraries(test_reload PRIVATE su_agent)
target_compile_definitions(test_reload PRIVATE _POSIX_C_SOURCE=200809L)
sutekh_apply_warnings(test_reload)
add_test(NAME reload COMMAND test_reload $<TARGET_FILE:worker_stub> "${CMAKE_CURRENT_SOURCE_DIR}" WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
