# GitHub-hosted CI

The [`host-ci` workflow](../../.github/workflows/host-ci.yml) selects
GitHub-hosted `ubuntu-24.04` runners for every push, pull request and manual
dispatch, whether the repository is private or public. Public contributors
need no local runner or private credential. A separate Linux job builds
and tests the PathSim channel simulator. Another uses MinGW to cross-compile
the Windows capture tool and its device-free test programs.
The workflow runs for pull requests, pushes to `main`, and manual dispatch.
It requests read-only repository access and uses no device connection. Each job checks out only the public
submodules it needs: TinyUSB and Catch2 for the main host build, and PathSim for
the channel-simulator build.

## Checks

The `style and complexity` job runs the owned-source style, error-style,
bool-return, and complexity gates. The `host build, tests, layout and clang-tidy`
job uses the runner's Linux tools, builds the default host configuration with
encoder plugins disabled, runs CTest, checks project paths, and runs the
clang-tidy and C-status-truth gates. The `PathSim channel simulator build and
tests` job initializes its submodule, applies the retained patches to build-tree
copies, builds with GCC 14, and runs four Linux CTest cases. It checks that the
Linux recipe does not claim parity with the historical Windows binary. The Windows
cross-build job builds
`waveform_capture`, `wfg_capture_core_tests`, and `wfg_winmm_audio_tests` using
the [MinGW toolchain file](../../cmake/windows-mingw-posix.cmake). It checks that
the resulting Windows executables exist. All four jobs must pass before a
change is treated as host-build validated. Linux CTest includes Catch2 cases
and the existing Python and C++ project tests.

The PathSim job covers build and unit behavior, including the gate-script
tests. It does not run the external reference-parity gates, which require
separately supplied lane binaries and recordings. Those comparisons remain an
on-demand maintainer check.

The cross-build does not execute Windows programs. In particular, it does not
run the WinMM fake-driver cases or the capture CLI refusal tests; those need a
native Windows test session. It also does not establish physical sound-card or
USB behavior. Run the Windows host suite and the relevant bench procedure on
demand before making a Windows-runtime or hardware claim.

For the device-free capture checks on Windows, build the host preset and run:

```powershell
ctest --test-dir build/host-tests --output-on-failure --no-tests=error -R '^(wfg_capture_core_tests:|wfg_winmm_audio_tests:|wfg-capture-refusals$)'
```

Contributors can run the shared basic host command after installing its tools
and Python packages:

```text
git submodule update --init --depth 1 third_party/tinyusb third_party/Catch2
cmake --workflow --preset host-tests
```

See [testing](../testing.md#host-tests) for package installation and the
distinction between this basic suite and the additional CI policy gates.

GitHub supplies a clean machine for each job. The host job uses four build
workers and the CTest worker budget in the `host-tests` preset. The jobs use no
hardware and do not flash firmware.
They establish covered host behavior on Linux and Windows compilation; use the
[testing guide](../testing.md) for firmware cross-builds and physical
RT1170/RT1064 evidence.

## Maintainer setup

The jobs install their own GCC 14, Ninja, clang-format/tidy/query 19, MinGW
POSIX x64 and Python packages on GitHub-hosted runners. PowerShell 7 comes
with the hosted image. The account must have available GitHub Actions budget
while the repository is private; a job refused before runner assignment is
not a source or test failure.

Enable GitHub Actions for this repository and allow the workflow to run. After
the first successful run, make the four job names required checks for `main` in
the repository ruleset. Keep the workflow's `contents: read` permission and
do not add secrets for ordinary pull-request validation. Contributors can run
the local equivalents listed in the workflow before opening a pull request.

Before changing this repository to public, remove every self-hosted runner
registration from it. Contributor changes to workflow files must not be able
to schedule jobs on a persistent maintainer machine. Runner registration and
repository access are the security boundary. Keep physical board
qualification in a separately controlled private workflow, using a reviewed
source revision and verified device ownership. Public pull requests need only
the device-free checks above. The workflow does not upload Actions artifacts;
each job builds its own checkout and exposes its outcome in the GitHub run.

This workflow performs continuous integration only. It does not create a
release, upload firmware, or publish the repository. Release packaging and
any continuous-delivery job should be reviewed separately before enabling it.
