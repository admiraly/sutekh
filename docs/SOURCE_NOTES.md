# Primary sources and evidence boundaries

Checked on **6 October 2026**. These sources support concrete tooling/platform facts; they do not validate Sutekh's proposed latency targets or engine performance. Dependency APIs and service limits may change; implementation agents must pin actual versions and recheck configuration-sensitive claims. No external benchmark numbers have been adopted as Sutekh results.

## S01 — AsmJit

Publisher: AsmJit project. Source: `https://asmjit.com/`

Supports the decision that an existing C++ library can emit machine code and provide optional register allocation behind an adapter. It does not verify our IR or establish an engine speedup.

## S02 — Cranelift JITModule

Publisher: Bytecode Alliance / crate documentation. Source: `https://docs.rs/cranelift-jit/latest/cranelift_jit/struct.JITModule.html`

Documents in-memory callable code/data, finalization, and the safety precondition that code memory must not be released while functions/pointers remain in use. It is a reference/alternative backend, not an M0 dependency.

## S03 — Windows generated-code page protection

Publisher: Microsoft. Source: `https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualprotect`

Documents changing page protections and the caller's responsibility for instruction-cache coherency when publishing executable memory. Our W^X policy and retirement protocol are design requirements built around platform support.

## S04 — Windows x64 calling convention

Publisher: Microsoft. Source: `https://learn.microsoft.com/en-us/cpp/build/x64-calling-convention?view=msvc-170`

Documents argument registers, stack/shadow-store conventions, register preservation, alignment, and unwind concerns relevant to a Windows native backend. The engine must separately implement/test each supported ABI.

## S05 — Linux performance-monitoring access

Publisher: Linux kernel documentation. Source: `https://cdn.kernel.org/doc/html/latest/admin-guide/perf-security.html`

Documents permission, capability, and resource controls for performance monitoring. This supports explicit unavailable/null telemetry rather than promising all counters on all machines. Do not weaken machine security solely for convenience.

## S06 — Clang numeric compilation modes

Publisher: LLVM/Clang. Source: `https://clang.llvm.org/docs/UsersManual.html`

Documents floating-point control and compilation behavior. The architecture consequently defines contraction/rounding/other arithmetic behavior explicitly before admitting a cross-backend deterministic FP profile. The source does not establish original BFME arithmetic semantics.

## S07 — LLD

Publisher: LLVM. Source: `https://lld.llvm.org/`

Documents the linker family and supported formats. We use it as the default native linker option rather than promising another application's link benchmark transfers to this project.

## S08 — GitHub Actions billing

Publisher: GitHub. Source: `https://docs.github.com/en/billing/concepts/product-billing/github-actions`

Documents free standard-runner use for public repositories and separate charging for larger runners, plus storage/billing rules. Recheck the actual account and runner configuration before enabling paid resources.

## S09 — GitHub Actions limits

Publisher: GitHub. Source: `https://docs.github.com/en/actions/reference/limits`

Documents concurrency, workflow/job, storage, and related service limits. Public repository status does not create unlimited concurrent capacity or a low-latency interactive builder.

## S10 — yyjson

Publisher: yyjson project. Source: `https://ibireme.github.io/yyjson/`

Documents the C JSON library selected for the native metadata boundary. Sutekh's stricter duplicate-key, schema, type, and resource rules require their own verification and tests.

## S11 — Vulkan pipeline management

Publisher: Khronos Vulkan Documentation Project. Source: `https://docs.vulkan.org/samples/latest/samples/performance/pipeline_cache/README.html`

Documents driver-side shader compilation during pipeline creation and the use of caching/preparation to reduce stalls. Producing SPIR-V does not make GPU pipeline creation free.

## S12 — Vulkan pipeline creation cache control

Publisher: Khronos Vulkan Documentation Project. Source: `https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_pipeline_creation_cache_control.html`

Documents an API mechanism for controlling creation behavior when compilation would be required, subject to implementation support. It informs a later off-critical-path pipeline manager, not a zero-latency guarantee.

## User-derived requirements, not externally verified engine facts

The BFME II/RotWK scope, War of the Ring exclusion, user-reported hardware, named game concepts, maximum agent velocity preference, and desire for assembly-level control come from the conversation/project context. We have not inspected current game repositories or run their binaries while preparing this specification. No existing repository was modified, no engine benchmark was run, and no engine license or remote repository was created.
