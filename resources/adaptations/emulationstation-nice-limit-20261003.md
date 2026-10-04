# R36 EmulationStation nice limit — source candidate

Vanilla's 08272026 OTA ZIP contains `etc/systemd/system/emulationstation.service` with `LimitNICE=-20`. Its current firmware source service omits the directive, so release source alone misses this artifact change. Local evidence with archive/member hashes is recorded in the workspace research report.

R36's official filesystem inventory has the existing service (root:root 0644, 284 bytes), with no recorded drop-in. Preserve that base service, launch wrapper, ordering, user and display environment. Add only `etc/systemd/system/emulationstation.service.d/20-r36-nice-limit.conf` with `[Service] LimitNICE=-20`.

This sets the permitted nice resource limit for processes started by the service; it does not itself set `Nice=-20`, real-time scheduling, CPU clocks, emulator settings or promise an FPS gain. A signed negative value is the normal Linux nice value, as documented by systemd: https://manpages.debian.org/trixie/systemd/systemd.exec.5.en.html

Source/draft only, not live. Future OTA must preserve rollback, use root:root 0644, reload systemd units and apply at the planned reboot/menu restart. Inspect effective service settings and `/proc/<ES pid>/limits`, then representative emulator priorities on R36; current device unit or later drop-ins may already provide the limit. Do not replace the original service wholesale. No ROM-card paths or SD switchers are touched. Physical menu/launcher validation remains pending.


## Superseding device-backed follow-up

See `device-backed-runtime-20261003.md`. Actual launcher/default owners and device service limits are now read back. The modern seed cap, SDL default and separate 2021 profile adaptation supersede earlier pending/unchanged-cap statements. Existing live user settings remain unchanged; physical title/menu/reboot tests are still pending.
