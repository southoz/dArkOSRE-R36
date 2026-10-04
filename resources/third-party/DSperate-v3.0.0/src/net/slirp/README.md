Vendored from https://gitlab.freedesktop.org/slirp/libslirp -- libslirp
v4.9.4, commit d09dc9a70360ee7838acccbd234ab83f3b9b37a5.

The user-mode TCP/IP stack behind ../slirp_driver.cpp: the emulated access
point's Ethernet frames go in, and the guest's TCP and UDP connections come
out as ordinary host sockets. It is the only internet transport that works
for us -- the alternative, bridging onto the host NIC, needs CAP_NET_RAW and
cannot work over Wi-Fi at all (a Wi-Fi NIC drops frames from a foreign MAC),
and the handhelds have nothing but wlan0. docs/wifi-scoping.md.

Vendored rather than found for the same reason as ../enet: the two static
handheld tiers (Miyoo A30, RG35XX SP) cannot dlopen and their sysroots have
no libslirp.

Contents are `src/*.c` and `src/*.h` from the tag, unmodified, plus LICENSE
and COPYRIGHT. `libslirp-version.h` is the one generated file -- meson
substitutes it from `libslirp-version.h.in`, and ours was substituted by
hand for 4.9.4. Not vendored: meson files, tests, fuzzing, build-aux.

## The glib shim

Upstream needs GLib. `glib/glib.h` and `glib/glib.c` are ours, not
upstream's: the part of GLib libslirp actually calls (about 25 functions --
allocation, a handful of string helpers, an append-only GString, the log
and assert macros, a PRNG) written against libc. ../CMakeLists.txt puts
`glib/` on the include path so libslirp's own `#include <glib.h>` finds it.

Two things there are deliberate and worth knowing:

- **`g_shell_parse_argv` and `g_spawn_async_with_fds` are stubs that fail.**
  They serve libslirp's `fork_exec`, which runs a command named by a
  `guestfwd` with an `-exec` string. DSperate configures no guestfwd at all,
  so the path is unreachable; a stub that fails cleanly is better than
  pulling process spawning into the build for it. If a guestfwd is ever
  wanted, this is what has to be written first.
- **Allocation failure aborts**, because libslirp is written to GLib's
  contract and never checks a `g_malloc` result.
- **`GLIB_SIZEOF_VOID_P` is not decoration.** libslirp branches on it in
  `cksum.c` (which accumulator loop) and in `ip.h` (whether `struct mbuf_ptr`
  is padded to eight bytes so the overlay over the IP header lines up). A
  missing define is not a compile error and not a link error -- it is a
  32-bit build that drops every IP packet while ARP still works. That is
  what it did, and how it was caught: on the A30, not on the dev box.

## Re-copying

Do not hand-edit the C. Copy `src/*.c` and `src/*.h` from a newer upstream
tag, regenerate `libslirp-version.h`, and update the commit recorded above.
Then rebuild and **run `test_slirp_driver` on a 32-bit tier as well as the
dev box** (the A30 toolchain, under qemu: `toolchains/a30/build-dsperate.sh`
then `ctest` in `b-a30`). A new GLib call shows up as a compile or link
error, which is the point of keeping the shim minimal -- but a *macro* the
shim does not define does not, it just changes what libslirp compiles to,
differently per word size. Build with warnings off (`-w`, as
../CMakeLists.txt does) -- it is third-party C and not ours to fix warnings
in -- and check the link too, since `-w` hides nothing the linker needs and
a missing shim symbol only appears there (`MIN` and `MAX` arrived exactly
that way).

BSD-3-Clause licensed (see LICENSE and COPYRIGHT), compatible with this
project's GPL-3.0-or-later.
