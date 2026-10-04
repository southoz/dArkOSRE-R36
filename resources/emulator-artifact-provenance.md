# Emulator and core artifacts in the R36S source tree

The following payloads were copied from the already published R36S OTA
`10032026/darkosupdate10032026.zip` into `files/ROOTFS` so a source checkout
contains the same emulator/core files as the public updater repository:

- PPSSPP 1.20.4 executable and matching assets (`opt/ppsspp`)
- Hypseus-Singe 2.12.1 and its shipped font/picture assets (`opt/hypseus-singe`)
- ScummVM 2026.3.0 and its matching themes/translations (`opt/scummvm`)
- RK3326 retrorun and retrorun32 binaries (`usr/local/bin`)
- XRoar 1.11 (`opt/xroar`)
- LowResNX core/info and RK3326 parallel_n64 core (already tracked in the
  source tree; their hashes match the public OTA)

The exact package inventory and original upstream release/package hashes are
recorded in `research/vanilla-audit/emulator-core-compatibility-review.md`.
These are targeted RK3326 artifacts; this does not replace the R36S
EmulationStation frontend or perform a wholesale RetroArch core swap. The R36S
EmulationStation executable and R36-specific system/configuration files remain
the dArkOSRE versions.

## Vanilla dArkOS EmulationStation graphics fix

Vanilla commit `4837de95426d2390f0ac2b8c87e8d29973ce04e` removes the `libOpenCL.so`
alias to `libMali.so` from both ABI directories and keeps its Vulkan linker
repair inside the RK3566 branch. R36S currently has that same Mali alias in its
AArch64 and ARMhf library directories. The R36 adaptation removes only those
two aliases, keeps Debian's versioned `libOpenCL.so.1` loader untouched, and
does not apply the RK3566 Vulkan change. The boot-time oneshot is idempotent.
