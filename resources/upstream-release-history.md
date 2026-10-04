# Upstream dArkOSRE-R36 release history

This archive preserves the published release names, tags, dates, and notes from [southoz/dArkOSRE-R36](https://github.com/southoz/dArkOSRE-R36/releases). The fork has not rebuilt these firmware images. The original releases have no GitHub-uploaded assets; their notes link to externally hosted downloads. Use those links only with the original device-specific installation guidance.

---

## dArkOSRE-R36 (03082026)

- **Tag:** `dArkOSRE-R36(03082026)`
- **Published upstream:** 2026-03-10T08:03:35Z
- **Prerelease:** no
- **Original release:** https://github.com/southoz/dArkOSRE-R36/releases/tag/dArkOSRE-R36(03082026)
- **GitHub assets:** 0

## Update to dArkOSRE-R36
- R36 Control Centre to manage LEDs, gamma, audio path and boot volume
- Device selector for Windows to manage dtb files.
- Added Support for all SoySauce Devices
- Updated Emulationstation to fix the Screenscraper login and Volume indicator for R36 devices.
- Advanced Drastic and Drastic selectable via Emulationstation Emulators.
- r36_config replaces rg351mp and audiopath services
- Fix Back Ground Music that impacted single SD Card users.
- Fix Switch SD Scripts to update Kodi Libraries.
- Remove various scripts related to the rg351mp
- Replace dArkOS OTA update with dArkOSRE OTA update.

**Read and follow the [Instructions](https://github.com/southoz/dArkOSRE-R36/wiki/Firmware-Installation)**

### Firmware File
[Mega](https://mega.nz/file/k6AgTSTS#RrMGot_xVXyzAr5h_7RDNKFIv2GaKniLYliLSPA3UWc)  -  [Google](https://drive.google.com/file/d/1ONnNxR3cpGAC0d5YefS-xE-Hp1ph7Hm-/view?usp=sharing)  -  [Onedrive](https://1drv.ms/u/c/ea159b785597670e/IQAU5hZKBrDCR5g1KH6MGTXTAdijiNpH-9sV3Pd9Q7u0emc?e=tUtfu3) - [Torrent](https://github.com/southoz/dArkOSRE-R36/raw/refs/heads/main/resources/torrent/dArkOSRE_R36_trixie_03082026.7z.torrent)
SHA1 - A4858EEE2F1ECED10D3CE90C911D89450EEA700F


## Device Pack 03142016
- Add Y3506_V04_20250529 2533 - Headphone fix by [reinfo3-spec](https://github.com/reinfo3-spec)
- Add Y3506_V04_20250529 2537 - Headphone fix by [reinfo3-spec](https://github.com/reinfo3-spec)
- Fix R36S-V12 2023-08-18 Panel 0 - Issue mounting SD2
- Fix R36S-V12 2023-08-18 Panel 4 - Issue mounting SD2 thanks to [RoiArthurB](https://github.com/RoiArthurB)

[Mega](https://mega.nz/file/MuoxwKJB#uFgUJ-E9fOqPfaTpMtexc4cBM36VC-HFjaeSPS2WG64)

---

## dArkOS-G80CA-RE (02062026)

- **Tag:** `darkOS-G80CA-RE(02062026)`
- **Published upstream:** 2026-02-08T11:35:01Z
- **Prerelease:** no
- **Original release:** https://github.com/southoz/dArkOSRE-R36/releases/tag/darkOS-G80CA-RE(02062026)
- **GitHub assets:** 0

## Before you do anything!
- Read these instructions [Firmware Installation](https://github.com/southoz/dArkOS-G80CA-RE/wiki/Firmware-Installation)
- The current dtb files you need are linked in here!

## Align with dArkOS 01302026 Release
- Fully Integrated Kodi 21.3 into the rg351mp build process with integrated missing libraries
- Integrate Wifi and Bluetooth Firmware Pack thanks to Jason3x
- Add [ZRam Manager](https://github.com/daffycodebug/ZRam-Manager-R36S) by daffycodebug to improve system performance in games.
- Add [Wifi Toggle](https://github.com/Jason3x/wifi-toggle) 3.6 by Jason3x to toggle OTG and Host Mode for USB Storage access.
- Fix PPSSPP mute on forced exit by disabling the kill service and updating controls.ini (delete all controls.ini from psp folder)
- Support for the R36S-V12 2024-12-18
- See [Wiki](https://github.com/southoz/dArkOS-G80CA-RE/wiki) for details and controls.

### Firmware File
[Mega](https://mega.nz/file/Izo3TRzA#6CAO5SIFsOxxPBbdwWZV6NvjHn5NJcur0XuVX_-fA3g)  -  [Onedrive](https://1drv.ms/u/c/ea159b785597670e/IQCFDwCK7Wh2T7hEvkeGW5uuAdHxmH6WWn35SJEKEc3xsfU?e=HORFat) - [Torrent](https://github.com/southoz/dArkOS-G80CA-RE/raw/main/resources/torrent/dArkOS_G80CA-RE_trixie_02062026.7z.torrent)
SHA1 - f03badff281f41189a01399b9bfe5bfe34324bf4 

## Update Patch for dArkOS RE 02062026
- Includes dArkOS drastic as a selectable emulator
- Add Screenscaper developer login to Emulatonstation
- Fix Back Ground Music that impacted single SD Card users.
- Fix Switch SD Scripts to update Kodi Libraries.
- Remove rg351mp change LED script.

### Update Installation
- Download the [Update](https://mega.nz/file/lypj1aZY#NeyZz5_mvpC2-Jc-TBINtt7eNSHq5k9wii31VwfxFd0) 
- Extract to the`ports` folder on the SD Card, 
- Boot and select Ports 
- Select Update-02062026 to apply

---

## dArkOS-G80CA-RE (01242026)

- **Tag:** `dArkOS-G80CA-RE(01242026)`
- **Published upstream:** 2026-01-30T03:09:08Z
- **Prerelease:** no
- **Original release:** https://github.com/southoz/dArkOSRE-R36/releases/tag/dArkOS-G80CA-RE(01242026)
- **GitHub assets:** 0

## Kodi Release

- Integrate Kodi 21.3 into dArkOS on a RK3326
- Create Control Recovery Script
-  Support SD Card in the second slot
-  See [Wiki](https://github.com/southoz/dArkOS-G80CA-RE/wiki/Kodi) for details and controls.
- Include support for Gametank Emulation.

## Firmware Installation 
- See [guide](https://github.com/southoz/dArkOS-G80CA-RE/wiki/Firmware-Installation)
- Panel 9 users will need to copy these [files](https://github.com/southoz/dArkOS-G80CA-RE/tree/main/files/DTB/Panel9) to the boot partition

### Firmware File
[Mega](https://mega.nz/file/RrRz2LbY#avaOz9NLXT04NKOps4h-ECV5AqQNynigqhn4Lu37ebc) 
SHA1 - 853C9545B0EB63005293B8A0E81DFBBB433F34E0   

## Update Patch for dArkOS 01302026 OTA Update
- Patch to fix changed files after the dArkOS 01302026 OTA update is applied
  - Fix es_systems.cfg to restore Kodi in Emulationstation
  - Fix Switch to SD2 script to reintroduce elements for Kodi
  - Remove unused rg351mp dtb file in boot partition (Make sure you keep your dtb name as rk3326-g80ca-linux.dtb)
- Add [ZRam Manager](https://github.com/daffycodebug/ZRam-Manager-R36S) by daffycodebug to improve system performance in games.
- Add [Wifi Toggle](https://github.com/Jason3x/wifi-toggle) 3.6 by Jason3x to toggle OTG and Host Mode for USB Storage access.
### Update Installation
- Download the [Update](https://mega.nz/file/o75lAI4R#m87ADtgA0WYrjBJlYmnf-0sbBRmK2Gq7CU2NSeRLb5k) 
- Extract to the`ports` folder on the SD Card, 
- Boot and select Ports 
- Select Update-01242026 to apply 
- The script has been updated thanks to u/Philosophy_Every to check which SD card is in use.

## Wireless and Bluetooth Firmware pack
- Wifi Firmwares
- Bluetooth Firmwares
- [Download](https://mega.nz/file/0qAyWaIb#KsROaKMsNzfACmi5EPITSkKZoGYbOgJRtB4e6uWvjzE) thanks to Jason3x
### Update Installation
- Extract the ZIP file to /tools/firmware on the ROMs partition
- Boot, open the Menu, Select Options -> Tools -> Firmware -> Firmware

---

## dArkOS-G80CA-RE (01142026) - Release 7

- **Tag:** `dArkOS-G80CA-RE(01142026)_R7`
- **Published upstream:** 2026-01-26T05:15:34Z
- **Prerelease:** no
- **Original release:** https://github.com/southoz/dArkOSRE-R36/releases/tag/dArkOS-G80CA-RE(01142026)_R7
- **GitHub assets:** 0

## Fix Single SD-Card Installation 2

- retroarch.cfg was looking in roms2 for bios by default instead of roms (Thanks to Robadel over on Discord for reporting).

## Instalation 
- See [guide](https://github.com/southoz/dArkOS-G80CA-RE/wiki/Firmware-Installation)
- Panel 9 users will need to copy these [files](https://github.com/southoz/dArkOS-G80CA-RE/tree/main/files/DTB/Panel9) to the boot partition

## In progress but not quite ready.
- Kodi is working in dArkOS for the RK3326, but is using software decoding, attempting to get mpp for hardware acceleration working before I release, so it will be a few days.
- G80C Testing is progressing and is almost done.

[Mega](https://mega.nz/file/Nigx3AiC#ug15s8saeC91CnAoIB2KGX0g8pDHUvfMudMKsH3v6J4)
SHA1 - 4C309BCB8AFEEC7C679215DED686CA3164AEA9CC

---

## dArkOS-G80CA-RE (01242026) - Pre-release

- **Tag:** `dArkOS-G80CA-RE(01242026)_Pre`
- **Published upstream:** 2026-01-27T20:14:03Z
- **Prerelease:** yes
- **Original release:** https://github.com/southoz/dArkOSRE-R36/releases/tag/dArkOS-G80CA-RE(01242026)_Pre
- **GitHub assets:** 0

## ⚠️ Important Expectation Warnings

***This is a pre-release for testing.***

## Kodi Testing Release

- Kodi 23.1 has been compiled for dArkOS on the RK3326
- Software rendering, so keep your sources at 720p, the dArkOS kernel for the RK3326 has not been compiled with VPU support. 
- Testing with Buck Bunny 1080p 24 FPS showed that this was the limit of the device's CPU.
- Controls are available in the [Wiki](https://github.com/southoz/dArkOS-G80CA-RE/wiki/Kodi).
- A number of fixes in this new pre-release with Kodi, including missing 32-bit libs from the compile.
- Kodi takes control of the Volume process, so I have a script to set the Volume to 100% when Kodi start to provide access to the full range.

## Instalation 
- See [guide](https://github.com/southoz/dArkOS-G80CA-RE/wiki/Firmware-Installation)
- Panel 9 users will need to copy these [files](https://github.com/southoz/dArkOS-G80CA-RE/tree/main/files/DTB/Panel9) to the boot partition

## In progress but not quite ready.
- G80C Testing is progressing and is waiting on feedback from Testers. The uboot dtb is still problematic, but the OS works.

[Mega](https://mega.nz/file/c7JTDSRC#nRHRKiL7zHQmxtoZUMmiymfV0JPFGOWLDAp5Cb5el7w)
SHA1 - 5B3786CFA7498BB4FCCE18F08EFCE75AEC0E3580

---

## dArkOS-G80CA-RE (01142026) - Release 6

- **Tag:** `dArkOS-G80CA-RE(01142026)_R6`
- **Published upstream:** 2026-01-23T21:33:08Z
- **Prerelease:** no
- **Original release:** https://github.com/southoz/dArkOSRE-R36/releases/tag/dArkOS-G80CA-RE(01142026)_R6
- **GitHub assets:** 0

## Fix Single SD-Card Installation

- es_systems.cfg was looking in roms2 for games by default instead of roms

## Instalation 
- See [guide](https://github.com/southoz/dArkOS-G80CA-RE/wiki/Firmware-Installation)
- Panel 9 users will need to copy these [files](https://github.com/southoz/dArkOS-G80CA-RE/tree/main/files/DTB/Panel9) to the boot partition

[Mega](https://mega.nz/file/M7AkXCaZ#RU_cZyIJhbavk5F-nHn1Fh3a_ecFNaL1jZgeu2apgLo)
SHA1 - 62F48B978177A54C5F63F583BCB0DAB4DDB78E75

---

## dArkOS-G80CA-RE (01142026) - Release 5

- **Tag:** `dArkOS-G80CA-RE(01142026)_R5`
- **Published upstream:** 2026-01-23T09:17:11Z
- **Prerelease:** no
- **Original release:** https://github.com/southoz/dArkOSRE-R36/releases/tag/dArkOS-G80CA-RE(01142026)_R5
- **GitHub assets:** 0

## Hotkey Update Release

- Replace R3 with Select in Retroarch see [Wiki](https://github.com/southoz/dArkOS-G80CA-RE/wiki/Retroarch) for details.
- Replace R3 with Select in Mupen64Plus see [Wiki](https://github.com/southoz/dArkOS-G80CA-RE/wiki/Nintendo-64-(n64)) for details.
- Replace R3 with Select in PPSSPP see [Wiki](https://github.com/southoz/dArkOS-G80CA-RE/wiki/Sony-Playstation-Portable-(psp))  for details.
- Replace R3 with Select in Advanced Drastic see [Wiki](https://github.com/southoz/dArkOS-G80CA-RE/wiki/Nintendo-DS-(nds))  for details.

## Installation 
- See [guide](https://github.com/southoz/dArkOS-G80CA-RE/wiki/Firmware-Installation)
- Panel 9 users will need to copy these [files](https://github.com/southoz/dArkOS-G80CA-RE/tree/main/files/DTB/Panel9) to the boot partition

Fixing an issue where the default SD-Card for ROMs is the Second SD Card - New Release shortly

---

## dArkOS-G80CA-RE (01142026) - Release 4

- **Tag:** `dArkOS-G80CA-RE(01142026)_R4`
- **Published upstream:** 2026-01-22T09:29:40Z
- **Prerelease:** no
- **Original release:** https://github.com/southoz/dArkOSRE-R36/releases/tag/dArkOS-G80CA-RE(01142026)_R4
- **GitHub assets:** 0

## Advanced Drastic Release

- Replace Drastic with Advanced Drastic, see [Wiki](https://github.com/southoz/dArkOS-G80CA-RE/wiki/Nintendo-DS-(nds)) for details.
- Update default joystick type to Dual Shock for PSX, see [Wiki](https://github.com/southoz/dArkOS-G80CA-RE/wiki/Sony-Playstation-(psx)) for details.
- Game Boy Advance [Wiki](https://github.com/southoz/dArkOS-G80CA-RE/wiki/Nintendo-Gameboy-Advanced-(gba)).
- Super Nintendo [Wiki](https://github.com/southoz/dArkOS-G80CA-RE/wiki/Nintendo-Super-Nintendo-Entertainment-System-(snes)).
- Super Famicom [Wiki](https://github.com/southoz/dArkOS-G80CA-RE/wiki/Nintendo-Super-Famicom-(sfc)).

## Installation 
- See [guide](https://github.com/southoz/dArkOS-G80CA-RE/wiki/Firmware-Installation)
- Panel 9 users will need to copy these [files](https://github.com/southoz/dArkOS-G80CA-RE/tree/main/files/DTB/Panel9) to the boot partition

---

## dArkOS-G80CA-RE (01142026) - Release 3

- **Tag:** `dArkOS-G80CA-RE(01142026)_R3`
- **Published upstream:** 2026-01-21T04:02:04Z
- **Prerelease:** no
- **Original release:** https://github.com/southoz/dArkOSRE-R36/releases/tag/dArkOS-G80CA-RE(01142026)_R3
- **GitHub assets:** 0

## Nintendo DS Release

- Fix a number of start scripts that were referencing the dtb filename to set environmental variables
- Update Drastic Controls on the G80CA, see [Wiki](https://github.com/southoz/dArkOS-G80CA-RE/wiki/Nintendo-DS-(nds)) for details.

## Installation 
- See [guide](https://github.com/southoz/dArkOS-G80CA-RE/wiki/Firmware-Installation)
- Panel 9 users will need to copy these [files](https://github.com/southoz/dArkOS-G80CA-RE/tree/main/files/DTB/Panel9) to the boot partition

---

## dArkOS-G80CA-RE (01142026) - Release 2

- **Tag:** `dArkOS-G80CA-RE(01142026)_R2`
- **Published upstream:** 2026-01-19T10:54:25Z
- **Prerelease:** no
- **Original release:** https://github.com/southoz/dArkOSRE-R36/releases/tag/dArkOS-G80CA-RE(01142026)_R2
- **GitHub assets:** 0

## Maintenance with Dreamcast updated for G80CA

- Fix a permission issue with Retroarch32
- Update Retroarch32 controls and Dreamcast start script for Retrorun32 on the G80CA, see [Wiki](https://github.com/southoz/dArkOS-G80CA-RE/wiki/Audio-and-Screen-Control) for details.

## Instalation 
- See [guide](https://github.com/southoz/dArkOS-G80CA-RE/wiki/Firmware-Installation)
- Panel 9 users will need to copy these [files](https://github.com/southoz/dArkOS-G80CA-RE/tree/main/files/DTB/Panel9) to the boot partition

---

## dArkOS-G80CA-RE (01142026) - Release 1

- **Tag:** `dArkOS-G80CA-RE(01142026)_R1`
- **Published upstream:** 2026-01-18T21:17:59Z
- **Prerelease:** no
- **Original release:** https://github.com/southoz/dArkOSRE-R36/releases/tag/dArkOS-G80CA-RE(01142026)_R1
- **GitHub assets:** 0

## First release of customised dArkOS for the G80CA 

- dArkOS 01142026 Version with update supported
- Audit, Brightness and Gamma hotkeys, see [Wiki](https://github.com/southoz/dArkOS-G80CA-RE/wiki/Audio-and-Screen-Control) for details.
- EmulationStation overview and G80CA keys, see [Wiki](https://github.com/southoz/dArkOS-G80CA-RE/wiki/EmulationStation) for details.
- Retroarch and Retroarch32 configuration changes, for hotkeys , see [Wiki](https://github.com/southoz/dArkOS-G80CA-RE/wiki/EmulationStation) for details.
- PPSSPP configuration changes and tuning, see [Wiki](https://github.com/southoz/dArkOS-G80CA-RE/wiki/Sony-Playstation-Portable-(psp)) for details.
- Mupen64Plus configuration changes and tuning, see [Wiki](https://github.com/southoz/dArkOS-G80CA-RE/wiki/Nintendo-64-(n64)) for details.
- Yabasanshiro configuration changes, see [Wiki](https://github.com/southoz/dArkOS-G80CA-RE/wiki/Sega-Saturn-(saturn)) for details.

## Instalation 
- See [guide](https://github.com/southoz/dArkOS-G80CA-RE/wiki/Firmware-Installation)
- Panel 9 users will need to copy these [files](https://github.com/southoz/dArkOS-G80CA-RE/tree/main/files/DTB/Panel9) to the boot partition
