# Vendored rcheevos

Upstream: <https://github.com/RetroAchievements/rcheevos>, branch `master`,
commit `2ad0b8672f68a48148620164510b963039e49eb1` (2026-07-23), version 12.4.0.
Taken verbatim: `include/`, `src/`, `LICENSE`. The only files dropped are the
two `.natvis` files (Visual Studio debugger views).

Nothing here is modified. Keeping it a clean snapshot is the point -- the
achievement logic has to agree with the server's, and a local edit to the
condition evaluator or the hash is a bug we would never find. Anything we need
to do differently is done in the layer above (`../cheevos_hash.cpp`), through
the callbacks rcheevos provides for exactly that.

## What is compiled, and what is only vendored

Phase 1 (`docs/retroachievements-scoping.md`) is identity only -- the
RetroAchievements hash of a ROM, no session and no network -- so
`../CMakeLists.txt` builds just:

| file | why |
|------|-----|
| `src/rhash/hash.c`, `hash_rom.c` | `rc_hash_nintendo_ds` and its dispatch |
| `src/rhash/md5.c` | the hash itself |
| `src/rc_util.c`, `src/rc_compat.c`, `src/rc_version.c` | what those need |

Compiled with `RC_HASH_NO_DISC`, `RC_HASH_NO_ENCRYPTED`, `RC_HASH_NO_ZIP` and
`RC_DISABLE_LUA`, which drops `cdreader.c`, `aes.c`, `hash_disc.c`,
`hash_encrypted.c` and `hash_zip.c`. Those are for consoles we will never ask
about. `RC_HASH_NO_ZIP` costs nothing even though DSperate does play zipped
ROMs: by the time rcheevos is involved, `cart/zip.cpp` has already produced the
ROM bytes and rcheevos is reading them through a `RomSource`, so it never sees
an archive.

The rest -- `src/rc_client.c`, `src/rapi/`, `src/rcheevos/` -- is vendored but
not built. Phase 2 adds `src/rcheevos/` (the runtime that evaluates
achievements against memory) and phase 3 adds `rc_client.c` and `rapi/` (the
session and the server protocol). They are here now so that the snapshot is one
upstream commit rather than three.

Not vendored for use at all: `src/rc_client_raintegration.c` (the Windows-only
achievement authoring toolkit) and `src/rc_libretro.c` (libretro's own
integration).

## Updating

Re-copy the three paths above from a new upstream tag and re-run the tests.
`tests/cheevos_hash_test.cpp` checks the hash against golden values derived
from the documented algorithm rather than from rcheevos, so an upstream change
that altered the DS hash would fail the build rather than silently orphan every
player's achievements.
