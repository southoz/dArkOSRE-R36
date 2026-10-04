# dArkOSRE R36 — maintained fork

<p align="center">
<img width="224" height="350" alt="image" src="https://github.com/user-attachments/assets/d0334598-9296-466f-8e6e-4ca2e15daf70" />
</p>

- **Customized dArkOS build** for supported R36S devices and clones. See the
  compatibility table below and the installation guide before flashing.
- This is the public source fork maintained at
  [dixtuel/dArkOSRE-R36](https://github.com/dixtuel/dArkOSRE-R36). The original
  project is [southoz/dArkOSRE-R36](https://github.com/southoz/dArkOSRE-R36).
- Source code in `main` is not a firmware image. Use the release page and follow
  the [firmware installation guide](docs/Firmware-Installation.md); do not flash
  a source checkout.
- Online update files are maintained separately in
  [dixtuel/darkos-updates](https://github.com/dixtuel/darkos-updates). The
  update repository documents which payloads are present and their validation
  limits.
- The vanilla-to-R36 adaptation scope and remaining device-validation gates
  are recorded in [`resources/vanilla-adaptations-20261003.md`](resources/vanilla-adaptations-20261003.md).
- The source overlay now carries the RK3326 emulator/core payloads published
  in OTA `10032026`, plus optional DSperate v3.0.0. Its GPL source and exact
  upstream binary provenance are under
  [`resources/third-party/DSperate-v3.0.0`](resources/third-party/DSperate-v3.0.0);
  the ROM-card-aware launcher keeps DSperate saves and states on the selected
  `/roms` or `/roms2` card. OTA R5 completes the vanilla RK3326 defaults and
  controller-operated restore tool. See
  [`resources/emulator-artifact-provenance.md`](resources/emulator-artifact-provenance.md).
- PortMaster's legacy FFmpeg compatibility libraries are included for both
  AArch64 and ARMhf with their Debian packages, hashes, and copyright notices
  in [`resources/third-party/portmaster-legacy-compat`](resources/third-party/portmaster-legacy-compat).
- The original firmware release notes are archived in
  [`resources/upstream-release-history.md`](resources/upstream-release-history.md).
  Those entries point to the original author's externally hosted images; this
  fork has not rebuilt or republished those firmware files.

[![GitHub release (latest by date)](https://img.shields.io/github/v/release/dixtuel/dArkOSRE-R36?style=flat-square)](https://github.com/dixtuel/dArkOSRE-R36/releases)
[![GitHub stars](https://img.shields.io/github/stars/dixtuel/dArkOSRE-R36?style=flat-square)](https://github.com/dixtuel/dArkOSRE-R36/stargazers)

## Prepare complete offline overlay inputs

The full ScummVM executable is stored losslessly in normal Git as a checked gzip asset because GitHub denies new LFS uploads to this public fork. Before assembling the overlay, run `python3 resources/third-party/scummvm-2026.3.0/materialize.py`; no network download is needed. [Storage and checksum details](resources/third-party/scummvm-2026.3.0/README.md).

## Current adaptation status — 2026-10-04

- Runtime R5 is installed on the physical R36S. Its active `/roms2` DSperate config now has the missing RK3326 profile and hotkey keys while retaining the user's dominant-screen layout and adaptive `frameskip=1` preference. The settings-reset tool and rollback archive are installed and verified. Full DS gameplay/hotkey testing remains separate.
- The existing [firmware release](https://github.com/dixtuel/dArkOSRE-R36/releases/tag/r36-updater-migration-20261004) still links the earlier static-checked R4 image candidate and now carries the R5-capable updater migration helper. That image has not passed physical clean-card boot/firstboot testing; the R5 changes arrive to existing R4 installs through the [R5 OTA](https://github.com/dixtuel/darkos-updates/releases/tag/ota-10032026-r5).
- The new ES source candidate has physically verified dual scraper selection, indicator settings and tool icons; authenticated scraper requests remain unverified, so the original production frontend is retained.
- The public preparation recipe now includes the required licensed icon font.
- [R3/ES scope](resources/validation/runtime-r3-and-es-20261003.md), [R4 SD routing validation](resources/validation/r4-sd1-rom-routing-20261004.md), and the [R5 DSperate/suspend live review](resources/validation/r5-dsperate-suspend-live-review-20261004.md). The image remains an explicitly unverified static candidate until clean-card physical boot/firstboot testing passes.

## Supported Systems  

The table below is retained from the upstream source project. Its tester names
and completion labels describe upstream testing, not validation of a new image
assembled by this fork. The current image assembly preserves the verified
official `03082026` BOOT partition, kernel, initrd and device trees. Newer BOOT
files present in this source tree are not automatically copied into that image.
Fresh-card boot and panel compatibility of a fork image require separate
physical validation; OTA testing on an existing installation does not establish
those results.

| Motherboard ID (Variant/Panel) | Type | Status | Tester |
|----------------|----------------|---------------|---------------|
| [HL-R36H-V20 2024-05-18](https://github.com/southoz/dArkOS-G80CA-RE/wiki/Firmware-Installation/#genuine-r36s) | r36s | :warning: [Beta Testing](https://github.com/southoz/dArkOSRE-R36/wiki/Beta-Testing) | :white_check_mark: southoz  |
| [HL-R36H-V21 2024-11-18](https://github.com/southoz/dArkOS-G80CA-RE/wiki/Firmware-Installation/#genuine-r36s) | r36s | :warning: [Beta Testing](https://github.com/southoz/dArkOSRE-R36/wiki/Beta-Testing) | :warning: untested  |
| [R36S Plus-V20 2025-03-18 2551](https://github.com/southoz/dArkOS-G80CA-RE/wiki/Firmware-Installation/#genuine-r36s) | r36s | :warning: [Beta Testing](https://github.com/southoz/dArkOSRE-R36/wiki/Beta-Testing) | :warning: untested  |
| [R36S-V12 2023-08-18 Panel 0](https://github.com/southoz/dArkOS-G80CA-RE/wiki/Firmware-Installation/#genuine-r36s) | r36s | :white_check_mark: completed | :white_check_mark: southoz  |
| [R36S-V12 2023-08-18 Panel 4](https://github.com/southoz/dArkOS-G80CA-RE/wiki/Firmware-Installation/#genuine-r36s) | r36s | :white_check_mark: completed  | :white_check_mark: RoiArthurB |
| [R36S-V21 2024-12-18 2550](https://github.com/southoz/dArkOS-G80CA-RE/wiki/Firmware-Installation/#genuine-r36s) | r36s | :warning: [Beta Testing](https://github.com/southoz/dArkOSRE-R36/wiki/Beta-Testing)  | :white_check_mark: southoz |
| [R36S-V21 2024-12-18 2551](https://github.com/southoz/dArkOS-G80CA-RE/wiki/Firmware-Installation/#genuine-r36s) | r36s | :warning: [Beta Testing](https://github.com/southoz/dArkOSRE-R36/wiki/Beta-Testing) | :white_check_mark: southoz |
| [R36S-V21 2024-12-18 2552](https://github.com/southoz/dArkOS-G80CA-RE/wiki/Firmware-Installation/#genuine-r36s) | r36s | :warning: [Beta Testing](https://github.com/southoz/dArkOSRE-R36/wiki/Beta-Testing)  | :white_check_mark: Yayi23 |
| [R36S-V22 2024-12-18](https://github.com/southoz/dArkOS-G80CA-RE/wiki/Firmware-Installation/#genuine-r36s) | r36s | :warning: untested |  |
| [R36S-V30 2025-11-18 2552](https://github.com/southoz/dArkOS-G80CA-RE/wiki/Firmware-Installation/#genuine-r36s) | r36s | :warning: [Beta Testing](https://github.com/southoz/dArkOSRE-R36/wiki/Beta-Testing) | :white_check_mark: 2jaym |
| [R36S-Y02 2024-12-18](https://github.com/southoz/dArkOS-G80CA-RE/wiki/Firmware-Installation/#genuine-r36s) | r36s | :white_check_mark: completed | :white_check_mark: Lysander92 |
| [R36S Plus-V20 2025-03-18](https://github.com/southoz/dArkOS-G80CA-RE/wiki/Firmware-Installation/#genuine-r36s) | r36s | :warning: [Beta Testing](https://github.com/southoz/dArkOSRE-R36/wiki/Beta-Testing) | :white_check_mark: southoz |
| [R36XX-V21 2024-12-18](https://github.com/southoz/dArkOS-G80CA-RE/wiki/Firmware-Installation/#genuine-r36s)| r36s | :white_check_mark: completed | :white_check_mark: Jason_3x  |
| [G80C-MB V1.1-20250319 Panel 8](https://github.com/southoz/dArkOS-G80CA-RE/wiki/Firmware-Installation/#r36s-clones)  | clone  | :warning: untested |  |
| [G80C-MB V1.1-20250319 Panel 9](https://github.com/southoz/dArkOS-G80CA-RE/wiki/Firmware-Installation/#r36s-clones)  | clone  | :warning: untested |  |
| [G80CA-MB V1.2-20250422 Panel 8](https://github.com/southoz/dArkOS-G80CA-RE/wiki/Firmware-Installation/#r36s-clones)  | clone  |:white_check_mark: completed | :white_check_mark: southoz |
| [G80CA-MB V1.2-20250422 Panel 9](https://github.com/southoz/dArkOS-G80CA-RE/wiki/Firmware-Installation/#r36s-clones) | clone  | :warning: untested |  |
| [G80CA-MB V1.2-20250423 Panel 8](https://github.com/southoz/dArkOS-G80CA-RE/wiki/Firmware-Installation/#r36s-clones) | clone  | :white_check_mark: completed | :white_check_mark: 66dude |
| [G80CA-MB V1.2-20250423 Panel 9](https://github.com/southoz/dArkOS-G80CA-RE/wiki/Firmware-Installation/#r36s-clones) | clone  | :warning: untested |  |
| [G80CA-MB V1.3-20251212 Panel 8](https://github.com/southoz/dArkOS-G80CA-RE/wiki/Firmware-Installation/#r36s-clones) | clone  | :white_check_mark: completed | :white_check_mark: Robadel  |
| [G80D-MB V1.0-20250609 Panel 8](https://github.com/southoz/dArkOS-G80CA-RE/wiki/Firmware-Installation/#r36s-clones) | clone  | :warning: [Beta Testing](https://github.com/southoz/dArkOSRE-R36/wiki/Beta-Testing) | :white_check_mark: Juliancillo1310 |
| [GR36S-MB V1.4-2025-07-30 R36 Ultra](https://github.com/southoz/dArkOS-G80CA-RE/wiki/Firmware-Installation/#r36s-clones) | clone  | :warning: [Beta Testing](https://github.com/southoz/dArkOSRE-R36/wiki/Beta-Testing) | :white_check_mark: southoz  |
| [R36S-V12 2023-08-18 R36 MAX](https://github.com/southoz/dArkOS-G80CA-RE/wiki/Firmware-Installation/#r36s-clones) | clone  | :warning: [Beta Testing](https://github.com/southoz/dArkOSRE-R36/wiki/Beta-Testing) | :white_check_mark: southoz  |
| [R36S-V12 2023-08-18 Variant 1 Panel 3 A](https://github.com/southoz/dArkOS-G80CA-RE/wiki/Firmware-Installation/#r36s-clones) | clone  | :warning: untested |  |
| [R36S-V12 2023-08-18 Variant 1 Panel 8 A](https://github.com/southoz/dArkOS-G80CA-RE/wiki/Firmware-Installation/#r36s-clones) | clone  | :warning: untested |  |
| [R36S-V12 2023-08-18 Variant 2 Panel 1](https://github.com/southoz/dArkOS-G80CA-RE/wiki/Firmware-Installation/#r36s-clones) | clone  | :white_check_mark: completed | :white_check_mark:  jawblade662 |
| [R36S-V12 2023-08-18 Variant 2 Panel 1 A](https://github.com/southoz/dArkOS-G80CA-RE/wiki/Firmware-Installation/#r36s-clones) | clone  | :warning: untested |  |
| [R36S-V12 2023-08-18 Variant 3 Panel A](https://github.com/southoz/dArkOS-G80CA-RE/wiki/Firmware-Installation/#r36s-clones) | clone  | :white_check_mark: completed | :white_check_mark: southoz |
| [R36S-V12 2023-08-18 Variant 3 Panel 3](https://github.com/southoz/dArkOS-G80CA-RE/wiki/Firmware-Installation/#r36s-clones) | clone  | :white_check_mark: completed | :white_check_mark: dnmnhat |
| [R36S-V20 2025-05-18 2541](https://github.com/southoz/dArkOS-G80CA-RE/wiki/Firmware-Installation/#r36s-clones) | clone | :warning: [Beta Testing](https://github.com/southoz/dArkOSRE-R36/wiki/Beta-Testing) | :white_check_mark: trofim601-create |
| [R36S-V20 2025-05-18 2548](https://github.com/southoz/dArkOS-G80CA-RE/wiki/Firmware-Installation/#r36s-clones) | clone | :warning: [Beta Testing](https://github.com/southoz/dArkOSRE-R36/wiki/Beta-Testing) | :white_check_mark: southoz  |
| [R36S-V20 2025-05-18 2549](https://github.com/southoz/dArkOS-G80CA-RE/wiki/Firmware-Installation/#r36s-clones) | clone | :warning: [Beta Testing](https://github.com/southoz/dArkOSRE-R36/wiki/Beta-Testing) | :warning: untested  |
| [Y3506_V03_20241104](https://github.com/southoz/dArkOS-G80CA-RE/wiki/Firmware-Installation#soy-sauce-r36s) | soysauce   | :white_check_mark: completed | :white_check_mark: Gr33k |
| [Y3506_v03_20241210](https://github.com/southoz/dArkOS-G80CA-RE/wiki/Firmware-Installation#soy-sauce-r36s) | soysauce   | :white_check_mark: completed | :white_check_mark: Gr33k |
| [Y3506_V03_20250317](https://github.com/southoz/dArkOS-G80CA-RE/wiki/Firmware-Installation#soy-sauce-r36s) | soysauce   | :white_check_mark: completed | :white_check_mark: darrynmelck |
| [Y3506_V04_20250529 2533](https://github.com/southoz/dArkOS-G80CA-RE/wiki/Firmware-Installation#soy-sauce-r36s) | soysauce   | :white_check_mark: completed | :white_check_mark: reinfo3-spec |
| [Y3506_V04_20250529 2537](https://github.com/southoz/dArkOS-G80CA-RE/wiki/Firmware-Installation#soy-sauce-r36s) | soysauce   | :white_check_mark: completed | :white_check_mark: DeaconCole |
| [Y3506_V04_20250529 2548](https://github.com/southoz/dArkOS-G80CA-RE/wiki/Firmware-Installation#soy-sauce-r36s) | soysauce   | :white_check_mark: completed | :white_check_mark: southoz |
| [Y3506_V04_20250529 2548 Panel 2](https://github.com/southoz/dArkOS-G80CA-RE/wiki/Firmware-Installation#soy-sauce-r36s) | soysauce   | :warning: [Beta Testing](https://github.com/southoz/dArkOSRE-R36/wiki/Beta-Testing)  | :warning: untested |
| [Y3506_V05_20251215 2551](https://github.com/southoz/dArkOS-G80CA-RE/wiki/Firmware-Installation#soy-sauce-r36s) | soysauce   | :warning: [Beta Testing](https://github.com/southoz/dArkOSRE-R36/wiki/Beta-Testing)  | :warning: untested |
| [Y3506_V05_20251215 2601](https://github.com/southoz/dArkOS-G80CA-RE/wiki/Firmware-Installation#soy-sauce-r36s) | soysauce   | :warning: [Beta Testing](https://github.com/southoz/dArkOSRE-R36/wiki/Beta-Testing)  | :white_check_mark: southoz |

## dArkOS Mods by Jason_3x
- [Bluetooth Manager](https://github.com/Jason3x/Bluetooth-Manager-for-dArkOS-)
- [Battery Voice Config](https://github.com/Jason3x/Battery-Voice-Config)
- [Ghost Loader](https://github.com/Jason3x/GhostLoader) 
- [ArkOS Dual SDCard Manager](https://github.com/Jason3x/Arkos-Dual-SD-Manager)

**Support Jason** [Here](https://github.com/Jason3x/Arkos-Dual-SD-Manager#-a-coffee-to-support-the-project)

### User Wish List:
- [Global Search Function](https://github.com/southoz/dArkOSRE-R36/issues/133)
- [Kodi Widevine arm64 support](https://github.com/southoz/dArkOSRE-R36/issues/152)

### If your Rk3326-based R36S, R36S Clone or Soy Sauce system is not listed
Raise an [issue in this maintained fork](https://github.com/dixtuel/dArkOSRE-R36/issues/new) with the motherboard ID and attach your original SD card `.dtb` files and `boot.ini` in a ZIP file. Remove personal data before attaching files.

## ✅ Features

- Advanced Drastic
- Kodi 21.3
- Wi-Fi
- USB Tethering (USB-C to USB-A adapter)
- Joystick with per-emulator controls updates and hotkeys.
- Function button support (Menu).
- Sound (speakers + headphones)
- LED Control

## Important Installation Information

- [Operating System Installation](https://github.com/southoz/dArkOS-G80CA-RE/wiki/Firmware-Installation)

## Related Projects & Thanks

- [dArkOS main project](https://github.com/christianhaitian/dArkOS) – huge thanks to christianhaitian!
- Community discussions: [r/R36S](https://www.reddit.com/r/R36S/) and [RetroHandhelds.gg](https://discord.com/channels/741895796315914271/1452057823927341196) R36S and clones discord channel.

Feel free to report issues or suggest improvements in the [Issues tab of this fork](https://github.com/dixtuel/dArkOSRE-R36/issues/new).
Happy retro gaming! 🎮
