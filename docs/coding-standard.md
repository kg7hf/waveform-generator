# Embedded C++ and documentation standard

## Scope and authority

This is the local engineering profile for owned C++ in `common`, `signal-lab`,
`waveform-source`, optional encoder modules, `rt1170`, host C++ tools and their tests.
It defines the waveform generator's embedded execution, interface and
maintenance requirements independently of any other project.

The [C++ Core Guidelines](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines)
provide the general rationale: P.4/P.5 for explicit, checked interfaces;
I.13 and F.24 for bounded sequences; C.21 for special-member consistency;
R.1 for resource ownership; CP.2/CP.8 for race-free synchronization; E.25â€“E.28
for the project's exceptions-disabled error strategy. The no-exceptions policy
is our embedded profile, not a claim that the general guidelines ban exceptions.

## Required implementation rules

| Area | Project rule | Enforcement / review evidence |
|---|---|---|
| Language | C++23; no extensions, exceptions or RTTI | Root CMake and `cmake/project_options.cmake` |
| Diagnostics | Treat owned C++ warnings as errors; do not hide a new warning with a blanket suppression | GCC `-Wall -Wextra -Wpedantic -Werror`; corresponding MSVC policy |
| Failure | `[[nodiscard]]` status/result or checked count; `noexcept` on portable/real-time interfaces | Headers, caller review, adversarial tests |
| Construction | Fallible setup returns status; start tasks/audio only after configuration succeeds | Configure/start lifecycle and failure tests |
| Storage | No heap allocation on sample, codec callback or ISR paths; fixed-capacity workspaces and bounded queues | Source inspection, capacities/static assertions, target map |
| Ownership | Resources have one owner; raw pointers borrow; document lifetime and invalidation | Noncopyable engine/source/WAV jobs; file/handle cleanup |
| Buffers | Prefer `std::span` at new C++ boundaries and `std::array` for owned arrays; check lengths before indexing | Engine span entry points, maximum counts, legacy adapter checks |
| Numbers | Validate finite values and range before conversion; name units and rounding | Scenario validation, PCM boundary tests, sidecar format |
| Concurrency | One mutation owner; bounded messages; lock-free atomics for independently published flags/indices; critical sections for compound snapshots | RT owner-loop tests and lock-free static assertions |
| DMA | Explicit memory placement, alignment, cache and publication contracts | Linker map and platform audio code; physical checks still required |
| Determinism | No fast-math; keep deliberate float/double boundaries and reproducible random streams | Build flags, block-size/digest tests, retained source identity |
| Interfaces | Self-contained `#pragma once` headers, forward-slash includes, explicit namespaces | Builds and source review |
| Dependencies | Ordinary Git submodules; build-tree patches; no implicit sibling source inputs | Dependency setup tests and CMake boundaries |

### Accepted boundaries

* Offline C++ tools may allocate for paths, payload planning and output assembly.
  Exceptions remain disabled; allocation exhaustion is fail-fast. Filesystem
  operations use error-code overloads. This does not make those tools real-time.
* Python tools allocate and use exceptions. They are host utilities, outside the
  embedded execution contract. Python rendering now emits PCM24 and reports its
  own version, numerical implementation and random generator.
* Existing C/vendor interfaces and legacy pointer-plus-count overloads remain
  explicit compatibility boundaries. Their counts are mandatory. New C++ clients
  should use bounded views; a raw pointer does not transfer ownership.
* Existing fixed C arrays with named capacity remain in serialized descriptions
  and imported DSP internals. Do not reinterpret such aggregates as wire bytes.
  New queue/DMA payload types need trivial-copy and size/alignment assertions.
* Namespaces retain component ownership (`signal_lab`, `waveform_source`,
  `native_m110`, imported `m110`). Merging unrelated libraries into one namespace
  would obscure the standalone boundary.
* Platform code owns FreeRTOS/CMSIS calls. Aggregate 64-bit telemetry uses short
  critical sections, avoiding non-lock-free atomic operations on Cortex-M7.
  Do not call audio hooks, log, allocate or perform file I/O while holding a lock.
* Volatile remains appropriate for MMIO and debugger-observable diagnostic
  records. It must never be the mechanism that publishes task/ISR state.
* The JSON parser has fixed token/storage/depth limits. Its shared scratch has a
  non-spinning atomic guard: overlapping parse attempts return failure. It is a
  configuration operation and must not be introduced into an audio callback.
* Selected upstream C code keeps its vendor style/policy. Change it through a
  documented patch in `patches/`, never by silently editing a submodule.

## Mechanical style

Use Allman braces, four spaces, type-aligned pointers/references, snake_case
functions, PascalCase types, named constants and a 200-column ceiling. Existing
vendor callback spellings are fixed by their ABI. Comments explain purpose,
ownership, units and decisions; avoid narrating obvious syntax.

[`scripts/cgm_options.txt`](../scripts/cgm_options.txt) is the AStyle formatting
authority, including explicit break-blocks rules.
`.clang-format` gives editors a compatible starting point. Install AStyle or pass
its executable explicitly; the repository does not depend on a sibling checkout.

```powershell
.\scripts\format-source.ps1 -AStylePath 'C:\Tools\AStyle\AStyle.exe'
.\scripts\format-source.ps1 -AStylePath 'C:\Tools\AStyle\AStyle.exe' -Check
```

The script visits owned `.cpp`/`.hpp` files only. It excludes submodules,
generated vendor overlays and C compatibility imports. Formatting is restricted to project-owned C++; dependency sources keep their upstream style.

## Documentation contract

Every major subsystem owns one Markdown guide under [modules](modules/README.md):

1. Scope and 50,000-foot inputs/output diagram.
2. Operator/API usage with prerequisites and working commands.
3. Scientist/maintainer view: data structures, units, equations, capacities,
   state transitions, ownership and failure handling.
4. Plain-language intuition and a small worked example.
5. Source reading order, function walkthrough, invariants and meaningful tests.

At important source entry points, provide a file banner linking the owning guide.
For public operations explain borrowed lifetimes, capacity, units, which state
changes on failure, and who may call them. Keep deep mathematical derivations in
the owning guide and the code comments necessary to use the interface safely.

## Change acceptance

Build host and relevant firmware profiles, run applicable tests and formatting,
and check that required dependency files are available. Numerical changes need edge values and
block-size coverage. Ownership changes need cancellation/failure cases. Report
target memory when relevant. Board timing, DMA/cache visibility and analog
behavior require separate physical validation; software tests cannot establish
them. This profile is an engineering standard, not a safety certification.
