# Profile-guided optimisation data

`aarch64/` holds the GCC profile (`.gcda` files, one per object, plus a
`MANIFEST`) that CI and any `-DDSPERATE_PGO=use` build apply.
It is produced by `tools/pgo_refresh.sh` on a machine that has the ROMs, BIOS
and firmware the training runs need, so the public build never does.

`aarch64-gcc<version>/` holds a *secondary* profile for one other compiler.
The configure prefers it whenever a build uses that exact compiler and falls
back to `aarch64/` otherwise, so a downstream distro on another GCC still gets
PGO instead of quietly dropping to an unprofiled build (dArkOS packages
DSperate with GCC 12.4; CI builds with 13.3). Make one with `--gcc N`, and
refresh it in the same pass as the default profile.

The profile is valid for one compiler version and one set of compile flags
(`MANIFEST` records a fingerprint; the configure step refuses a mismatch) and
decays gracefully with source changes: a function whose body changed loses its
profile, the rest keep theirs. Refresh before a release, or whenever
`tools/pgo_refresh.sh --check` reports drift in hot files:

    DS_ROMS=... DS_BIOS=... tools/pgo_refresh.sh     # ~2 minutes, then commit pgo/

File names are relative to the build directory (`-fprofile-prefix-path`), so
any build tree layout finds them.
