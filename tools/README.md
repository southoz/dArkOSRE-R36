# R36S update-feed migration tool

`upgrade-to-maintained-updater.sh` is a small migration helper for a dArkOSRE-R36 R36S already running the latest public firmware image (`.VERSION=03082026`). Copy the script to the first card's `/roms/tools/` directory, then launch it from EmulationStation's **Tools** system. `/roms/tools` remains on the first card even when the game library is switched to `/roms2`.

The tool does not write a firmware image, change partitions, update the kernel/DTB/initrd, or move ROMs/saves. It checks RK3326, `.VERSION`, and that EmulationStation paths select exactly one mounted ROM root. If both `/roms` and `/roms2` are mounted, the selected EmulationStation paths determine which card receives the backup. It downloads the fork's `Update.sh` from a pinned firmware commit over verified HTTPS and checks its SHA-256. It replaces the installed entrypoint only if it is byte-identical to the reviewed R36 entrypoint apart from the known upstream-vs-maintained-feed URL. The original entrypoint is backed up with numeric ownership, ACLs and xattrs to the active ROM card before replacement.

The device entrypoint then fetches `dArkOSUpdate.sh` from `dixtuel/darkos-updates/main`. That script detects the installed `.VERSION` and completion markers, validates that the state is consistent, and selects the first missing stage in order:

1. Confirms the `03082026` base image's existing legacy markers (`12242025`, `12312025`, `01082026`, `01162026`, `01302026`). Missing markers stop the updater; old upstream packages are not replayed.
2. Base OTA `10032026`.
3. R1, compatibility libraries, R2, R3, R4, then R5.

Installers that reboot stop the current run normally. After the device boots, run this tool or the regular EmulationStation **Update** action again to continue with the next missing stage. It does not skip directly to the newest version and does not install a later stage when prerequisites/markers are missing or inconsistent.

The helper accepts `.VERSION` values `03082026` and `10032026` through `10032026-r5`. Older or unknown versions stop without changing files; they need a separately prepared, backed-up full firmware-image upgrade first. Do not treat the image candidate in the same release as hardware-tested: the updater helper is the safer in-place route, but OTA stages still require stable power and the release notes' stated preconditions.

The source entrypoint SHA, updater-stage mapping, and per-release package checksums are pinned in the script/updater repository. The helper should be attached as a GitHub release asset; it is intentionally not part of the Google Drive image archive.

Verify a downloaded copy against [`SHA256SUMS`](SHA256SUMS) before placing it
under `/roms/tools/`.
