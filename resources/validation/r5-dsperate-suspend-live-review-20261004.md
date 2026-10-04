# R5 DSperate profile and suspend check — 2026-10-04

## Applied update

The physical target is the user's R36S/RK3326, previously at `10032026-r4`, with EmulationStation selecting `/roms2`. The maintained updater applied OTA R5 from `dixtuel/darkos-updates`; the updater log reported the expected package SHA-256 and selected ROM root. `.VERSION` and `.update10032026-r5` now read as R5, EmulationStation is active, Plymouth says `dArkOSRE (10032026-r5)`, and the R5 rollback archive passes `sha256sum -c` on `/roms2`.

The R36 profile keys now match vanilla's `DSperate/configs/dsperate.ini.rk3326`: `cpu_oc=false`, `timing_oc=false`, `fast_load=true`, plus its pad hotkeys. The live `/roms2/nds/dsperate/dsperate.ini` retained `layout=dominant_v`, `frameskip=1`, and `frameskip_mode=adaptive`; its save/state/cheat paths remain on `/roms2`. The installer merged missing profile keys into the existing global template and card config without replacing their other settings. The new `Restore Default DSperate Settings.sh` matches firmware source SHA-256 `bc209ab9a27bc40bb3702ef8588e9051c6b2224cda9779851b9b2e4609494297`; live global config SHA-256 is `d2d80f0540cb43459cbab7ee1353f12f74966d11bc0d91e640a95e77593932b1`, and ROM2 config is `df821aa43aa04c0c633a68da914bbeb107d4a6f97663ebff38a927a6cf59d33c`. The tool is limited to the active ROM card, asks with controller A/B, backs up one prior config copy, and does not delete saves, states or per-game overrides.

The OTA package passed ZIP/checksum/shell validation, version/marker progression tests, backup-retention tests, ROM-root resolver tests, and isolated installer tests for both `/roms` and `/roms2`. The updater source and package are published as `ota-10032026-r5`.

## Scope and remaining physical validation

The DS game was not launched during the R5 install. Actual game boot, controller hotkeys, microphone, audio, battery-save, save-state, and exit-to-EmulationStation checks remain unverified. The restore tool's code and installation were checked, but its A/B prompt was not activated during the physical session so the user's current config remained untouched.

Automatic suspend remains disabled: `.TIMEOUT` is absent, `AutoSuspendTimeout` is unset, and `autosuspend.service` is disabled/inactive. Installed `auto_suspend.py` still matches vanilla's reviewed implementation and the maintained source. No suspend/wake cycle was triggered; this change set does not enable automatic sleep.

## Image/release distinction

The R5 OTA updates an existing R4 installation. It does not rebuild or modify the separate R4 static firmware-image candidate in the firmware release. That image still requires clean-card physical boot/firstboot validation before it can be called a verified image.
