# R36S runtime and frontend validation — 2026-10-03

Target: original R36S/RK3326 dArkOSRE firmware, Debian13.3, active second-card ROM layout. Broad Debian upgrade is deferred.

## Runtime R3

The nineteen-target runtime candidate ZIP SHA-256 is `f6123c9e3a7e95d58b7ca95c7fd653134fac1f5c8236427f2da6637de7755b87`. Actual device installation, reboot, R3 marker/version, active original frontend, mounted second card, all target hashes/types and numeric metadata, and rollback archive integrity passed. Existing PSP control profile, ES system paths, both SD switch scripts and zram configuration stayed byte-identical.447MiB compressed zram swap is active after boot. The user confirmed NFS through the new modern PPSSPP launcher: face/shoulder controls, FN menu, audio and loading an existing save work. The save format was not separately identified.

Host fixtures cover selected-card backup behavior with both cards mounted, absent/masked/disabled/custom zram units, preserving Wi-Fi enablement, missing/known/repaired/custom controller DBs, symlink refusal and installation/reboot failure restoration. Main-card hardware switching,2021 emulator mode, credential import, BigPEmu and sustained performance remain separate tests.

The source modern controller DB SHA-256 is `741a2b21cc85c12c58f1edbe325d610b09fb81d872b9fb3fbb84c4abf86159c5`; the prior base OTA omitted it. Base OTA now includes the required asset and its validator rejects incomplete PPSSPP replacements. Existing profiles are preserved. PPSSPP's own Controls→RestoreDefaults still uses generic defaults instead of the original R36 seed; a separate emulator source adaptation is required.

## EmulationStation candidate

Pinned FCAMOD351v with the R36 volume implementation, native Mali/EGL linkage, both scraper gates and batteryIndicator networkIcon property was staged under a temporary isolated home. Candidate ELF SHA-256 `4c862d336e468f0aa2e5178f5d1bd66521fd9f192525097e4158ea63f25a8dad`. User confirmed both ScreenScraper and TheGamesDB selectable, clock/battery/network show/hide settings available, and Options icons restored after providing the expected FontAwesome4.7 font. That exact licensed font is now supplied by the source preparation recipe. No replacement frontend is installed by this record.

Provider selection is verified; authenticated scraping is not. ScreenScraper requires an authorized application credential arrangement. TheGamesDB requires a user key for its request test. The original production frontend was restored active and untouched. See [source recipe](../third-party/emulationstation-r36/README.md).

## Delivery limits

This is source and scoped physical validation evidence, not a flashable-image or universal-game compatibility claim. The original R36 boot/kernel/DTB chain remains authoritative. Firmware image assembly resumes from the verified official base in a separate WD ext4 workspace; source overlays are not disk images. Raw OTA wiring and release records are owned by the [update fork](https://github.com/dixtuel/darkos-updates).The separate26 pinned ARMhf dependency additions were installed on the physical R36S using reviewed local packages without APT, with exact installed versions and clean dpkg audit. The older live R2 lacked the repaired ARMhf WebP mux object/link; both absent paths were completed from the exact reviewed R2 source. All10 main legacy FFmpeg system-path loader checks then passed across ARMhf/AArch64. The exact public compatibility bundle subsequently passed its physical already-installed verification, followed by10 loader checks and a clean package audit before its completion marker. Maintained update main `908381a` now supplies R3 and compatibility before R2. Actual PortMaster gameplay and GitHub release-asset confirmation remain separate.
