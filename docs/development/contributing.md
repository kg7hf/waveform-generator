# Contributing

## Pull request workflow

Fork the repository, create a branch for one focused change, and open a pull
request against `main`. Run the [basic host suite](../testing.md#host-tests)
before submitting. Include the commands and outcomes in the pull request, and
call out any checks you could not run. You do not need an RT1170 or RT1064 board
to contribute; maintainers handle physical qualification when a change needs
it. The public [GitHub-hosted CI](hosted-ci.md) checks pull requests without
access to private hardware or credentials.

## Change scope

Keep a change focused on its feature. As a separate improvement, choose a modest
existing utility, test suite or code area outside the feature work and leave it
better when practical. Do not turn that improvement into a broad retrofit.
Record completed work and deferred follow-up in the change description.

New C++ tests use Catch2. When changing an existing hand-written test suite,
migrate one suite without dropping checks or changing their order. Keep raw
test evidence under ignored `build/` or `artifacts/` directories and summarize
the checks that were run.

## Validation and evidence

Use the current commands and scope in [Testing](../testing.md). Distinguish
source review, host tests, cross-builds and physical board evidence. A passing
host build does not establish board behavior. Keep retained bench artifacts
with the run that produced them; do not replace failed evidence with a later
successful run.

## Code requirements

The [embedded C++ and documentation standard](../coding-standard.md) defines
implementation constraints. The subsystem guides under [`docs/modules`](../modules/README.md)
own their API contracts and operator procedures. Update the owning guide when a
public command, interface or acceptance boundary changes.
