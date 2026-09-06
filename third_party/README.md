# Materialized dependencies

This directory contains the source snapshots used by the standalone build. The
build never searches the parent M110 checkout for missing files. Run
`scripts/init-dependencies.ps1` with explicit, verified source checkouts to
materialize the six dependencies locked for the standalone fixture.

The revised Phase 1.2 microSD slice uses selected USDHC/adapter files from the
MCUX SDK plus selected SDMMC and FatFs files. Their exact MCUX_2.16.100 commits,
licenses, selected paths, byte sizes, and hashes are recorded in
`dependencies.lock.json`; `materialized-files.json` covers the complete local
third-party byte set. Generator-owned board adaptation and FatFs configuration
remain outside this directory.
