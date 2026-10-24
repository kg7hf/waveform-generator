# Embedded C++ and documentation standard

## Scope and authority

This is the local engineering profile for owned C++ in `common`, `signal-lab`,
`waveform-source`, optional encoder modules, `rt1170`, `rt1064`, host C++ tools, the
channel simulator `channel-sim` (its chain driver, CLI and RNG; pathsim itself is
third-party code, `third_party/pathsim`) and their tests.
It defines the waveform generator's embedded execution, interface and
maintenance requirements independently of any other project.

The rules below apply to project-owned code. Owning API contracts and target
constraints govern waveform-specific types and build settings; the local gates
below provide the executable checks for this repository.

New C++ tests use Catch2. Keep raw test evidence outside the repository and summarize validation in the change description. Contribution workflow is documented in [Contributing](development/contributing.md).

The [C++ Core Guidelines](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines)
provide the general rationale: P.4/P.5 for explicit, checked interfaces;
I.13 and F.24 for bounded sequences; C.21 for special-member consistency;
R.1 for resource ownership; CP.2/CP.8 for race-free synchronization; E.25 through E.28
for the project's exceptions-disabled error strategy. The no-exceptions policy
is our embedded profile, not a claim that the general guidelines ban exceptions.

## Required implementation rules

| Area | Project rule | Enforcement / review evidence |
|---|---|---|
| Language | C++23; no extensions, exceptions or RTTI | Root CMake and `cmake/project_options.cmake` |
| Diagnostics | Treat owned C++ warnings as errors; do not hide a new warning with a blanket suppression | GCC `-Wall -Wextra -Wpedantic -Werror`; corresponding MSVC policy |
| Failure | `[[nodiscard]]` status/result or checked count; `noexcept` on portable/real-time interfaces | Headers, caller review, adversarial tests, the [failure gates](#failure-gates) |
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

Processing completion, stop completion, worker retirement, and buffer return
are separate lifecycle conditions. A failed stop must not release resources
that a worker, driver or DMA engine can still access. Constructors stay inert
when setup can fail; explicit setup/start calls establish validity. Device
callbacks hand off bounded data or ownership, and report overflow as a
discontinuity. They must not allocate, block, perform file/network I/O or run
unbounded processing. Portable algorithms remain independent of UI and platform
adapters; platform code owns drivers, clocks and RTOS integration.

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

### Failure gates

A fallible function returns `m110::Status` or `m110::Result<T>`
(`common/status.hpp`) and says why it failed only there (#50). Owned C code,
which cannot return a Status, returns `enum wfg_status`
(`rt1170/platform/wfg_status.h`): `kWFG_StatusOk`, or a value named for the
failure. Each such C function is declared `__attribute__((warn_unused_result))`,
C's `[[nodiscard]]`. At the C++ boundary `rt1170/c_status_map.hpp` maps each
value to a Status (a code and a static message) and back, one row per value,
pinned by static_assert. A vendor callback whose `bool` TinyUSB or the NXP SDK
fixes stays `bool` and turns the status into that `bool` in one place. A
Result taken as a value while it holds an error is a defect: `Result::value()` passes the
Status to `m110::critical_error()` and the installed handler. The firmware
installs `rt1170/critical_error.hpp`'s, which records it for a debugger. Each
host tool's `main()` (`signal_lab_render`, `wfg_watterson_render`,
`waveform_capture`, `pathsim_chain`) installs `common/host_critical_error.hpp`'s
as its first statement: one line on stderr, `<tool>: critical error: <code>
(<number>): <message>; call site 0x<address>`, then the tool's failure status.
`wfg-host-critical-error` runs a test build of `signal_lab_render` that takes
the value of a failed Result (`--critical-error-probe`, compiled in only with
`WFG_CRITICAL_ERROR_PROBE`) and checks that line and its exit status, and it
reads every owned `main()` for the call. Three gates hold the return types
and how a C status is read. ctest runs all three (the AST check where
clang-query 19 or later is installed); the host CI's checks job runs the two
text gates and its host job the AST check. Configure finds each clang tool
at LLVM 19 or later (`cmake/wfg_clang_tools.cmake`): `<tool>-19` first, then
`<tool>`, passing over one that reports an older LLVM, as ubuntu-24.04's
`/usr/bin/clang-tidy` (18) is; `wfg-clang-tools-version` holds it to that:

* `scripts/check-error-style.py` fails an error out-parameter and an error
  text kept in an object: a `const char**`; a `const char*&` named for an
  error or `what`; a string (`std::string`, `std::string_view`, another
  standard string type or a `std::optional` of one) written through a
  reference or a pointer and named for an error, or `what`, `detail(s)`,
  `text`, `e` or `out`; a `char*` or `std::span<char>` error buffer; a
  non-const `Status&` or `Status*`; either half of a
  `set_error()`/`error()` pair, a setter that takes an error text or an
  accessor that gives one; and a function with parameters, named for an
  error or `what`, that returns its failure as a text (a `char` pointer, a
  string or a `std::optional` of one). An object that latches a failure
  keeps a `Status` and gives it as one (`Engine::status()`). Only a function
  given the failure it names returns its text: its one parameter a `Status`,
  a `StatusCode`, or an error or reason enum, as the published-name tables
  (`rt1170/error_map.hpp`'s `error_name()`, `host/capture`'s `reason_name()`)
  and `wfg_watterson_render`'s `scenario_failure()` and `process_failure()`
  do. That last rule reads only namespace, class, `extern "C"` and file
  scope: owned code declares no function in a block, so there
  `std::string message(count * width, ' ')` is a variable. It also fails a
  C status read as a truth value: `kWFG_StatusOk` is 0, so a call of a
  function declared to return `enum wfg_status` that is left in the old
  `bool` form (`if (!f())`, `if (f())`, `bool ok = f()`, an operand of `&&`
  or `||`) compiles, satisfies `warn_unused_result` and inverts. A call
  compared first (`f() != kWFG_StatusOk && ready`) or passed on as it is
  (`return f()`, `status_of(f())`) passes. The text sees a call only, in
  these forms; the AST check below finds the rest. And it fails a function that
  returns `enum wfg_status` without `__attribute__((warn_unused_result))`:
  GCC cannot put the attribute on the type, so each declaration carries it
  (or `[[nodiscard]]`), and a definition without it passes only when a
  declaration of its name, in any owned source, carries it.
* `scripts/check-bool-returns.py` holds the functions that return `bool`,
  because no pattern can tell a question from a failure.
  `scripts/bool-returns-baseline.json` lists every such function in the owned
  non-test code (the complexity ratchet's paths and `signal-lab/validation`)
  under one of three classes:
  * `fallible`: an operation that reports a failure as `false` (a read, a write,
    a parse, an open, a refused request). #50 took this list to zero, and it
    stays at zero: the check fails on any entry in it.
  * `predicates`: a function whose `false` is an answer, never a failure: a
    question (`is_*`, `has_*`, `empty`, a comparison, a range or table check)
    or a poll whose `false` means "nothing yet". Each entry says why.
  * `abi`: a callback whose `bool` an outside C interface fixes (TinyUSB's MSC
    and UAC2 callbacks, the NXP SDK's card detect). Each entry names the
    interface. These stay `bool`.

  The fallible list is empty, and any entry in it fails, so a function that
  can fail returns a Status or a Result (owned C code an `enum wfg_status`). A
  new bool-returning function fails the check unless the same commit lists it
  as a predicate (or an `abi` callback) with its reason. So does a second
  overload of a listed name. Against the previous revision's baseline (HEAD's
  for the work tree, the first parent's with `--rev <commit>`), a fallible
  entry moved to `predicates` or `abi`, and a predicate or `abi` entry that
  counts more declarations, fail unless the baseline's `reclassified` list
  names it with why and that count, so that admitting a fallible function
  under a predicate's name shows in review. An entry
  whose function is gone fails until `--update` drops it, so a function brought
  back with `bool` finds no stale entry. `--update` only drops and lowers
  entries; it never adds one. Comments, literals and preprocessor directives
  are not code. A function whose return type is deduced (`auto`, a lambda
  without `->`) is found when its own body has `return true;` or
  `return false;`; one that returns only another bool expression is not.
  `--list` prints every function found with its class.
* `scripts/check-c-status-truth.py` (#51) finds a C status read as a truth
  value by the AST, beside the text rule above, also in the forms text
  cannot see: `(bool)f()`, `static_cast<bool>(f())`, `assert(f())`,
  `if ((f()))`, `for (; f(); )`, and a stored status
  (`enum wfg_status s = f(); if (s)`, `bool ok = s`, `s ? a : b`), a member,
  a typedef, a template. It runs clang-query over every owned unit of a
  compile database (the host build's by default; `--build-dir` names
  another), with each compile command the database has for it and NDEBUG
  undefined, so an `assert()` in a release build is read too. Each matcher
  binds its own name, which the finding gives as its form: `bool-conversion`
  (an implicit conversion to `bool`, QA's matcher: in C++ every truth read,
  in C a `_Bool` initialised, assigned, passed or returned), `bool-cast` (to
  `bool` or `_Bool`), the condition of an `if`, `while`, `do`, `for` and `?:`,
  and the operand of `!`, `&&` and `||`, which C reads with no conversion to
  `bool`, and `bitwise-operand`, an operand of `&`, `|`, `^`, `&=`, `|=` or
  `^=`: a status is no set of bits, so that is the old `bool` accumulation
  (`ok &= f()`), false on success. A status compared with `kWFG_StatusOk`, read through `status_of()`,
  passed on, stored or switched on passes. `boolean-comparison` also catches
  C++ comparisons with the `true` and `false` literals. C spells those literals
  as integer macros, indistinguishable in the AST from deliberate comparisons
  with `0` or `1`, so those ambiguous C comparisons are not flagged.
  C promotes an enum conditional expression to an integer before its result
  is read as truth, so the ordinary condition and conversion matchers cannot
  see the status arms. `conditional-branch` reports each status arm when the
  conditional directly supplies an `if`, `while`, `do` or `for` condition;
  `conditional-not-operand` and `conditional-logical-operand` report arms
  when the conditional directly supplies `!`, `&&` or `||`; and
  `conditional-bool-conversion` reports arms when the promoted result is
  implicitly converted to `bool`/`_Bool`, including assignment and return.
  `integer-cast-truth` reports a status explicitly cast to an integer and then
  directly read by an `if`, `while`, `do`, `for` or `?:` condition, `!`, `&&`,
  `||`, or an implicit bool conversion; `integer-cast-conditional-branch`
  reports cast status arms when their conditional result is read in those
  contexts. A cast inside an explicit comparison such as `(int)status == 0`
  or `static_cast<int>(status) != kWFG_StatusOk` remains a deliberate numeric
  comparison and is not flagged. The conditional and integer-cast matchers
  target direct operands; they do not follow status arms nested through
  arbitrary arithmetic or intermediate integer variables. A comparison such
  as `(ready ? status : probe()) ==
  kWFG_StatusOk` remains a valid status comparison and is not reported. The hidden
  `status-seen` sentinel uses the same canonical type anchor as the other
  matchers and requires a checked function declaration that returns or accepts
  the enum, so a misspelled type anchor cannot silently produce a clean result.
  Values carried through a separate integer variable are outside this local
  AST check; arbitrary interstatement dataflow is not tracked.
  A template finding points to its status read in the template source; the
  compiler AST does not reliably map that read back to each instantiation call.
  It also fails when it could not check: a unit clang-query cannot parse (clang-query still exits 0 and
  matches what it parsed), a matcher that does not parse, fewer matchers run
  than given, and a database with no owned unit. `tests/c_status_truth` is
  its fixture, every supported form in C and C++ with each line marked with
  the matchers that must find it, including C-only conditional-promotion
  forms and explicit comparison negatives; the text rule skips that directory, whose
  reads are wrong on purpose.

  ```powershell
  python scripts/check-c-status-truth.py --build-dir build/host-release --jobs 8
  ```

  ctest and the host CI read the host database. The firmware-only units
  (`rt1170/main.cpp`, `player.cpp`, `usb_service.cpp`, `tx_usb.cpp`,
  `os/task.cpp`, the platform's board sources and `usb_audio_tinyusb.c` in
  the player build; `sdcard_probe.cpp` and `tone.cpp` in theirs), and what
  only the firmware's macros reach (the `WFG_PLAYER_IMAGE` branches), are in
  a firmware database only, so a change there also needs the firmware hand
  run, as clang-tidy's does (below), and it must find nothing and check every
  unit. A firmware database carries the stack check's GCC flags and names
  its compiler for its target (`arm-none-eabi-g++`), which clang-tidy reads
  the target from and clang-query does not, so the check reads it through a
  copy without `-fcallgraph-info=su` and `-fstack-usage` and with
  `--target=arm-none-eabi`, and says so; each `--extra-arg` passes one more
  argument to clang-query, the toolchain's include directories here
  (`$include` as in the clang-tidy hand run):

  ```powershell
  python scripts/check-c-status-truth.py --build-dir build/rt1170-player-release --jobs 8 `
      "--extra-arg=-isystem$include\c++\14.2.1" `
      "--extra-arg=-isystem$include\c++\14.2.1\arm-none-eabi\thumb\v7e-m+dp\hard" `
      "--extra-arg=-isystem$include"
  ```

## Mechanical style

Use Allman braces, four spaces, type-aligned pointers/references, snake_case
functions, PascalCase types, named constants and a 200-column ceiling. Existing
vendor callback spellings are fixed by their ABI. Comments explain purpose,
ownership, units and decisions; avoid narrating obvious syntax.

Private and protected data members use `mCamelCase`; public members, constants
and statics use plain snake_case. Owned identifiers do not start or end with an
underscore. Preserve required SDK spellings, C exports and published interfaces.
The identifier-naming check reports legacy differences as warnings; new code
uses this convention. Review renames rather than blindly applying fix-its.

[`.clang-format`](../.clang-format) is the formatting authority for editors,
the formatting script and CI. Use clang-format 19 for consistent results. The
profile preserves include blocks, sorts includes within each block, and permits
short inline functions. The repository has no dependency on a sibling checkout
or a bundled formatter binary.

```powershell
.\scripts\format-source.ps1
.\scripts\format-source.ps1 -Check
# If the executable has a versioned name:
.\scripts\format-source.ps1 -ClangFormatPath clang-format-19 -Check
```

The script visits owned `.cpp`/`.hpp` files only. It excludes submodules,
generated vendor overlays and C compatibility imports. Formatting is restricted to project-owned C++; dependency sources keep their upstream style.
The [ignore file](../.clang-format-ignore) preserves the generated attenuation
table's exact bytes; `host/attenuation.py` owns that file and its tests check it.

## Active static-analysis gates

[`.clang-tidy`](../.clang-tidy) defines the repository's clang-tidy profile,
including the checks promoted to errors. `scripts/clang-tidy.ps1` runs the
profile against a configured compile database. Use the current build and
validation instructions in [Testing](testing.md); firmware-only code may require
its target compile database.

New and changed code must not introduce findings or hide them through blanket
suppression. Keep necessary vendor/C ABI conversions in documented adapters.
Checks explicitly listed in `WarningsAsErrors` fail the run; other enabled
checks remain warnings until promoted. A new tool version can add checks, so
the configuration, not a historical count, defines the active profile.

### Style and complexity

```powershell
python scripts/check-style.py
python -m pip install lizard==1.24.0
python scripts/check-complexity.py
python scripts/check-project.py --build-dir build/host-release
```

The style checker covers owned C/C++ braces and tracked text line endings.
The complexity gate refuses new functions above CCN 25, growth of existing
exceptions, an average at or above 6, and missing measured functions. Its
`scripts/complexity-baseline.json` also tracks function counts to detect parser
coverage loss. After an intentional removal, rename or reduction, review
`check-complexity.py --update` and its baseline diff; do not use it to hide a
regression. The project-layout check verifies source locations and dependency
inputs, not the behavior of external libraries.

### Host and firmware analysis

Use PowerShell 7 and clang-tidy 19 or later on PATH, after configuring the build:

```powershell
.\scripts\clang-tidy.ps1 -BuildDir build/host-release -Summary
.\scripts\clang-tidy.ps1 -BuildDir build/host-release `
  -Files rt1170/usb_audio.cpp,tests/usb_audio_tests.cpp
```

The script checks owned translation units, deduplicates shared-header findings
and fails on tool errors as well as promoted diagnostics. Headers are checked
through their includers. Host tests cover some firmware code, but cannot reach
all target-only sources or conditional branches. A change to those needs the
corresponding firmware compile database and Arm headers:

```powershell
$armgcc = "$env:USERPROFILE\.mcuxpressotools\arm-gnu-toolchain-14.2.rel1-mingw-w64-x86_64-arm-none-eabi"
$include = "$armgcc\arm-none-eabi\include"
.\scripts\clang-tidy.ps1 -BuildDir build/rt1170-player-release -ExtraArgs `
  "-isystem$include\c++\14.2.1", `
  "-isystem$include\c++\14.2.1\arm-none-eabi\thumb\v7e-m+dp\hard", `
  "-isystem$include" `
  -Files rt1170/main.cpp,rt1170/usb_service.cpp,rt1170/platform/waveform_msc.c
```

Use the actual toolchain path/version and RT1064 database when applicable.
The script filters GCC-only stack-report flags in a temporary database; it does
not modify the configured database. Invoke it from a PowerShell session so
`-Files` and `-ExtraArgs` bind as arrays. Without `-Files`, it checks every owned
unit in that database. Channel-sim has a separate database selected with
`-BuildDir`. Contribution workflow lives in [Contributing](development/contributing.md).

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

Build host and relevant firmware profiles, run applicable tests, formatting and
`scripts/clang-tidy.ps1` on the changed sources (with the firmware analysis above
for a firmware-only source or header), and check that required
dependency files are available. Numerical changes need edge values and
block-size coverage. Ownership changes need cancellation/failure cases. Report
target memory when relevant. Board timing, DMA/cache visibility and analog
behavior require separate physical validation; software tests cannot establish
them. This profile is an engineering standard, not a safety certification.
