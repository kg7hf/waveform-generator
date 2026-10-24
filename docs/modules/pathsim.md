# pathsim channel simulator (vendored) and the channel-sim chain

## Scope and 50,000-foot view

pathsim is an HF ionospheric channel simulator (a Watterson model): it takes an
8 kHz mono WAV and returns it as received over up to three fading, delayed paths
with additive noise. **PathSim is by Moe Wheatley AE4JY, reworked by Vojtech
Bubnik OK1IAK** (<https://github.com/bubnikv/pathsim>). The HF compliance lane
uses it, with its own "rev-6" patch, to make every channel-impaired stimulus.

In this repository pathsim is the lane's reference oracle and the statistical
comparison target for our own channel stages. It runs on the PC only; the
firmware never contains it, and neither the root CMake build nor the firmware
builds need it. The fading channel on the board is signal-lab's own Watterson
stage ([Watterson module](signal-lab-watterson.md)). PathSim remains a host
reference; there is no planned PathSim firmware port.

pathsim is vendored intact, and two programs are built on the PC. The first is
pathsim's own command-line program, as the lane builds it. The second is our
chain driver, which calls pathsim's classes directly and also does the lane's
48 kHz resampling. The optional comparison gates check them against external reference binaries
and stored outputs for the selected toolchain and inputs.

```text
third_party/pathsim (checked-out commit) --export--> build tree --git apply rev-6 patch--> pathsim-src
pathsim-src --git apply pathsim-wfg-embedded.patch (draw order)--> pathsim-chain-src
pathsim-src (all 7 sources) --GCC 14.2 -O3--> pathsim_ref.exe                           gate 0: == lane pathsim.exe 7746121
pathsim-chain-src (6 library sources) + rng/pathsim_rng_shim.hpp (-include) --> pathsim_lib
pathsim_lib + channel-sim/core (driver, resamplers) + cli/ --> pathsim_chain.exe

 8 kHz WAV --pathsim_chain--> 8 kHz WAV        ==  pathsim_ref  ==  lane pathsim.exe          (gate 1)
pathsim's draw sites, host GCC and Arm GCC: first draw -> imaginary part, as the lane's   (gate 1b)
48 kHz WAV --pathsim_chain--> 48 kHz WAV       ==  lane run_pathsim.py chain outputs          (gate 2)
same seed -> same bytes, other seed -> other bytes                                         (gate 3)
```

| Piece | Path |
|---|---|
| Upstream source, pristine | `third_party/pathsim` (Git submodule; Git records its revision) |
| Lane rev-6 patch (verbatim) and its dated notice | `patches/pathsim-b5fa9ae-gcc14-cli-hilbert-onset-seed-paths-truth.patch`, `.notice.md` |
| Chain patch (draw order) and the chain build's dated notice (patch and RNG shim) | `patches/pathsim-wfg-embedded.patch`, `patches/pathsim-wfg-embedded.notice.md` |
| Patched build-tree copies (generated) | `build/main/pathsim-host/pathsim-src` (rev-6), `build/main/pathsim-host/pathsim-chain-src` (rev-6 + chain patch) |
| RNG shim (force-included into pathsim's sources) | `channel-sim/rng/pathsim_rng_shim.hpp`, `rng/ucrt_lcg.hpp`, `rng/pathsim_rng.cpp` |
| Chain driver library (platform-neutral) | `channel-sim/core/` |
| Host CLI `pathsim_chain` | `channel-sim/cli/` |
| Standalone build and unit tests | `channel-sim/CMakeLists.txt`, `channel-sim/cmake/`, `channel-sim/tests/` |
| Gates | `channel-sim/gates/gate0_reference_parity.py` (gate 0), `channel-sim/gates/run_gates.py` (gates 1 to 3), `channel-sim/gates/check_draw_order.py` (gate 1b) |

## How to use it

```powershell
git submodule update --init third_party/pathsim                        # or .\scripts\init-dependencies.ps1 (every submodule)
cmake -S channel-sim -B build/main/pathsim-host -G "MinGW Makefiles"   # or: cd channel-sim; cmake --preset pathsim-host
cmake --build build/main/pathsim-host --parallel 8
ctest --test-dir build/main/pathsim-host --output-on-failure          # from PowerShell, not Git Bash; or .\scripts\test.ps1 -Preset pathsim-host
python channel-sim/gates/gate0_reference_parity.py --build build/main/pathsim-host --lane-root 'C:/reference-data'
python channel-sim/gates/run_gates.py --build build/main/pathsim-host --lane-root 'C:/reference-data'
```

The [hosted Linux job](../development/hosted-ci.md) builds this separate
project and runs its four Linux CTest cases. It has no external reference data,
so the parity gates in the last two commands remain on-demand checks.

Configure selects the lane's WinLibs GCC 14.2.0 when it is installed
(`channel-sim/cmake/winlibs-host.cmake`; override with `-DWFG_WINLIBS_BIN=<bin>`).
With another compiler, generator or platform it still builds, but warns that
parity with the lane's Windows binary is unproven. The build writes
`pathsim_ref.recipe.json` and `pathsim_chain.recipe.json` (compiler, flags and
checked-out submodule revision). On Windows, the compiler's runtime DLLs are
copied beside every program, so another `libstdc++` on `PATH` cannot shadow
them.

### pathsim_chain

`pathsim_chain` takes the patched pathsim's options, by the same names, and
picks its job from the input file:

```powershell
# 8 kHz PCM16 in: pathsim's job, byte-identical to the lane's pathsim.exe
build/main/pathsim-host/pathsim_chain.exe --spread 1.0 --delay2 2.0 --spread2 1.0 --snr 17.6 --seed 60001 in-8k.wav out-8k.wav
# 48 kHz mono in (PCM16, PCM24 or float32): run_pathsim.py's whole chain, 48k -> 8k (-12 dB) -> pathsim -> 48k
build/main/pathsim-host/pathsim_chain.exe --spread 1.0 --delay2 2.0 --spread2 1.0 --snr 17.6 --seed 60001 clean-48k.wav out-48k.wav
```

- Channel: a preset flag (`--ccir-poor`, `--direct`, `--awgn10`, ...; `--help`
  lists them), `--snr` (dB, label semantics below), `--spread`/`--offset`
  (path 1, Hz), `--delay2`/`--spread2`/`--offset2` and `--delay3`/`--spread3`/
  `--offset3` (ms, Hz, Hz), `--seed` (unsigned, default 1), `--truth-out <file>`.
- Files: positional `input output`, or `-i`/`--input_file` and `-o`/`--output_file`.
- Chain options: `--chain auto|8k|48k` (default `auto`: 8000 Hz PCM16 runs
  pathsim, 48000 Hz mono runs the lane chain) and `--resample-gain-db` (the
  48 kHz chain's down-conversion gain, default -12, the campaign's). pathsim's
  own job has no resampler, so `--resample-gain-db` on an 8 kHz input is a
  usage error rather than an option that changes nothing.
- The output and the truth file must not be the input, nor the truth file the
  output. The files are compared by identity (volume and file index on Windows,
  device and inode elsewhere), so any spelling of one file matches: case, `..`,
  a link or a short name. Existing files are compared before any file is
  opened. A truth file not yet written is created, empty, just before the
  output is opened; if the output's name then names it too, it is removed again.
  A match is a usage error and nothing is written. (`pathsim_chain x.wav x.wav`
  used to truncate the input to its 44-byte header, #46; an output `run.wav`
  and a truth file `RUN.WAV`, neither yet written, were one file that both
  writers shared.) A truth file that cannot be created fails before the output
  is written. File names are passed on as the C runtime opens them (the ANSI
  code page on Windows), so a name such as `café.wav` works.
- File names reach the program as written. The WinLibs MinGW runtime expands
  `*` and `?` in a command line before `main()` runs, so the build links the
  runtime's `CRT_noglob.o`, which turns that off: `in-4*.wav` is a name that
  cannot be created, not the `in-48k.wav` it would match and overwrite, and an
  extended-length `\\?\` name is compared and opened like any other.
  `pathsim_ref` keeps the expansion, as the lane's `pathsim.exe` does: it is
  that binary rebuilt, and its PE identity digest is the shipped one's.
- A value follows a space or `=`. Exit status: 0 done, 1 a file or run failed
  (or a critical error: common/host_critical_error.hpp), 2 a usage or channel error. A short summary goes to stderr: samples, blocks,
  seed, active samples, the level seed in dBFS and, for the 48 kHz chain, the
  sample counts and kernel computations.

### The gates

`run_gates.py` and the gate 0 parity check require `--lane-root` to name a separately supplied external reference tree containing the lane binaries and data. That material is not included in this repository; default host tests do not require it. The full comparison is an optional external-data check. It
writes only under `build/main/pathsim-host/work`,
plus `gates.json` and `gates.md` in the build directory. The lane's programs and
analyzers run at below-normal priority, beside the WinLibs runtime, with no
Python bytecode written. `--quick` skips the 1-hour and Appendix E cases (about
3 minutes in all); `--full` adds the two remaining Appendix E runs;
`--gates 1,3` runs a subset (only 1, 2 and 3 are gates of this script: gate 0
is `gate0_reference_parity.py`, and any other value, or none, is a usage error
with exit status 2); `--keep-outputs` keeps the large 48 kHz outputs,
which are otherwise hashed and deleted. Gate 1b needs `arm-none-eabi-g++`
(`--arm-gxx`, else `$ARMGCC_DIR` as the firmware build uses it, `PATH`, or
`~/.mcuxpressotools`); without one it is reported as SKIP, which is not a pass.
It also runs on its own: `python channel-sim/gates/check_draw_order.py --build build/main/pathsim-host`.
Each gate is PASS, FAIL or SKIP; a gate that ran no case is FAIL (0/0), never a
pass. Exit status 0 means every gate passed; 1 that one failed or was skipped. `--allow-skip` (on either script) accepts a skipped gate 1b,
so a host without the Arm compiler can still get exit status 0 from the others.

## Scientist and maintainer view

### What the model does (rev-6)

- Fixed 8 kHz, processed in 2048-sample blocks. A 59-tap Hilbert band-pass
  makes the analytic signal; each path takes an integer delay `int(8 * ms)`
  samples (0.125 ms steps) and equal power `1/sqrt(paths)`. The delay line is a
  ring of `Delay::BUFSIZE` = 2048 + 400 = 2448 samples. Writes and reads
  alternate sample by sample, so every lag from 0 to 2447 samples is exact: any
  delay below 306 ms. Upstream's "50 ms" (`MAXDELAY`) only sized the ring.
  `Delay::add_delay` checks no range. A delay of -0.125 ms or less, or of 306 ms
  or more, indexes outside the ring, so callers must check.
- Each path is multiplied by complex Gaussian noise, low-pass filtered to the
  commanded spread (2 sigma, clamped to 0..30 Hz; below 0.1 Hz there is no
  fading). The noise is made at 12.8, 64 or 320 Hz by the Marsaglia polar method
  on the C library's `rand()` and interpolated x5 per stage up to 8 kHz. An NCO
  adds the frequency offset. In the lane's binary the first draw of each
  uniform pair becomes the imaginary part (see "The RNG shim").
- AWGN goes through the same 59-tap band-pass (noise bandwidth about 3304 Hz).
  The signal is pinned at 0.122 RMS (-18.27 dBFS) and written as PCM16 by
  truncation.
- File mode (the patch): `srand(--seed)` selects the whole realization; the level
  tracker starts from the whole-file RMS of samples with |v| > 1e-4, and the
  fading generators run for 4 s before the first sample.
- SNR is a label, not the delivered value: the lane measured delivered SNR =
  label + 0.42 dB (N0 x 3000 Hz in 300-3300 Hz), and lane scripts command
  target - 0.40 dB. This build keeps that label semantics unchanged.

The patch's changes and reasons are listed in its
[notice](../../patches/pathsim-b5fa9ae-gcc14-cli-hilbert-onset-seed-paths-truth.notice.md)
and in [patches/README.md](../../patches/README.md).

### Vendoring contract

`channel-sim/cmake/pathsim_vendor.cmake` runs at configure and stops on any
mismatch:

1. `third_party/pathsim` is initialised. Local edits in the submodule only
   produce a warning, because they are never used.
2. The checked-out commit's blobs are exported with `git cat-file`, so the working
   tree and line-ending settings cannot change them. Several upstream files are
   stored with CRLF and the patch carries CRLF in those hunks.
3. A patch may touch only exported files. It is applied with `git apply`
   outside any repository; a patch conflict stops configuration.
4. The notice is copied in as `MODIFIED-BY-WAVEFORM-GENERATOR.md`, next to the
   unchanged `LICENSE`, `COPYING` and READMEs. This is the reference copy,
   `<build>/pathsim-src`, which `pathsim_ref` compiles.
5. A second copy gets `patches/pathsim-wfg-embedded.patch` on top, and the chain's notice as
   `MODIFIED-BY-WAVEFORM-GENERATOR-CHAIN.md`. This is `<build>/pathsim-chain-src`,
   which `pathsim_lib` compiles.
6. No library source of the chain copy may take two random draws in one
   statement, because their order would be the compiler's choice. A regular
   expression rejects that at configure time, and gate 1b checks the compiled
   order.
7. Both copies are synced into the build tree. Unchanged files keep their
   timestamps.

To update PathSim, change the submodule and rebase either patch if it no longer
applies. Run the host tests and gates 0 to 3 before claiming parity with the
historical lane binary or data.

### Reference recipe

The lane built `pathsim.exe` (sha256 `77461211...`, 630,355 B) with CMake from
upstream's `CMakeLists.txt`: WinLibs GCC 14.2.0 (x86_64-ucrt-posix-seh, r2),
MinGW Makefiles, Release `-O3 -DNDEBUG`, no `-std` (GCC 14 default gnu++17), no
ISA, warning or floating-point flags, dynamic libstdc++. `pathsim_ref` uses the
same recipe and upstream's source order (`main.cpp` between `GaussFIR.cpp` and
`NoiseGen.cpp`). With the same order the linked image is the same: the two
executables differ only in the PE `TimeDateStamp` and `CheckSum` fields. With
`main.cpp` first, `.text` was 32 bytes longer.

### The chain build

| Target | Sources | Flags |
|---|---|---|
| `pathsim_lib` | pathsim's `Delay`, `GaussFIR`, `NoiseGen`, `Path`, `PathSimParams`, `PathSimProcessor` (chain copy: rev-6 + draw-order patch) | Release `-O3 -DNDEBUG`, `-std=gnu++17`, `-fno-exceptions -fno-rtti -ffp-contract=off`, `-include rng/pathsim_rng_shim.hpp`; pathsim's own warning level, no `-Werror` |
| `pathsim_rng` | `rng/pathsim_rng.cpp` | owned: `-std=c++23` (no extensions), `-Wall -Wextra -Wpedantic -Werror -fno-exceptions -fno-rtti -ffp-contract=off` |
| `channel_sim` | `core/*.cpp`, `common/status.cpp` | owned |
| `pathsim_chain` | `cli/*.cpp` | owned |
| `channel_sim_tests`, `wav_file_tests`, `ucrt_rand_tests` | `tests/*.cpp` (`wav_file_tests` also `cli/wav_file.cpp`) | owned |

Owned code converts between sample counts, signed sample indices and doubles
only through `core/convert.hpp` (`as_double`, `to_signed_index`,
`to_unsigned_index`, `floor_to_index`). Those helpers are the only casts in the
core's arithmetic. Counts stay at or below 2^56, which `LaneResampler::push`
enforces, so each conversion is exact, and the index conversions saturate
rather than wrap.

pathsim's library sources build without exceptions and RTTI. Only `main.cpp`
needs them (cxxopts throws and uses `dynamic_cast`), and the chain does not
compile `main.cpp`, `AudioFile.h` or `cxxopts.h`. They need a GNU dialect:
under strict `-std=c++23`, `Path.cpp` loses `M_PI`, because its
`_USE_MATH_DEFINES` comes after `<math.h>` is already included. pathsim's
headers reach owned code as system headers, so their style does not trip the
owned warnings. `-ffp-contract=off` changes no instruction on x86-64, whose
baseline ISA has no FMA, but the Cortex-M7 has VFMA, so it is set now.

### The RNG shim

The lane's `pathsim.exe` imports `rand` and `srand` from the Universal C Runtime
(`api-ms-win-crt-utility-l1-1-0.dll`). The UCRT keeps a 32-bit state per thread:
`srand(s)` sets it to `s`, and `rand()` steps `state = state * 214013 + 2531011`
(mod 2^32) and returns `(state >> 16) & 0x7FFF`. Other C libraries, such as
glibc and newlib (the RT1170's), have different generators and
`RAND_MAX = 0x7FFFFFFF`. A build that used one would draw other fading and noise
for the same seed.

`rng/ucrt_lcg.hpp` implements the UCRT generator as `UcrtLcg`. A `static_assert`
checks the first ten values after seed 1 (41, 18467, 6334, 26500, 19169, 15724,
11478, 29358, 26962, 24464), the Microsoft runtimes' published sequence.
`ucrt_rand_tests` compares 100,000 draws for each of 12 seeds (0, 1, 2, 3, 5,
31, 99, 60000, 60001, 0x12345678, 0x80000000, 0xFFFFFFFF) with this host's
`rand()`, and they agree value for value.

The shim redirects pathsim's draws without editing pathsim. pathsim calls
`rand()` unqualified inside `namespace PathSim` (`Path.cpp`, `NoiseGen.cpp`).
The shim, force-included before any pathsim code, declares `PathSim::rand()`.
Ordinary name lookup finds that declaration first, so every draw steps the
generator in `rng/pathsim_rng.cpp`. No macro renames `rand`. The shim's only
macro replaces `RAND_MAX` with `0x7FFF` when the C library defines another
value, as glibc and newlib do. The Microsoft runtimes already define `0x7FFF`,
so on the host it changes nothing, and a `static_assert` pins it.
`seed_pathsim_rng(seed)` replaces `srand(seed)`, which pathsim calls only in the
`main.cpp` the chain does not compile.

A post-build step, `cmake/check_pathsim_rng.cmake`, runs `nm` on `pathsim_lib`.
It fails the build unless the library references `PathSim::rand()`
(`_ZN7PathSim4randEv`, today from `Path.cpp.obj` and `NoiseGen.cpp.obj`) and
references neither `rand` nor `srand`. If a later pathsim called `::rand()`, its
draws would leave the shim silently; this check catches that. A unit test also
checks that a driver run leaves the C runtime's own `rand()` sequence untouched.

The generator is process-wide, like the C library's. One job at a time owns it:
the driver seeds it immediately before pathsim's `init()`, and nothing else may
draw until the job ends.

The shim edits no pathsim file, but it changes how pathsim's compiled code
behaves. So it is a modification under GPLv3 section 5a, and its dated notice is
`patches/pathsim-wfg-embedded.notice.md`, section 1.

**Draw order.** The same numbers are not enough on their own: each must also
reach the same variable. rev-6 draws each complex pair as
`v.set(rsample(), rsample())` (`Path.cpp`, `Rayleigh::sample`; `NoiseGen.cpp`,
`add_band_limited_noise`). C++ leaves the order of a call's arguments
unspecified, and GCC fixes it per target when it lowers the call:

| Compiler | rev-6: the first draw goes to | Effect |
|---|---|---|
| GCC 14.2 x86-64 (the lane's `pathsim.exe`) | the imaginary part | the reference realization |
| arm-none-eabi-g++ 14.2.1, Cortex-M7 (the firmware) | the real part | every pair swapped: each Rayleigh tap becomes `j*conj(h)` (same envelope, mirrored phase), and the AWGN samples swap in pairs. With the same seed, this is a different realization, not a last-bit difference. |

`patches/pathsim-wfg-embedded.patch` (2026-10-03) writes the lane's order out:
`const double i_draw = rsample(); const double r_draw = rsample(); v.set(r_draw, i_draw);`
at both sites. The chain copy is built from that patch, and `pathsim_ref` stays
on rev-6 alone. Three checks cover it:

- Gate 1: on x86-64 the chain's 8 kHz output is byte-identical to `pathsim_ref`
  and to the lane's binary, so the patch changes nothing there.
- Gate 1b (`check_draw_order.py`): GCC's GIMPLE dump of both copies, with the
  host compiler and with arm-none-eabi-g++ for the Cortex-M7, shows which
  argument the first draw reaches. rev-6 is swapped on Arm, and the chain copy
  matches the lane on both compilers.
- `channel_sim_tests`: the first AWGN pair and the first Rayleigh tap after
  `init` for seed 60001 are computed from the UCRT stream in the lane's order.
  The test is portable C++, so it checks the order under any compiler that
  builds the chain. With the order swapped on purpose (a negative check of the
  test), its four draw-order checks fail.

### The 8 kHz driver (core/pathsim_driver)

`PathsimDriver::run(job, source, sink)` is the patched `main.cpp` file mode,
streamed. It calls pathsim's own classes in the same order, so the random draws
and every floating-point operation happen in the same sequence:

| Step | main.cpp | Driver |
|---|---|---|
| Level pre-pass | `acc += v*v` over samples with `fabs(v) > 1e-4`, `v = s / 32768`, whole file | pass 1 over the source, same arithmetic, then `rewind()` |
| Seed | `srand(seed)` (default 1) | `seed_pathsim_rng(seed)` |
| Init | `PathSimProcessor::init(params)` (draws: each fading path's filter preload) | same, on a fresh processor |
| Truth export | `enable_truth_capture(path, seed)` if `--truth-out` | same, if `job.truth_path` is set |
| Level seed | `seed_signal_rms(sqrt(acc / active))` if any sample was active | same |
| Warm-up | `warm_up_fading(4.0)` | same |
| Blocks | pad to whole 2048-sample blocks with 0.0; `process_buffer` per block | pass 2, block by block, the last one zero-padded |
| Output | `clamp(y, -1, 1)`, `int16(y * 32767)` (truncation), input length only | `unit_to_pcm16_truncated`, each block's real samples only |

`ChannelSpec` holds the channel without any pathsim type and is built with
`main.cpp`'s option semantics. A preset replaces everything and comes first.
`--snr` turns AWGN on at that label. A path option sets one field of its slot.
Addressing a slot that does not exist yet adds default paths (static, 0 ms) up
to it, so `--spread3 1` alone makes three paths. `validate()` rejects what
pathsim would clamp silently or index out of bounds. Otherwise it accepts what
`pathsim.exe` runs:

| Value | Accepted | pathsim without the check |
|---|---|---|
| Spread | 0 to 30 Hz | clamps to 0..30 Hz |
| Delay (paths 2 and 3) | 0 to below 306 ms (lag `int(8 * ms)` = 0 to 2447 samples; the last 0.125 ms step is 305.875 ms) | -0.125 ms or less, or 306 ms or more, indexes outside the 2448-sample delay ring. Negative delays between -0.125 and 0 ms act as 0 in pathsim; the chain refuses them. |
| Offset | -4000 to 4000 Hz | any value |
| SNR label | finite, within +/-150 dB | any value |

The delay limit comes from pathsim's `Delay::BUFSIZE`; `channel_spec.cpp`
`static_assert`s the number against pathsim's header. Gate 1 runs 60 ms and
305.875 ms delays byte-identical to the lane's binary.

The sources and sinks (`core/sample_stream.hpp`) know no file format. A source
returns up to the requested count (0 means the end) and can be rewound to
deliver the same samples again, which pathsim's whole-file level seed needs.
The driver fails if the second pass is not as long as the first. The host CLI
wraps WAV files (`cli/wav_file.*`).

Memory: the driver's own buffers are fixed arrays, 2048 doubles and 2048
`int16` (20 KB). pathsim's processor sits in place in the driver (about 40 KB,
mostly the 2448-sample complex delay line). pathsim allocates the rest from the
global heap:

- `init()`: a 2048-sample complex buffer per path (32 KB each, 96 KB for three
  paths), each fading path's Gaussian filter (at most about 225 taps, 7 KB) and
  the parameter copies.
- `process_buffer()`: a small vector of path pointers on every block, freed at
  the end of the block.
- The truth export, a host diagnostic: three more buffers per path and a record
  buffer of about 330 KB per block for three paths.

pathsim runs on the host only, so that is the ordinary heap. Owned code makes
no allocation of its own. The standard library's file streams (`cli/wav_file.*`,
the truth file's creation and check) allocate their buffers, and the truth path
is copied into a `std::string` for pathsim's API. The CLI's file identity check
uses the platform's own calls (`CreateFileA` and `GetFileInformationByHandle`,
or `stat`), not `std::filesystem`, whose path conversion of a name such as
`café.wav` in the ANSI code page ended the program.

### The 48 kHz chain (core/lane_resampler, core/lane_chain)

`run_pathsim.py` (lane packet 3a) runs, for one 48 kHz trial:

1. `m110_wav_resample in.wav clean-8k.wav 8000 -12`: 48 kHz to 8 kHz at -12 dB, PCM16 rounded.
2. The campaign's PCM16 rewrite (`trial-in.wav`); no sample changes.
3. `pathsim.exe <options> trial-in.wav trial-chan.wav`.
4. The rewrite again (`trial-out-8k.wav`, `keep_from = 0`).
5. `m110_wav_resample trial-out-8k.wav out.wav 48000`: 8 kHz to 48 kHz at 0 dB, PCM16 rounded.

`m110_wav_resample` (snapshot wave3-final, sha256 `e9268ae6...`) is built from
the lane's `compliance/app_decode/wav_resample_fast.cpp`. That file is a
multi-threaded rebuild of the reference `wav_resample.cpp`, and the lane checked
the two byte for byte. For output sample `i` of `N` inputs:

```text
p = i * (in / out)          c = floor(p)        taps j = c - 95 .. c + 96 with 0 <= j < N
d = p - j                   u = d / 96          skip |u| >= 1
fc = 0.45 * min(1, out / in)
w = 2 fc * sinc(2 fc d) * (0.42 + 0.5 cos(pi u) + 0.08 cos(2 pi u))     sinc(x) = sin(pi x) / (pi x), 1 for |x| < 1e-12
y = sum(x_j w) / sum(w)  (0 when |sum(w)| <= 1e-12)        y *= gain = 10^(dB / 20)
PCM16 = lround(clamp(y * 32767, -32768, 32767))           outputs: llround(N * out / in)
```

Inputs are read as `s / 32768` (PCM16), `s / 8388608` (PCM24) or float32 as it
is. `LaneResampler` evaluates exactly these operations, in this order, in
double. On the same C runtime (UCRT `sin`, `cos`, `floor`, `lround`, `llround`
and `pow`) it is therefore bit-identical for finite input. Two facts make it fast
without changing a bit:

- `d = fl(p - j)` equals `fl(f - t)` with `f = p - c` (exact by Sterbenz's
  lemma) and `t = j - c`, because both round the same real number. The 192
  weights and their in-order sum therefore depend only on the exact double `f`,
  and they are cached by it. Downsampling has a single `f` (0). Upsampling has
  six phases plus a few rounding variants: the golden case needed 45 kernel
  computations for 508,800 outputs, and a 3-hour Appendix E run 67 for
  518,520,000, instead of 192 x 3 transcendental calls per output.
- A tap of weight +0.0 leaves both sums unchanged, as a skipped tap does. Neither
  sum can be -0, and `s + 0 = s` otherwise. Interior outputs therefore run all
  192 taps without branches. Outputs whose window crosses either end of the
  input skip the missing taps, as the lane tool does.

The second fact needs finite samples. `0 * inf` is NaN, where the lane tool
skips the tap and keeps a value. So `LaneResampler::push` refuses inf and NaN,
and a float32 WAV that contains them fails the job before any output. Such a
file is not audio, and the lane tool's own result for it depends on which taps
the inf falls under.

The resampler streams. It keeps the last 256 inputs, and the caller takes every
ready output before pushing the next input. `finish()` fixes the output count.
`LaneDownsampler` is a rewindable PCM16 source over a 48 kHz unit-scaled source;
`LaneUpsampler` is a PCM16 sink that writes 48 kHz downstream. `LaneChain` joins
them around the driver. pathsim's level pre-pass rewinds the downsampler, which
rewinds the 48 kHz source, so the input is read and downsampled twice and
nothing is stored between the stages. A unit test checks the resampler bit for
bit against a direct transcription of the lane tool's loop, in both directions,
for 13 lengths around the filter's edges (1 to 20,003 samples at 8 kHz, six
times as many at 48 kHz) and for any chunking of the stream.

Memory: each resampler holds 2 KB of history and a 16-entry kernel cache
(25 KB). The chain object is about 130 KB in place, plus pathsim's heap use
above.

### Repeating the comparison

Run the gates described above against an explicitly supplied external reference
tree. The generated gate reports record the current binaries, commands and
comparisons under the build directory. Historical results are not a substitute
for rerunning after a toolchain, dependency or patch change. See
[testing](../testing.md#external-reference-comparisons).

### Known differences from the lane programs

- The chain CLI is stricter than pathsim's cxxopts parser. pathsim ignores
  unknown options and takes the last of a repeated option; `pathsim_chain`
  refuses both. It also refuses the out-of-range values listed above. Seeds are
  decimal only.
- The 8 kHz job requires an 8000 Hz PCM16 WAV, mono or stereo. pathsim also
  accepts 8- and 24-bit files, and files at any rate, which it would treat as
  8 kHz anyway. As in pathsim, channel 0 is processed and copied to every
  output channel.
- The 48 kHz job requires 48000 Hz mono, as `run_pathsim.py` does, in PCM16,
  PCM24 or float32 (format tag 3; the lane resampler also rejects
  `WAVE_FORMAT_EXTENSIBLE`).
- WAV parsing walks the chunks and needs `fmt ` before `data`. pathsim's
  AudioFile instead searches the file for the first `fmt`/`data` byte strings.
  The two agree on every file the lane writes. Chunk sizes, with their pad byte,
  are checked in 64 bits against the file's length. A chunk that runs past the
  end of the file is refused, including the 0xFFFFFFFF "unknown length" that
  some streaming writers emit. So is a data chunk longer than the file
  (`wav_file_tests`).
- A float32 input with inf or NaN samples fails the 48 kHz job (see the
  resampler above). The lane tool would still write a file.
- pathsim ignores a truth file it cannot open; `pathsim_chain` then fails.
- The 8 kHz job is pathsim's processing with leaner WAV I/O, so it is at least as
  fast as `pathsim.exe`. The 48 kHz chain runs the resamplers in one thread with
  cached kernels. Measure throughput on the intended host and workload; old elapsed times
  do not establish a performance bound.
- Bit-exactness holds on the host toolchain (WinLibs GCC 14.2.0, UCRT). pathsim
  itself calls `sincos`, `exp`, `log`, `pow`, `fmod` and `sqrt`; the resampler
  calls `sin`, `cos` and `pow`. Another C library rounds some of those
  differently in the last bit, so a build with another C library (newlib, for
  example) is expected to be statistically equal, not bit-equal. Two things
  keep the realization itself the lane's: the RNG shim (the same random
  numbers) and the draw-order patch (each number in the same place).

### Failure diagnosis

| Symptom | Cause |
|---|---|
| Configure: "not initialised" | Run `git submodule update --init third_party/pathsim`. |
| Configure: patch does not apply cleanly | The checked-out submodule and patch disagree. Rebase the patch, keep its dated notice accurate, and rerun tests and comparison gates. |
| Configure warning: compiler, platform or flags differ | The build is usable, but gate 0 parity is not established for it. |
| Build: "check_pathsim_rng: ... references the C runtime's rand" | A pathsim source draws from `::rand()`: the shim no longer covers it. Extend the dated chain patch (`patches/pathsim-wfg-embedded.patch`) and its notice. |
| Configure: "takes two random draws in one statement" | A pathsim revision added an unsequenced draw pair. Sequence it in the lane's order in the chain patch. |
| Gate 1b: FAIL, or a chain draw site that reaches the real part first | The chain patch no longer covers a draw site, or the parser found no site (a pathsim change). Compare the GIMPLE dumps under `work/draw-order`. |
| `ucrt_rand_tests` fails | The host C runtime is not a Microsoft one, or its generator changed. Gate 1 parity with the lane's binary is then unproven. |
| Gate 0: shipped sha256 differs | The snapshot's `pathsim.exe` is not the lane's 7746121; the scripts refuse to run. |
| Gate 0 or 1: outputs differ | Compare the recipe JSON files with the recipes above. Then check the runtime DLL hashes in the report, the compiler version, flags (FP contraction, `-march`) and the C runtime (`rand`, libm). |
| Gate 2: a 48 kHz output differs | `gates.json` gives the largest difference in LSB and the difference energy. A 1-LSB scatter points at a libm or rounding difference in the resampler; a large difference points at the options, the gain or the input. |
| Another toolchain: full-scale differences from the host oracle for the same seed | Not libm. Check the draw order first (gate 1b, the draw-order unit test built with that toolchain), then the RNG shim (`check_pathsim_rng`). |
| `pathsim_chain`: "the source delivered a different length on its second pass" | The input changed while the job ran. |

## Licence and credits

pathsim is GNU GPL v3 (`LICENSE`, `COPYING`). `FilterTables.h` is (c) 2000 Moe
Wheatley AE4JY under GPL v2 or later. `AudioFile.h` (GPLv3 or later) and
`cxxopts.h` (MIT) are compiled only into `pathsim_ref`, the host reference CLI.
All of these are compatible with waveform-generator's GPL-3.0-only licence.
Every licence file and header is kept unchanged. Every modification has a dated
notice in `patches/`, and each build-tree copy carries the notices for what was
applied to it:

| Modification | Applies to | Dated notice | In the copy as |
|---|---|---|---|
| rev-6 patch (the lane's, verbatim) | `pathsim-src`, `pathsim-chain-src` | `pathsim-b5fa9ae-...-truth.notice.md` (the patch itself cannot carry a header) | `MODIFIED-BY-WAVEFORM-GENERATOR.md` |
| `pathsim-wfg-embedded.patch` (draw order, 2026-10-03) | `pathsim-chain-src` | its own header, and `pathsim-wfg-embedded.notice.md` section 2 | `MODIFIED-BY-WAVEFORM-GENERATOR-CHAIN.md` |
| RNG shim, force-included (2026-10-03): `rand()` bound to `PathSim::rand()`, `RAND_MAX` pinned to `0x7FFF` where it differs, `srand` replaced by `seed_pathsim_rng()` | `pathsim_lib` objects | `pathsim-wfg-embedded.notice.md` section 1 | `MODIFIED-BY-WAVEFORM-GENERATOR-CHAIN.md` |

The chain driver, resamplers and CLI are waveform-generator's own code. The
repository (the Git-recorded submodule revision, the patches and `channel-sim/`) is the
corresponding source. See [THIRD_PARTY_NOTICES.md](../../THIRD_PARTY_NOTICES.md).

## 5th-grader view and worked example

pathsim is a pretend radio sky. You hand it a recording, and it plays it back
as if it had bounced off the ionosphere: two copies arrive a moment apart, each
one fading in and out, with hiss on top. It decides how the sky behaves by
rolling dice, and the seed says which dice rolls to use.

We wrote our own conductor for pathsim's orchestra. It calls the same players
in the same order, and it brought its own dice that roll exactly like the
lane's Windows dice. pathsim rolls two dice at a time, one for "left" and one for
"right". The Windows build happened to read the second die first, and the
board's compiler reads them the other way round, so we wrote down the order
explicitly. The conductor also does the lane's "shrink to 8 kHz and grow back
to 48 kHz" steps itself. Given the same recordings and the same seeds, the
conductor, pathsim's own program and the lane's program all produced the same
files, down to the last byte.

Toy example: in the golden case the second path arrives 2.0 ms later, which is
`int(8 * 2.0)` = 16 samples at 8 kHz. Each path gets half the power
(`1/sqrt(2)` in amplitude). The signal is pinned at 0.122 RMS, which is
20 log10(0.122) = -18.27 dBFS. The label `--snr 17.6` delivers about
17.6 + 0.42 = 18.02 dB, which is the lane's 18 dB target after its -0.40 dB
offset. All three programs wrote the same 169,644-byte 8 kHz WAV, sha256
`cc56fd3b...`. From the 508,800-sample 48 kHz source, the chain wrote the
lane's 1,017,644-byte 48 kHz WAV, sha256 `82151737...`.

The resampler toy: down to 8 kHz, output `i` sits exactly on input `6i`
(`p = 6i`, `f = 0`). So one set of 192 weights serves every interior output,
and the -12 dB gain (x 0.2512) is applied after the weighted average.

## Source map

| File | Role |
|---|---|
| `channel-sim/CMakeLists.txt` | Standalone project: `pathsim_ref` and its recipe check, the chain targets, the tests |
| `channel-sim/cmake/pathsim_vendor.cmake` | Export checked-out submodule sources, apply both patches to build-tree copies, check draw pairs, sync |
| `patches/pathsim-wfg-embedded.patch`, `.notice.md` | The chain's draw-order patch; the dated notice for it and the RNG shim |
| `channel-sim/cmake/winlibs-host.cmake` | Selects the lane's WinLibs GCC when it is installed |
| `channel-sim/cmake/check_pathsim_rng.cmake` | Post-build: pathsim's draws reference `PathSim::rand()` only |
| `channel-sim/CMakePresets.json` | `pathsim-host` preset (`build/main/pathsim-host`) |
| `channel-sim/rng/pathsim_rng_shim.hpp` | The force-included shim: `PathSim::rand()` declaration, `RAND_MAX` |
| `channel-sim/rng/ucrt_lcg.hpp`, `pathsim_rng.hpp`, `pathsim_rng.cpp` | The UCRT generator, its seed function and `PathSim::rand()` |
| `channel-sim/core/units.hpp`, `pcm.hpp`, `sample_stream.hpp` | Named units, the two PCM16 writers, the stream interfaces |
| `channel-sim/core/convert.hpp` | The core's count, index and double conversions (its only arithmetic casts) |
| `channel-sim/core/channel_spec.*` | The channel, with pathsim's CLI semantics and the range checks |
| `channel-sim/core/pathsim_driver.*` | The 8 kHz job (main.cpp's orchestration) |
| `channel-sim/core/lane_resampler.*` | The lane's resampler, streamed, with its source and sink adapters |
| `channel-sim/core/lane_chain.*` | The 48 kHz chain |
| `channel-sim/cli/pathsim_chain.cpp`, `options.*`, `wav_file.*` | The host CLI, its options and its WAV streams |
| `channel-sim/tests/channel_sim_tests.cpp`, `wav_file_tests.cpp`, `ucrt_rand_tests.cpp` | Unit tests (ctest) |
| `channel-sim/tests/test_pathsim_chain_cli.py`, `test_gate_scripts.py` | The built CLI's refusals and options; the gate scripts' verdicts and arguments (ctest; the gate tests fail, not skip, without numpy) |
| `channel-sim/gates/gate0_reference_parity.py`, `run_gates.py`, `check_draw_order.py` | Gate 0; gates 1 to 3; gate 1b |
| `third_party/pathsim/PathSimProcessor.cpp` | Block processing: Hilbert, delays, paths, sum, level and AWGN |
| `third_party/pathsim/Path.cpp` | Rayleigh generator, x5 interpolators, NCO |

Reading order for the chain: `sample_stream.hpp`, `channel_spec.hpp`,
`pathsim_driver.cpp` beside the patched `main.cpp`, then `lane_resampler.hpp`
beside the lane's `wav_resample.cpp`, then `lane_chain.cpp`, and
`pathsim_rng_shim.hpp` with `patches/pathsim-wfg-embedded.patch` for the draws.
