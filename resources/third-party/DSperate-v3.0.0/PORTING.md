# DSperate v3.0.0 on dArkOSRE-R36

- Upstream source tag: [`v3.0.0`](https://github.com/beebono/DSperate/tree/v3.0.0)
- Upstream source commit: `1b76c355109c9f7576363ccc023927b3137d3c6f`
- Upstream Linux AArch64 release asset: `dsperate-v3.0.0-linux-aarch64-static.tar.gz`
- Upstream asset SHA-256: `83d50fa776097647eabaa93627dae89098f91fc6644e809054556c4caa62db54`
- License: GNU GPL-3.0-or-later; see [`LICENSE`](LICENSE).

The executable in `files/ROOTFS/opt/DSperate/dsperate` is the upstream AArch64
release executable, not a locally compiled build. It requires the system SDL2
library; the R36S Debian image provides SDL2. This repository carries the exact
upstream source snapshot and upstream release notice alongside the executable.

The R36S EmulationStation NDS menu keeps Drastic and Advanced Drastic and adds
DSperate as a third selectable emulator. Its launcher reads the selected ROM
path to choose `/roms` or `/roms2`; each card receives its own configuration,
battery saves, and save states. It uses DSperate's built-in FreeBIOS unless the
user configures their own dumps. It never copies BIOS files into the package.
7z archives are expanded temporarily on the selected ROM card and cleaned up
when the emulator exits, avoiding an unbounded RAM extraction on this device.

The R36S controller mapping uses the `GO-Super Gamepad` GUID already present in
the device's EmulationStation input config. Controller hotkeys and game/save
compatibility still require physical-device testing. DS performance has not
been measured on RK3326 and no speed advantage over Drastic is claimed.

For a clean cross-build from this source, install CMake, Ninja, the
AArch64 GNU cross compiler, an AArch64 SDL2 development sysroot, and Vulkan
headers, then follow upstream's `aarch64-cross` CMake preset. The official
release asset is the binary used here; rebuilding against another sysroot may
change its runtime dependencies.
