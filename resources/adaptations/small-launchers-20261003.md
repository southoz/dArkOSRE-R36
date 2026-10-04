# R36 small launcher adaptations — 2026-10-03

## Exact upstream and R36 counterparts

Vanilla BigPEmu commit `97c837aa87fd55b721509b2ddd74cc32fa0ddef6` supplies the missing home symlink destination. The original R36 launcher contained the same destination omission and removed the home config recursively. Adapt only `files/ROOTFS/usr/local/bin/bigpemu.sh`: set the explicit link destination, refuse an existing real home config, retain R36 controller defaults and emulator/audio/governor logic, and refuse an unmounted second ROM card before profile writes. Card selection follows the Jaguar game argument. This deliberately refuses automatic migration of a real home config instead of deleting user settings.

R36 perfmax/perfnorm already contain the configurable image-delay behavior corresponding to vanilla `461ae017e958048ce0d2b7941f5cd87ce19ca2a2`. Their fallback artwork still points exclusively at /roms and is not rewritten by either SD switcher. Resolve fallback artwork from an explicit game-card argument; without one, use /roms2 only when actually mounted and selected by ES paths. Keep /roms for clean single-card defaults. Preserve existing delay/governor/artwork decoder choices. Missing-artwork branches now use a no-op instead of invalid continue statements outside loops.

## Verification and delivery boundary

Fifteen isolated fixture cases passed: both cards, mounted/unmounted SD2, game argument and ES path selection, filenames with spaces, explicit BigPEmu link destination and preservation of real home configs. Syntax and Git whitespace checks passed. Fixtures did not run emulator/governor/audio commands. No physical Jaguar game, launch-screen display, controller or save test has passed for this batch yet.

Identical source targets are staged in the update fork's non-installable next-OTA draft. Neither source commit nor draft staging establishes a tested public OTA. No ROM data, savedata, ES path configuration or SD switch script is included or altered.
