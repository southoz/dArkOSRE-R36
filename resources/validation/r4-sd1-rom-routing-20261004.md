# R4 ROM-card routing validation — 2026-10-04

## Target and scope

Physical test on a dArkOSRE-R36 R36S (RK3326). The test covers the R4 update installer, which repairs the system-owned Advanced SD2 launcher at `/opt/system/Advanced/Switch to SD2 for Roms.sh` and stores its rollback archive on the selected ROM card. It does not change ROM data, save data, SD switch scripts, kernel, or partition layout.

## SD2 selected, safe-absence branch

The R36S was at `.VERSION=10032026-r3` with the R3 and compatibility markers present. Both card mounts were active; EmulationStation selected game paths under `/roms2`. The Advanced SD2 entry was absent, while the canonical SD2 switcher and Main-SD switcher matched reviewed hashes. The R4 installer ran with `roms2` selected under the normal update-maintenance lock. It left the absent Advanced entry untouched, recorded `.VERSION=10032026-r4` and the R4 marker, and wrote its rollback archive under `/roms2/backup/darkosre-update/10032026-r4/`. Device post-readback verified the archive checksum and confirmed the ROM paths and protected settings were unchanged.

## SD1 selected, present-file repair branch

The device was then at R4 with the Advanced entry absent. Both `/roms` and `/roms2` remained mounted. Before testing, a numeric-owner/xattr tar snapshot of the EmulationStation system list, `.VERSION`, and R4 marker was saved on `/roms2`; its SHA-256 was `e8d5bbc2b5e438b15c718e7af81ca78e7400cc98240649036dbbf25cfd39ed45`.

The on-disk EmulationStation ROM paths were temporarily changed so all 124 game paths selected `/roms`, while `/roms2` stayed mounted. The exact pre-fix Advanced wrapper from firmware commit `1dd1255` was installed at the system target; SHA-256 `ea3dc01439224d150284966a18e162fcb9fb822cd1c0ff5870a1e36b27e7ac7f`. The R4 installer used in the OTA candidate had SHA-256 `8b8e38cdf1a4861636c3ff5446d0e9356daaa81b328218523e03579e176823fe`.

The installer removed only the pinned stale Singe path-rewrite line, preserved file metadata, and produced the expected repaired target hash `3163fd4f5b34386e2fad15985344711a91e2c01e88810c58b0a03a6d7d7ca4e9`. Its rollback archive was written under `/roms/backup/darkosre-update/10032026-r4/advanced-sd2-wrapper.before.tar`, not `/roms2`; SHA-256 `1c0c3dc8f9d7eda7301ede3231bab10b667408447e9ae9163d64d86a973c76aa`. The archive sidecar, archive integrity, and expected members passed verification.

## Restoration and result

The temporary system target was removed and the pre-test snapshot restored. Post-readback confirmed `.VERSION=10032026-r4`, the R4 marker present, the Advanced entry absent, and the original EmulationStation system-list hash restored (`a23c8073f326923d086946b0a8042c6e48dd5be8f7c704078b897d7becc0d3b0`). Both mounts and both ROM switcher hashes were unchanged; the active EmulationStation configuration again selected `/roms2`. The pre-test archive and OTA rollback archive both passed their SHA-256 sidecar checks. Temporary test files were removed; the two rollback/evidence archives remain on their respective ROM cards.

**Result:** R4 chooses its backup destination from the mounted ROM root selected by EmulationStation, even when the other ROM mount is also active. Both the `/roms2` safe-absence path and the `/roms` selected-file repair path passed on the physical R36S. No game, switch script, frontend restart, or reboot was run during the follow-up test. This does not validate a clean firmware-image boot or the complete migration flow from an older firmware release.
