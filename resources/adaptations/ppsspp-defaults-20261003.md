# R36 PPSSPP default adaptation — source candidate, 2026-10-03

Upstream: christianhaitian/dArkOS commit `b19a081539e728c1f5460a158f4243a1728435c0` (2026-05-01), full diff reviewed. It removes conflicting device-1 bindings alongside device-10 mappings in RG351MP/G350 defaults and enables `IgnoreBadMemAccess`.

R36 counterpart: `/opt/ppsspp/backupforromsfolder/ppsspp/PSP/SYSTEM/{controls.ini,ppsspp.ini}`. The original R36 control template contained exactly the same 13 conflicting bindings. Remove device-1 entries only for Up/Down/Left/Right, Square/Triangle, Start, L/R and four analog directions. Preserve all original device-10 mappings, Select combinations, exit, save/load, slot and fast-forward hotkeys. Set only `IgnoreBadMemAccess = True` in the settings template.

This is the template owner under `/opt`; it does not overwrite existing `/roms/psp/ppsspp` or `/roms2/psp/ppsspp` user profiles or saves. SD switchers and game XML are unchanged. The profile is copied by the original launcher/restore flow when needed; the overlay lacks that launcher, so its exact installed flow must be inspected before claiming new defaults are live. A running device with an existing profile will retain it.

Do not copy vanilla RG351MP controls wholesale: R36 has additional Select hotkeys that are preserved here. Do not transplant the multi-version launcher from `596c142b375d32d706b36a11c65c6c8e1c3cdc35` until the installed R36 launcher and 2021 paths are available. `ForceMaxEmulatedFPS = 30` remains unchanged pending that launcher/profile comparison; upstream removed this cap for modern standalone but retained it for 2021.

State: source adaptation only. No physical PSP title/control/save tests or live settings change; no OTA publication. Matching templates are staged under the updater's `drafts/next-ota-ppsspp-defaults/`, outside its raw release feed. Before packaging, inspect both ROM-card flows and the template-copy behavior; apply only `/opt` files with rollback, preserving numeric metadata and all user profiles.


## Superseding device-backed follow-up

See `device-backed-runtime-20261003.md`. Actual launcher/default owners and device service limits are now read back. The modern seed cap, SDL default and separate 2021 profile adaptation supersede earlier pending/unchanged-cap statements. Existing live user settings remain unchanged; physical title/menu/reboot tests are still pending.
