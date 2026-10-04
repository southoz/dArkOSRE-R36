# Firmware installation on Windows

This guide describes the Windows preparation steps for a firmware image released by this fork. A GitHub source checkout, updater-migration helper, or OTA ZIP is not a flashable full image. Check the [firmware releases](https://github.com/dixtuel/dArkOSRE-R36/releases) and confirm that the release explicitly includes a firmware image before proceeding.

## What the Windows model selector does

The Windows helper described by the upstream wiki is `SELECT MODEL.bat`. It launches the PowerShell selector at `dtb/select_device.ps1`, which reads the supported-board list in `dtb/r36_devices.ini`. It is **not an image flasher**. The maintained source-tree version previews the chosen board, asks for confirmation, removes root-level `.dtb` files and `logo.bmp`, then copies the selected DTBs and resolution-matched logo from `dtb`. The official base image's bundled version predates the logo update and only selects DTBs; see the status note below before using an unreleased candidate.

The maintained source selector and its data are in [`files/BOOT/SELECT MODEL.bat`](../files/BOOT/SELECT%20MODEL.bat) and [`files/BOOT/dtb/`](../files/BOOT/dtb/). The official R36 installation wiki describes the same Windows step: [Firmware Installation — Step 5](https://github.com/southoz/dArkOSRE-R36/wiki/Firmware-Installation#step-5-update-the-dtb-files-files-are-being-rebuilt-for-the-r36s-control-service).

## Install a released image

1. Read the exact release notes and compatibility information. Back up the SD card and its data first. Use a good-quality card.
2. Download the firmware image asset from that release. Do not use a source archive, updater helper, or OTA ZIP as the image.
3. Extract the image using the archive format and instructions stated in the release. Verify the published image checksum against the extracted `.img`; confirm which file the checksum describes.
4. Write the `.img` to the firmware microSD card using a compatible image writer, such as Rufus or Raspberry Pi Imager. Verify the selected target is the SD card; writing erases it. The upstream R36 guide advises avoiding Balena Etcher for this device.
5. Open the card's **BOOT** volume in File Explorer. Back up any root-level `.dtb` files and `logo.bmp` to the PC before changing them.
6. Double-click `SELECT MODEL.bat`. Choose the exact motherboard/variant/panel entry that matches the handheld. If the board identity is unknown, stop and identify it before selecting; choosing a merely similar model can cause a blank display or prevent boot.
7. Review the preview. Confirm only if the selected DTB directory and model are correct. If you cancel at the prompt, no selector changes are applied. Eject the card safely.
8. Follow the release's first-boot instructions. The upstream R36 procedure says to remove the second ROM card until first boot finishes and not connect the charger during first boot; follow the release notes if they provide a newer device-specific procedure.

The selector changes only the BOOT-volume DTBs and logo. It does not flash the image, repartition the card, or migrate ROMs and saves. Keep the backup until the device has booted successfully. If the display stays blank, power off and restore the prior known-good DTB/logo or repeat the selection with the verified board identity.

## Current image status

The existing [firmware release](https://github.com/dixtuel/dArkOSRE-R36/releases/tag/r36-updater-migration-20261004) now includes a **static-checked R4 test candidate** and its checksums. It has not passed a clean-card physical R36S boot or first-boot test. Use it only on a separate test card; do not replace a working card. Its release notes list the exact archive/image hashes and all remaining hardware checks.

The candidate preserves the official `03082026` BOOT partition byte-for-byte, so its bundled `SELECT MODEL.bat` is the official base version; the newer source-tree selector is not part of this image. Select the correct motherboard/panel DTB using the device's verified identity. The image includes the maintained ROOTFS overlay and updater snapshot described in the release notes, but physical display, controls, audio, `/roms`/`/roms2`, updater-recovery and gameplay checks remain pending.
