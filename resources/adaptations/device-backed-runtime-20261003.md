# R36 device-backed runtime adaptation — 2026-10-03

This source batch extends the earlier PPSSPP/default and ES nice-limit changes after actual R36 files were read over SSH. No source-mode inference replaces the device baseline: observed version 10032026-r2, ROM1 exFAT, ROM2 VFAT, Tools bound to ROM2, ES inactive before inspection. Installed unit/wrapper/scripts are retained in the workspace's device-validation readback with hashes; no credential files or host keys were read.

## PPSSPP (upstream b19a081, 596c142)

The installed R36 launcher uses `.ini.sdl` and shares the profile between modern and 2021. The source now fixes both modern INI templates, preserves the 13 corrected device-10 mappings and all R36 Select hotkeys, and adds a separate 2021 template owner. Modern clean defaults omit ForceMaxEmulatedFPS (following upstream); 2021 clean seed retains 30. Existing user profiles are not overwritten.

The adapted launcher follows the selected game under either ROM card via a strict dynamic regex. It preserves the original R36 HDMI preload branch, 2021 ALSA/killer-daemon handling and original libretro/watchpsp branch. It sets the home profile symlink by version, refusing to replace a real home config directory. On the first 2021 launch, an existing shared profile is copied through a temporary sibling directory into the new profile, preserving settings and saves and leaving the old tree untouched. After separation, subsequent profile settings/saves may diverge; initial copying is not ongoing synchronization. No Vulkan/zram branch is imported from vanilla.

The reset tool preserves R36 buttonmon and A/B confirmation, recognizes the R36 DTB label, resets both versions on both cards, and skips unmounted SD2. It removes only three configuration files, preserving savedata. The paired SD switchers remain unchanged: their literal ROM path substitution does not match the dynamic launcher, and its home symlink is selected again for each launch.

Host temporary-filesystem checks with stub emulators/services passed fresh profiles on both cards, migration of a dummy save and custom setting without loss, mode symlink selection, rejection of an invalid card and preservation of a real config directory. These are filesystem/control-flow checks, not PSP gameplay/controller/audio tests. Scripts passed bash -n.

## Wi-Fi importer (upstream 4a816bc, ad8a538)

The actual R36 importer had the old ifconfig detection, no importer service/boot hook was found in scoped boot/script checks. Preserve its existing credential parsing/NetworkManager/watchdaemon flow. Add upstream-style oneshot integration with firstboot ordering and RequiresMountsFor=/opt/system/Tools; this follows whichever ROM card owns the configured Tools mount without changing it.

Current USB adapter driver is r8188eu, WEXT. `/usr/sbin/iw dev wlan0 info` fails with No such device even as root while wlan0 is up; the wireless sysfs directory and ip-link query succeed. The new guard therefore prefers iw but falls back to the tested sysfs+ip path. Both importer guards use it. The device adapter check passed. Do not claim a successful credential import: no keyfile was read or importer run, and network services were not restarted.

## Existing R36 zram bug (downstream fix, not vanilla feature)

ZRam Manager's generated service is enabled on the device but inactive; /proc/swaps is empty. DefaultDependencies=yes adds After=sysinit/basic, while Before=swap.target and After=local-fs creates a boot ordering cycle. The original ExecStop=/usr/bin/swapoff also points to a nonexistent file; /usr/sbin/swapoff exists. This defect is in the original R36 generator (upstream source commit 11ce4295), not introduced by the new ES/Wi-Fi candidates.

Fix the generator and reviewed full unit: DefaultDependencies=no; After=systemd-modules-load.service; Before=swap.target shutdown.target; Conflicts=shutdown.target; correct swapoff path. Keep size/compression/priority and /etc/zram.conf unchanged. The helper script is recorded exactly, not rewritten. Do not auto-enable zram for users who disabled it.

An attempted dependency-reset drop-in failed validation: After dependencies cannot be removed via empty assignments in drop-ins. That failed candidate was removed and is not shipped. The full R36 unit replacement was then verified alongside ES/Wi-Fi candidates in an isolated temporary unit search path on the physical device: exit 0, no ordering cycles/command errors, with man-page checks disabled. No installed unit was replaced or reloaded. Future OTA must replace only an existing generated unit, preserve customizations/enable state, then verify real swap/start/shutdown behavior.

Systemd semantics: https://manpages.debian.org/trixie/systemd/systemd.service.5.en.html and https://github.com/systemd/systemd/blob/main/man/systemd.unit.xml

## Scope/publication

The updater draft stages 15 targets with hashes/modes/link targets. It is not a dated OTA or live raw-feed step. Physical game/menu/reboot/import/swap tests and owner/mode/active-card rollback are pending. Keep this batch out of public installation until those gates pass. The complete unrelated Debian upgrade and firmware image work remain paused/deferred; no large build input was recreated.

## PSP Minis and SD2 correction (post-batch diff review, 2026-10-03)

The live and source ES system entries invoke ppsspp.sh for both `psp` and `pspminis`. The first adapted guard accepted only `psp`; this unpublished regression was caught by checking every caller and corrected to accept both under either `/roms` or `/roms2`. Both systems intentionally share the selected card's `psp/ppsspp` or `psp/ppsspp-2021` configuration tree, matching R36 behavior. An unmounted SD2 is now rejected before any profile creation/link change. Neither SD-switcher's slash-delimited replacement matches this mount guard or the dynamic profile paths.

Host mocks passed both cards × both systems × modern/2021 and libretro branches, filenames with spaces, shared profile selection, first-2021 save/config migration, reset with SD2 mounted/unmounted and preservation of real home config directories. No emulator binaries, controls, sound or real games were exercised by these mocks. Source/draft equality, all 15 manifest hashes/modes/link targets and changed shell syntax were checked. These do not replace physical gameplay, boot or OTA validation.

Zram replacement is narrowed to the recorded generated unit SHA256 `237d0729fbc1c8ee2a7dfd50740e304e020a8d1d33e21bd2d65e5b2ef3a724d3`; custom units require separate review. The draft has no installer enforcing this yet and must not be published as a ready OTA.
