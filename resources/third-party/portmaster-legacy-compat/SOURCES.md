# PortMaster legacy FFmpeg compatibility libraries

This directory preserves the exact Debian binary packages used to populate
the R36S compatibility overlay in `files/ROOTFS/usr/lib`. Each package is
available for AArch64 and ARMhf. `SHA256SUMS` covers all 44 downloaded `.deb`
files; `copyright/` contains each package's Debian copyright/licensing file.

The original 21-package set follows vanilla dArkOS `fetch_compat_libs.sh` at
firmware commit `1dac9ef`, with the failed ARMhf `libjpeg8` pool path
corrected. One direct FFmpeg dependency, `libwebpmux3`, is also pinned for
each ABI in the evidence store, but only the ARMhf object is installed in the
R36 overlay. The `libavcodec.so.58` objects have a direct `DT_NEEDED` entry
for `libwebpmux.so.3`; the old ARMhf overlay had `libwebp.so.6` but omitted
its mux library. The current device has a valid newer AArch64
`libwebpmux.so.3` (`3.1.1`), so the legacy AArch64 package is retained only
for provenance and must not be installed over it. `libwebpmux3` package
version `0.6.1-2.1+deb11u2` depends on
`libwebp6 (>= 0.5.1)` and `libc6 (>= 2.29)`. The paired existing Debian
`libwebp6` package is the same `0.6.1-2.1+deb11u2` version and provides the
required `.so.6`; its direct ELF dependencies are `libm.so.6`,
`libpthread.so.0`, libc, and the ARMhf loader, all supplied by the target
system. No floating APT transaction or system upgrade is introduced.

These old FFmpeg 4.3 SONAMEs are required by legacy PortMaster runtimes. This
is a compatibility layer, not a system FFmpeg upgrade. The files are
installed in the standard architecture library directories, matching
vanilla's approach.

One additional SONAME alias is included for both ABIs:
`libsrt-gnutls.so.1.4 -> libsrt-gnutls.so.1.4.2`. Debian's package contains the
versioned object while the old `libavformat.so.58` requests the unversioned
SONAME filename.

## Source and upstream

- Vanilla helper: [`fetch_compat_libs.sh`](https://github.com/christianhaitian/dArkOS/blob/1dac9ef/fetch_compat_libs.sh)
- Debian packages: see the exact `.deb` files, package versions, checksums,
  and copyright notices in this directory.
- Added `libwebpmux3` packages:
  - `libwebpmux3_0.6.1-2.1+deb11u2_armhf.deb`, SHA-256
    `9a4107c7316465cae6577917feb506d852dd670f97e71762d61fb088e5ec0c11`.
  - `libwebpmux3_0.6.1-2.1+deb11u2_arm64.deb`, SHA-256
    `d51a27de565b736ba5a4a443399beb692d5aaf0c488b7f014c94f5e5c7f5bf31`.
  - Both hashes and package metadata match the Debian bullseye `Packages.xz`
    indices; the same hashes are present in bullseye-security. The exact
    source package is `libwebp_0.6.1-2.1+deb11u2`. Only the ARMhf library and
    SONAME link are added to the overlay; the AArch64 package is evidence
    only because the device has a working newer library.
  - Extracted ARMhf `libwebpmux.so.3.0.1` SHA-256:
    `247b24116480e9323e0c1d4870208aad3412d8b7bf5f4aad5fc3c4c48ed84cfc`.
    Its overlay link is `libwebpmux.so.3 -> libwebpmux.so.3.0.1`.
- Debian source packages can be retrieved from the corresponding Debian
  archive/snapshot source suites using the source package named in each
  `.deb` control record. The source code is not modified in this R36 port.
- FFmpeg library copyright and license notices are included in
  `copyright/libavcodec58.copyright`; all other component notices are kept
  beside it under `copyright/`.

## Device validation

### Clean official-image loader limitation (2026-10-03)

The WebP mux addition closes the observed missing `libwebpmux.so.3` dependency
on the already-updated physical device; it does **not** make the complete ARMhf
legacy FFmpeg library set self-contained on a clean official R36 base. The
official-image QEMU loader inventory at
`assets/upstream/r36-official-review/qemu-loader-results.json`, interpreted
alongside the base package manifest at
`assets/upstream/r36-official-review/packages.tsv`, still reports ARMhf
dependencies with no matching ARMhf counterpart in the base; the loader reports wrong-class fallback errors for:
`libzvbi.so.0`, `libgme.so.0`, `libva-drm.so.2`, `libgcrypt.so.20`, and
`libsoxr.so.0`. The corresponding ARMhf packages are absent from the clean
base inventory. The live device has a different package history, and complete presence of these
ARMhf dependencies there still requires verification; its observations cannot
establish clean-image compatibility. Their Debian
package provenance and whether they should be added to the OTA remain under
separate review; this source overlay does not include speculative replacements.

The OTA installer retains its matching-ABI loader gate and must reject an
installation where any staged shared-library tree fails to resolve. Clean-base
ARMhf FFmpeg dependency closure, and a fresh package loader test on both ABIs,
remain pending.

An earlier device-validation note reported the original 21-library set and
SRT alias as passing both loaders. A later live ARMhf check found that this
was incomplete: `libavcodec.so.58` still required `libwebpmux.so.3`, which the
old overlay lacked. That earlier ARMhf success claim is superseded; see
`research/vanilla-audit/r2-package-hardening-20261003.md`. The selected Debian
`libwebpmux3` package closes the observed direct missing dependency using the
matching `libwebp6` package. Its dependency chain continues through the
device's system `libm`, `libpthread`, libc, and ABI loader. A loader check of
the rebuilt R2 payload on the handheld is still pending.

On the R36S, prior checks used the native dynamic loaders with temporary
directories and the device's existing system libraries:

- `/lib/ld-linux-aarch64.so.1` for all AArch64 compatibility objects
- `/lib/ld-linux-armhf.so.3` for all ARMhf compatibility objects

The candidate DSperate AArch64 binary also resolves its SDL2 dependency on
this device. Loader checks do not test a PortMaster game, video/audio output,
or gameplay; the OTA record states that limit explicitly.

## Superseding historical ARMhf candidate closure

See `trixie-armhf-dependencies/README.md`, package manifest, SHA256SUMS and licensing records. 26 pinned package inputs solve without upgrades/removals on both official-base and live-device statuses; temporary-library native loader checks now pass. These inputs are not yet integrated into the active image/OTA installation flow; game and clean-base ELF validation remain pending. Earlier dependency-gap findings explain the old incomplete overlay and are retained as historical evidence.
