# dArkOS-G80CA RetroOZ Edition

<p align="center">
<img width="480" height="320" alt="image" src="https://github.com/southoz/dArkOS-G80CA-RE/blob/main/files/BOOT/logo.bmp" />
</p>

**Customised dArkOS build optimized for G80CA / G80CA-MB devices**  
(RK3326-based R36S/RG351MP-style clones)

- This version of [dArkOS](https://github.com/christianhaitian/dArkOS) is customised for the best experience on G80CA hardware.
- If you are looking for the pure [dArkOS](https://github.com/christianhaitian/dArkOS) experience on the G80CA try [dArkOS-G80CA](https://github.com/southoz/dArkOS-G80CA)


[![GitHub release (latest by date)](https://img.shields.io/github/v/release/southoz/dArkOS-G80CA-SE?style=flat-square)](https://github.com/southoz/dArkOS-G80CA-SE/releases)
[![GitHub stars](https://img.shields.io/github/stars/southoz/dArkOS-G80CA-SE?style=flat-square)](https://github.com/southoz/dArkOS-G80CA-SE/stargazers)

## ✅ What's Working

- Screen
- Wi-Fi (rtl8192cu, rtl8821cu, rtl8812au and mt7601u verified)
- Joystick (uses **dArkOS native** control scheme for games & EmulationStation)
- Function Button
- Sound (speakers + headphones)
- Battery LED behaviour:
   - **Blue** ≥ 20%
   - **Blinking Red** < 20%
- Battery Charge current 800mAh

## ⚠️ Important Installation Warnings

Before flashing — **very important** to avoid bricking or failed installs:

- **Do NOT** have the charger plugged in during the whole flashing & first-boot process
- **Remove the Second SD Card** if installed and wait until the installation is complete before re-inserting.
- The ArkOS Wiki recommends **avoiding Balena Etcher** for this device (many users report failures)

## Step-by-Step Installation

1. Download the latest image from:  
   → [Releases page](https://github.com/southoz/dArkOS-G80CA/releases)

2. Extract the `.img` file using **7-Zip** (or similar)

3. Verify the file integrity (recommended):  
   Compare the hash with the value published in the release notes

4. Flash the image to a good quality microSD card using one of these tools (all confirmed working):
   - USB Image Tool
   - Raspberry Pi Imager
   - Rufus (Windows)
   - **Avoid** Balena Etcher if possible

5. Insert the card (no second ROM card yet) → power on

6. First boot:  
   → Blue screen appears → device expands partitions → auto-reboots

7. Second boot:  
   → Blue screen again → expands `roms.tar` to EASYROMS partition → auto-reboots

8. Final boot → you should land in EmulationStation

### Adding ROMs on second SD card (optional)

- Power off completely
- Insert your ROMs SD card
- Boot the device
- Press **START** → **Options** → **Advanced** → **Switch to SD2 for Roms**

## 🎮 Controller / Key Mapping (SDL)

Buttons have been aligned as closely as possible to the **RG351MP** layout.  
Analogue tuning is not yet implemented.

| G80CA Button          | SDL Mapping / Name                  | Type / Value          |
|-----------------------|-------------------------------------|-----------------------|
| A (Right)             | `b`                                 | button 01             |
| B (Bottom)            | `a`                                 | button 00             |
| X (Top)               | `x`                                 | button 02             |
| Y (Left)              | `y`                                 | button 03             |
| L1                    | `leftshoulder`                      | button 09             |
| R1                    | `rightshoulder`                     | button 10             |
| L2                    | `lefttrigger`                       | axis 04 → 32767       |
| R2                    | `righttrigger`                      | axis 05 → 32767       |
| L3                    | —                                   | button 14             |
| R3                    | —                                   | button 15             |
| D-Pad Up              | `dpup`                              | button 11             |
| D-Pad Down            | `dpdown`                            | button 12             |
| D-Pad Left            | `dpleft`                            | button 13             |
| D-Pad Right           | `dpright`                           | button 14             |
| Start                 | —                                   | button 13 (Joystick)  |
| Select                | —                                   | button 12 (Joystick)  |
| Function              | —                                   | button 16 (Joystick)  |
| Left Analog (all dir) | `leftx` / `lefty`                   | axis 00 / 01          |
| Right Analog (all dir)| `rightx` / `righty`                 | axis 03 / 02          |

**Note:** Axis directions/inversions have been adjusted to match typical RG351MP expectations.

## Related Projects & Thanks

- [dArkOS main project](https://github.com/christianhaitian/dArkOS) – huge thanks to christianhaitian!
- Community discussions: r/R36S, r/SBCGaming, Retro Handhelds groups

Feel free to report issues or suggest improvements in the [Issues tab](https://github.com/southoz/dArkOS-G80CA/issues).  
Happy retro gaming! 🎮
