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
target_link_libraries(su_agent PUBLIC su_vm su_world su_ir su_platform su_snapshot)
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

find_package(Threads REQUIRED)
add_library(su_jobs STATIC src/runtime/jobs/su_jobs.c src/runtime/jobs/su_effects.c)
target_include_directories(su_jobs PUBLIC src "${SUTEKH_CONTRACT_INCLUDE}")
target_compile_definitions(su_jobs PRIVATE _POSIX_C_SOURCE=200809L)
target_link_libraries(su_jobs PUBLIC su_vm su_world su_ir Threads::Threads)
sutekh_apply_warnings(su_jobs)

add_executable(test_scheduler tests/scheduler/test_scheduler.c)
target_link_libraries(test_scheduler PRIVATE su_jobs)
target_compile_definitions(test_scheduler PRIVATE _POSIX_C_SOURCE=200809L)
sutekh_apply_warnings(test_scheduler)
add_test(NAME scheduler COMMAND test_scheduler WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(test_reload tests/reload/test_reload.c)
target_link_libraries(test_reload PRIVATE su_agent)
target_compile_definitions(test_reload PRIVATE _POSIX_C_SOURCE=200809L)
sutekh_apply_warnings(test_reload)
add_test(NAME reload COMMAND test_reload $<TARGET_FILE:worker_stub> "${CMAKE_CURRENT_SOURCE_DIR}" WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(test_snapshot_cp tests/snapshot_cp/test_snapshot_cp.c)
target_link_libraries(test_snapshot_cp PRIVATE su_agent)
target_compile_definitions(test_snapshot_cp PRIVATE _POSIX_C_SOURCE=200809L)
sutekh_apply_warnings(test_snapshot_cp)
add_test(NAME snapshot_cp COMMAND test_snapshot_cp $<TARGET_FILE:worker_stub> "${CMAKE_CURRENT_SOURCE_DIR}"
         WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

# ---- BEGIN M1-01 optional native scalar backend (SUTEKH_NATIVE; default OFF) ----
# Enable with: cmake -DSUTEKH_NATIVE=ON -S . -B <build>   (the default build is unchanged)
option(SUTEKH_NATIVE "Build the M1 AsmJit native scalar backend and its tests" OFF)
if(SUTEKH_NATIVE)
    enable_language(CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)

    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/third_party/asmjit.lock.json" SU_ASMJIT_LOCK_TEXT)
    string(JSON SU_ASMJIT_COMMIT GET "${SU_ASMJIT_LOCK_TEXT}" commit)
    string(LENGTH "${SU_ASMJIT_COMMIT}" SU_ASMJIT_COMMIT_LEN)
    if(NOT SU_ASMJIT_COMMIT MATCHES "^[0-9a-f]+$" OR NOT SU_ASMJIT_COMMIT_LEN EQUAL 40)
        message(FATAL_ERROR "third_party/asmjit.lock.json has no pinned commit; run tools/bootstrap_asmjit.py")
    endif()
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/asmjit/asmjit/core/code_holder.cpp")
        message(FATAL_ERROR "AsmJit is not present; run: python3 tools/bootstrap_asmjit.py")
    endif()

    # Core and x86 translation units only (no ARM, A64, UJIT, and no *_test.cpp).
    file(GLOB SU_ASMJIT_CORE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/asmjit/asmjit/core/*.cpp")
    file(GLOB SU_ASMJIT_X86_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/asmjit/asmjit/x86/*.cpp")
    # axl is AsmJit's support library (arena, vector, hash); core and x86 link against it.
    file(GLOB SU_ASMJIT_AXL_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/asmjit/asmjit/axl/*.cpp")
    set(SU_ASMJIT_SOURCES ${SU_ASMJIT_CORE_SOURCES} ${SU_ASMJIT_X86_SOURCES} ${SU_ASMJIT_AXL_SOURCES})
    list(FILTER SU_ASMJIT_SOURCES EXCLUDE REGEX "/(arm|a64|ujit)[^/]*$|_test\\.cpp$|/arm/|/ujit/")

    add_library(su_asmjit STATIC ${SU_ASMJIT_SOURCES})
    target_include_directories(su_asmjit PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/third_party/asmjit")
    target_compile_definitions(su_asmjit PUBLIC ASMJIT_STATIC ASMJIT_NO_AARCH64 ASMJIT_NO_UJIT)
    set_target_properties(su_asmjit PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    target_compile_options(su_asmjit PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang>:-w>)

    add_library(su_native STATIC backends/native/su_native.c backends/native/su_native_asmjit.cpp)
    target_include_directories(su_native PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" src "${SUTEKH_CONTRACT_INCLUDE}")
    target_link_libraries(su_native PUBLIC su_ir su_asmjit)
    target_compile_definitions(su_native PRIVATE SU_NATIVE_ASMJIT_COMMIT="${SU_ASMJIT_COMMIT}")
    set_target_properties(su_native PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    # Strict C17 set for the C files. The C++ adapter gets -Wall -Wextra -Werror only: the AsmJit
    # headers rely on compiler extensions that -pedantic rejects.
    target_compile_options(su_native PRIVATE
        $<$<AND:$<COMPILE_LANGUAGE:C>,$<C_COMPILER_ID:GNU,Clang,AppleClang>>:-Wall -Wextra -Werror -pedantic>
        $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:GNU,Clang,AppleClang>>:-Wall -Wextra -Werror>)

    add_executable(test_native_scalar tests/native/scalar/test_native_scalar.c)
    target_link_libraries(test_native_scalar PRIVATE su_native su_vm su_world su_ir)
    sutekh_apply_warnings(test_native_scalar)

    set(SU_NATIVE_CORPUS_DIR "${CMAKE_BINARY_DIR}/native_scalar_corpus")
    add_test(NAME native_scalar_corpus_gen
             COMMAND python3 "${CMAKE_CURRENT_SOURCE_DIR}/tests/native/scalar/gen/gen_corpus.py"
                     --out "${SU_NATIVE_CORPUS_DIR}"
                     --expect-sha "${CMAKE_CURRENT_SOURCE_DIR}/tests/native/scalar/gen/corpus.sha256")
    add_test(NAME native_scalar COMMAND test_native_scalar "${SU_NATIVE_CORPUS_DIR}"
             WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    set_tests_properties(native_scalar PROPERTIES DEPENDS native_scalar_corpus_gen)
endif()
# ---- END M1-01 optional native scalar backend ----
