# Backends and dependency decisions

## 1. Minimize new research on the critical path

The project's novelty should be the development/runtime contract, not simultaneously reinventing JSON, instruction encoding, a linker, a shader compiler, an operating-system sandbox, and a complete programming language. Reuse small pinned tools behind narrow adapters. A dependency compiled once is different from a dependency rebuilt on every gameplay edit.

M0 requires C17, native platform support, the build toolchain, a strict JSON library, and a reviewed hashing implementation. Python standard-library tooling is outside the hot runtime. The native JSON default is yyjson; its own documentation describes the library and APIs [S10]. Review its parser configuration and enforce duplicate-key/semantic policy at our boundary rather than assuming all defaults meet SU-LIR requirements.

## 2. Native backend decision

M1's default is **AsmJit in a narrow C++ translation-unit boundary exposing a C ABI**. It already provides machine-code emission and optional register allocation [S01]. The rest of the core remains C17; AsmJit headers do not spread through `include/sutekh/`.

Initially lower only SU-LIR 0.1. Implement a scalar backend before AVX2. Use an explicit stack/register plan rather than a general optimizer. Record source mappings from IR instruction indices to native ranges and preserve a disassembly inspection path. Do not hand-encode VEX/EVEX instructions to satisfy an “assembly purity” metric.

If a concrete platform/tooling issue blocks AsmJit, record an architecture decision with evidence and an alternative. Cranelift can emit callable code into memory and is an available reference or future optimizing backend [S02], but M1 must not grow two backends merely because both are interesting. A Rust dependency in a prebuilt optional tool is not the same as imposing Rust compilation on all game edits.

## 3. Handwritten assembly

Raw x86-64 kernels are optional trusted native components. Every kernel has an ABI, read/write/alignment contract, scalar reference, zero/tail/bounds tests, differential corpus, and a measured justification. The assembly lane uses a pinned assembler when source changes; normal Live IR does not spawn it.

Avoid handwritten replacements for vetted security primitives or general memory routines without a demonstrated bottleneck and expert-quality test coverage. Assembly is not a substitute for an algorithmic or layout fix.

## 4. Native memory and safety

Generated code pages follow writable-then-executable publication, with no permanent RWX mapping. Handle allocation/protection failures explicitly and preserve the previous implementation. Apply platform cache synchronization rules [S03]. Debug/unwind registration, return-address handling, and ABI tests are part of backend completion.

Checked code generation is still a trusted compiler implementation, not a complete malicious native-code verifier. Admit unreviewed native candidates only in isolated workers. A later optional WebAssembly sandbox can be evaluated when mod/plugin requirements justify it; it is not imposed as an extra M0 runtime.

## 5. Shader/backend tooling

Use an established shader compiler through an adapter when rendering begins. Cache outputs by source, includes, options, target capabilities, compiler version, and resource layout. Do not begin with a custom SPIR-V compiler.

The driver can still compile pipeline/shader work internally, and pipeline creation may be expensive [S11]. Schedule it off the render-critical path and keep a compatible old pipeline or safe fallback until readiness. Native CPU code generation does not remove GPU compilation costs.

## 6. Toolchain locking

Bootstrap records exact versions/commits, download origins, checksums, license texts, patches, target triples, compiler/linker flags, and cache key inputs. Unsupported platform configurations are reported rather than silently emulated through undefined ABI assumptions.

Do not fetch dependencies at runtime in a shipped game. Development builds can operate offline after approved dependencies are present. A new dependency must identify its problem, footprint/build cost, update policy, and simpler alternatives. No arbitrary “zero dependencies” rule should force years of unnecessary reimplementation.

## 7. Decision ledger

| Decision | Status | Revisit trigger |
|---|---|---|
| C17 resident core | Selected | Measured concrete blocker, not fashion |
| JSON IR before custom syntax | Selected for M0 | M0 complete; author ergonomics measured |
| Reference VM before JIT | Selected | Never remove semantic oracle |
| AsmJit C-ABI adapter | Selected for M1 | Documented unsupported need/performance evidence |
| No CPU/GPU autotuner in M0 | Selected | Representative GPU workloads and cost model exist |
| Static SoA before auto-layout | Selected | Real access traces and migration tests exist |
| Engine license | Owner decision pending | Before public distribution |
| Final name/repository | Owner decision pending | Before publication/branding |

Sources: [SOURCE_NOTES.md](SOURCE_NOTES.md).
