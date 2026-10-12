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

# ---- BEGIN M2-00a generational entity table (su_ents; contract r2) ----
add_library(su_ents STATIC src/world/entities/su_ents.c)
target_include_directories(su_ents PUBLIC src "${SUTEKH_CONTRACT_INCLUDE}")
sutekh_apply_warnings(su_ents)
add_executable(test_entities tests/entities/test_entities.c)
target_link_libraries(test_entities PRIVATE su_ents)
sutekh_apply_warnings(test_entities)
add_test(NAME entities COMMAND test_entities)
# ---- END M2-00a generational entity table ----

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
target_link_libraries(su_agent PUBLIC su_vm su_world su_ir su_platform su_snapshot su_jobs)
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

# M1 closure B: the live tick loop with tick workers (schedule.c over su_jobs), compared with serial.
add_executable(test_tick_workers tests/scheduler/test_tick_workers.c)
target_link_libraries(test_tick_workers PRIVATE su_agent)
target_compile_definitions(test_tick_workers PRIVATE _POSIX_C_SOURCE=200809L)
sutekh_apply_warnings(test_tick_workers)
add_test(NAME tick_workers COMMAND test_tick_workers WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

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

    # M1-03: AVX2 lowering (C++, same AsmJit library) and host capability detection (C).
    add_library(su_native STATIC backends/native/su_native.c backends/native/su_native_asmjit.cpp
                backends/native/avx2/su_native_avx2.cpp backends/native/avx2/su_native_caps.c)
    target_include_directories(su_native PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" src "${SUTEKH_CONTRACT_INCLUDE}")
    target_link_libraries(su_native PUBLIC su_ir su_asmjit)
    target_compile_definitions(su_native PRIVATE SU_NATIVE_ASMJIT_COMMIT="${SU_ASMJIT_COMMIT}")
    set_target_properties(su_native PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    # Strict C17 set for the C files. The C++ adapter gets -Wall -Wextra -Werror only: the AsmJit
    # headers rely on compiler extensions that -pedantic rejects.
    target_compile_options(su_native PRIVATE
        $<$<AND:$<COMPILE_LANGUAGE:C>,$<C_COMPILER_ID:GNU,Clang,AppleClang>>:-Wall -Wextra -Werror -pedantic>
        $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:GNU,Clang,AppleClang>>:-Wall -Wextra -Werror>)

    # M1-02b: the engine (supervisor and worker) gets the native artifact registry. The compile
    # definition SU_HAVE_NATIVE gates world.specialize; the default build does not define it.
    target_sources(su_agent PRIVATE src/runtime/native_versions.c)
    target_compile_definitions(su_agent PUBLIC SU_HAVE_NATIVE)
    target_link_libraries(su_agent PUBLIC su_native)

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

    # M1-03 AVX2 tests. The corpus is generated by the scalar generator with its own seed and
    # count; the digest is stored in tests/native/avx2/gen/corpus.sha256.
    add_executable(test_native_avx2 tests/native/avx2/test_native_avx2.c)
    target_link_libraries(test_native_avx2 PRIVATE su_native su_vm su_world su_ir)
    sutekh_apply_warnings(test_native_avx2)

    set(SU_NATIVE_AVX2_CORPUS_DIR "${CMAKE_BINARY_DIR}/native_avx2_corpus")
    add_test(NAME native_avx2_corpus_gen
             COMMAND python3 "${CMAKE_CURRENT_SOURCE_DIR}/tests/native/scalar/gen/gen_corpus.py"
                     --out "${SU_NATIVE_AVX2_CORPUS_DIR}"
                     --seed 20261012 --count 5000
                     --expect-sha "${CMAKE_CURRENT_SOURCE_DIR}/tests/native/avx2/gen/corpus.sha256")
    add_test(NAME native_avx2 COMMAND test_native_avx2 "${SU_NATIVE_AVX2_CORPUS_DIR}"
             WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    set_tests_properties(native_avx2 PROPERTIES DEPENDS native_avx2_corpus_gen)

    # M1-02b: artifact registry lifetime, admission and mapping hygiene (tests/native/qualification/).
    add_executable(test_native_versions tests/native/qualification/test_native_versions.c)
    target_link_libraries(test_native_versions PRIVATE su_agent su_native su_ir)
    sutekh_apply_warnings(test_native_versions)
    add_test(NAME native_versions COMMAND test_native_versions "${CMAKE_CURRENT_SOURCE_DIR}"
             WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

    # M1-02c-1: world.specialize admission in a disposable worker (C07). The test worker is the production
    # worker entry compiled with SU_TEST_FAULT_HOOKS and the native registry; the seams exist only there.
    add_executable(native_test_worker tests/native/qualification/native_test_worker.c src/runtime/worker.c
                   src/runtime/native_versions.c)
    target_link_libraries(native_test_worker PRIVATE su_agent)
    target_compile_definitions(native_test_worker PRIVATE _POSIX_C_SOURCE=200809L SU_TEST_FAULT_HOOKS)
    sutekh_apply_warnings(native_test_worker)
    add_executable(test_specialize_admission tests/native/qualification/test_specialize_admission.c)
    target_link_libraries(test_specialize_admission PRIVATE su_agent su_native su_ir)
    sutekh_apply_warnings(test_specialize_admission)
    add_dependencies(test_specialize_admission native_test_worker)
    add_test(NAME specialize_admission COMMAND test_specialize_admission $<TARGET_FILE:native_test_worker>
             "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/specialize_admission_scratch"
             WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    # The production engine binary must carry none of the fault seams.
    # M1-02c-2: native kernel crash, quarantine and respecialization (test worker only).
    add_executable(test_native_fault tests/native/qualification/test_native_fault.c)
    target_link_libraries(test_native_fault PRIVATE su_agent su_native su_ir)
    sutekh_apply_warnings(test_native_fault)
    add_dependencies(test_native_fault native_test_worker)
    add_test(NAME native_fault COMMAND test_native_fault $<TARGET_FILE:native_test_worker>
             "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/native_fault_scratch"
             WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    # M1-02c-3: test supervisor = the agent library compiled with the test seams (SU_TEST_INTERLEAVE).
    # Only the test links it; sutekh is built from su_agent without SU_TEST_FAULT_HOOKS.
    add_library(su_agent_test STATIC ${SU_AGENT_SOURCES} src/runtime/native_versions.c)
    target_include_directories(su_agent_test PUBLIC src "${SUTEKH_CONTRACT_INCLUDE}")
    target_link_libraries(su_agent_test PUBLIC su_vm su_world su_ir su_platform su_snapshot su_native su_jobs)
    target_compile_definitions(su_agent_test PRIVATE _POSIX_C_SOURCE=200809L SU_HAVE_NATIVE SU_TEST_FAULT_HOOKS)
    sutekh_apply_warnings(su_agent_test)
    # M1 closure C: safe retirement (in-flight count, retirement inside a kernel call) and the quarantine
    # key. In process, against the agent library compiled with the test seams.
    add_executable(test_safe_retire tests/native/qualification/test_safe_retire.c)
    target_link_libraries(test_safe_retire PRIVATE su_agent_test su_native su_ir)
    target_compile_definitions(test_safe_retire PRIVATE _POSIX_C_SOURCE=200809L SU_HAVE_NATIVE)
    sutekh_apply_warnings(test_safe_retire)
    add_test(NAME safe_retire COMMAND test_safe_retire "${CMAKE_CURRENT_SOURCE_DIR}"
             WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    # M1 closure C: chunked native kernels (N in {1,2,4} x {vm, scalar, avx2}, 100,001 and 1,000,000 rows,
    # chunk failure keeps the committed state). In process, against the agent library with the seams.
    add_executable(test_native_chunked tests/native/qualification/test_native_chunked.c)
    target_link_libraries(test_native_chunked PRIVATE su_agent_test su_native su_ir su_jobs)
    target_compile_definitions(test_native_chunked PRIVATE _POSIX_C_SOURCE=200809L SU_HAVE_NATIVE)
    sutekh_apply_warnings(test_native_chunked)
    add_test(NAME native_chunked COMMAND test_native_chunked "${CMAKE_CURRENT_SOURCE_DIR}"
             WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    add_executable(test_specialize_inflight tests/native/qualification/test_specialize_inflight.c)
    target_link_libraries(test_specialize_inflight PRIVATE su_agent_test su_native su_ir)
    sutekh_apply_warnings(test_specialize_inflight)
    add_dependencies(test_specialize_inflight native_test_worker)
    add_test(NAME specialize_inflight COMMAND test_specialize_inflight $<TARGET_FILE:native_test_worker>
             "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/specialize_inflight_scratch"
             WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    # M1-02c-3: 1000 specialize cycles and revision swaps on one agent (production supervisor).
    add_executable(test_specialize_churn tests/native/qualification/test_specialize_churn.c)
    target_link_libraries(test_specialize_churn PRIVATE su_agent su_native su_ir)
    sutekh_apply_warnings(test_specialize_churn)
    add_dependencies(test_specialize_churn native_test_worker)
    add_test(NAME specialize_churn COMMAND test_specialize_churn $<TARGET_FILE:native_test_worker>
             "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/specialize_churn_scratch"
             WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    # Under AddressSanitizer the allocator quarantine (256 MB by default) inflates RSS for reasons
    # unrelated to the engine, and the churn test's 16 MB RSS bound would fail. Cap the quarantine for
    # this test only; any ASAN_OPTIONS already set in the environment are kept. Non-sanitizer builds
    # ignore ASAN_OPTIONS.
    set(_su_churn_asan "quarantine_size_mb=1")
    if(DEFINED ENV{ASAN_OPTIONS} AND NOT "$ENV{ASAN_OPTIONS}" STREQUAL "")
        set(_su_churn_asan "$ENV{ASAN_OPTIONS}:${_su_churn_asan}")
    endif()
    set_tests_properties(specialize_churn PROPERTIES ENVIRONMENT "ASAN_OPTIONS=${_su_churn_asan}")
    add_test(NAME release_seams COMMAND python3
             "${CMAKE_CURRENT_SOURCE_DIR}/tests/native/qualification/check_release_seams.py"
             $<TARGET_FILE:sutekh>)
endif()
# ---- END M1-01 optional native scalar backend ----

# M1-02a: worker crash recovery tests (crash seam only in the test worker, never in sutekh)
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/recovery_scratch")
add_executable(recovery_worker tests/recovery/recovery_worker.c src/runtime/worker.c)
target_link_libraries(recovery_worker PRIVATE su_agent)
target_compile_definitions(recovery_worker PRIVATE _POSIX_C_SOURCE=200809L SU_TEST_FAULT_HOOKS)
sutekh_apply_warnings(recovery_worker)
add_executable(test_recovery tests/recovery/test_recovery.c)
target_link_libraries(test_recovery PRIVATE su_agent)
target_compile_definitions(test_recovery PRIVATE _POSIX_C_SOURCE=200809L)
sutekh_apply_warnings(test_recovery)
add_test(NAME recovery COMMAND test_recovery $<TARGET_FILE:recovery_worker> "${CMAKE_CURRENT_SOURCE_DIR}"
         "${CMAKE_BINARY_DIR}/recovery_scratch" $<TARGET_FILE:sutekh>
         WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

# M1 closure C: C07 containment observed from inside the child (rlimits, seccomp network denial,
# RLIMIT_FSIZE and RLIMIT_NOFILE). The probe method exists only in test workers (SU_TEST_FAULT_HOOKS).
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/containment_scratch")
add_executable(test_containment tests/recovery/test_containment.c)
target_link_libraries(test_containment PRIVATE su_agent)
target_compile_definitions(test_containment PRIVATE _POSIX_C_SOURCE=200809L)
sutekh_apply_warnings(test_containment)
add_dependencies(test_containment recovery_worker)
add_test(NAME containment COMMAND test_containment $<TARGET_FILE:recovery_worker>
         "${CMAKE_BINARY_DIR}/containment_scratch" "${CMAKE_CURRENT_SOURCE_DIR}"
         WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

# M1 closure D: digest field comparison and spool directory mode (unit checks, no worker).
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/hardening_scratch")
add_executable(test_hardening tests/recovery/test_hardening.c)
target_link_libraries(test_hardening PRIVATE su_agent)
target_compile_definitions(test_hardening PRIVATE _POSIX_C_SOURCE=200809L)
sutekh_apply_warnings(test_hardening)
add_test(NAME hardening COMMAND test_hardening "${CMAKE_BINARY_DIR}/hardening_scratch"
         WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
