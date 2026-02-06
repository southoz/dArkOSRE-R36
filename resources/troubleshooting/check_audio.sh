#!/bin/bash

# Audio Debug and Fix Script for RK3326 R36S Clone on dArkOS (RK817 Focus)
# Enhanced for deeper analysis: Prioritize rk3326-g80ca-linux.dtb (or similar kernel DTB) over uboot ones.
# Full DTB decompilation, RK817-specific fixes, ALSA state dump, headphone jack detection,
# and extended greps for speaker-related issues.
# Targets RK817 codec; assumes no headphones for speaker focus.
# Logs to audio_debug_fix.log in script dir (handles symlinks).
# Run from EmulationStation; reboot may be needed.

SCRIPT_DIR="$(dirname "$(realpath "$0")")"
LOGFILE="${SCRIPT_DIR}/audio_debug_fix.log"
DTB_DUMP="${SCRIPT_DIR}/dtb_decompiled.dts"  # Full DTB decomp for analysis

echo "Starting enhanced audio system analysis and fix attempt..." | tee $LOGFILE
echo "Timestamp: $(date)" >> $LOGFILE
echo "User: $(whoami)" >> $LOGFILE
echo "Hostname: $(hostname)" >> $LOGFILE

echo "\n=== System Overview ===" >> $LOGFILE
uname -a >> $LOGFILE 2>&1
cat /etc/os-release >> $LOGFILE 2>&1 || echo "No /etc/os-release found" >> $LOGFILE

echo "\n=== Kernel Command Line (for DTB/overlay info) ===" >> $LOGFILE
cat /proc/cmdline >> $LOGFILE 2>&1

echo "\n=== Loaded Modules (lsmod | grep snd/audio/i2c/codec) ===" >> $LOGFILE
lsmod | grep -iE 'snd|audio|i2c|codec|rockchip|rk817|es8316' >> $LOGFILE 2>&1 || echo "No matching modules found" >> $LOGFILE

echo "\n=== Full dmesg Output (kernel logs) ===" >> $LOGFILE
dmesg >> $LOGFILE 2>&1

echo "\n=== Filtered dmesg for Audio-Related Messages (Deeper Grep) ===" >> $LOGFILE
dmesg | grep -iE 'snd|audio|alsa|codec|hdmi|i2s|spdif|headphone|jack|rk817|es8316|rockchip|dtb|overlay|error|fail|route|widget|simple-audio-card|asoc|sound|spk|hp' >> $LOGFILE 2>&1 || echo "No audio-related dmesg entries" >> $LOGFILE

echo "\n=== ALSA Playback Devices (aplay -l) ===" >> $LOGFILE
aplay -l >> $LOGFILE 2>&1 || echo "aplay not found or no devices" >> $LOGFILE

echo "\n=== ALSA Mixer Controls Before Fixes (amixer) ===" >> $LOGFILE
amixer >> $LOGFILE 2>&1 || echo "amixer not found" >> $LOGFILE

echo "\n=== Detailed ALSA Mixer Contents Before Fixes (amixer contents) ===" >> $LOGFILE
amixer contents >> $LOGFILE 2>&1 || echo "amixer contents not supported" >> $LOGFILE

echo "\n=== Full ALSA State Dump (alsactl dump) ===" >> $LOGFILE
if command -v alsactl >/dev/null 2>&1; then
    alsactl -f - dump >> $LOGFILE 2>&1 || echo "alsactl dump failed" >> $LOGFILE
else
    echo "alsactl not available; install alsa-utils if needed" >> $LOGFILE
fi

echo "\n=== Sound Cards (/proc/asound/cards) ===" >> $LOGFILE
cat /proc/asound/cards >> $LOGFILE 2>&1 || echo "No /proc/asound/cards found" >> $LOGFILE

echo "\n=== Headphone Jack Status (if detectable) ===" >> $LOGFILE
cat /proc/asound/card*/codec* 2>/dev/null | grep -iE 'headphone|jack|hp|spk|speaker|detect' >> $LOGFILE 2>&1 || echo "No codec proc info or jack status found" >> $LOGFILE

echo "\n=== I2C Devices (potential audio codecs) ===" >> $LOGFILE
if command -v i2cdetect >/dev/null 2>&1; then
    for bus in $(ls /dev/i2c-* 2>/dev/null | sed 's/\/dev\/i2c-//'); do
        echo "I2C Bus $bus:" >> $LOGFILE
        i2cdetect -y $bus >> $LOGFILE 2>&1
    done
else
    echo "i2cdetect not available; install i2c-tools if needed" >> $LOGFILE
fi

echo "\n=== DTB/Overlay Files in /boot/ ===" >> $LOGFILE
ls -l /boot/dt* /boot/overlay* /boot/*.dtb 2>/dev/null >> $LOGFILE || echo "No DTB/overlay files found in /boot/" >> $LOGFILE

echo "\n=== Current DTB Info (Deeper Analysis - Prioritizing Kernel DTB) ===" >> $LOGFILE
if [ -f /proc/device-tree/model ]; then
    echo "Device Tree Model: $(cat /proc/device-tree/model)" >> $LOGFILE
    find /proc/device-tree/ -name '*sound*' -or -name '*audio*' -or -name '*codec*' -or -name '*rk817*' -or -name '*es8316*' -or -name '*spk*' -or -name '*speaker*' -or -name '*hp*' -or -name '*headphone*' >> $LOGFILE 2>&1
    echo "\nDumping deeper /proc/device-tree audio nodes:" >> $LOGFILE
    for node in $(find /proc/device-tree/ -name '*sound*' -or -name '*audio*' -or -name '*codec*' -or -name '*rk817*' -or -name '*es8316*' -or -name '*spk*' -or -name '*speaker*' -or -name '*hp*' -or -name '*headphone*'); do
        if [ -d "$node" ]; then
            echo "Node: $node" >> $LOGFILE
            ls -l "$node" >> $LOGFILE
            for prop in $(ls "$node"); do
                if [ -f "$node/$prop" ]; then
                    echo "  Prop $prop: $(cat "$node/$prop" 2>/dev/null || echo 'binary')" >> $LOGFILE
                fi
            done
        fi
    done
fi
if command -v dtc >/dev/null 2>&1 && ls /boot/*.dtb >/dev/null 2>&1; then
    # Prioritize kernel DTB like rk3326-g80ca-linux.dtb over uboot ones
    DTB_FILE=$(ls /boot/rk3326-*.dtb 2>/dev/null | head -1)
    if [ -z "$DTB_FILE" ]; then
        DTB_FILE=$(ls /boot/*.dtb | grep -iv 'uboot' | head -1)  # Fallback, exclude uboot if possible
    fi
    if [ -z "$DTB_FILE" ]; then
        DTB_FILE=$(ls /boot/*.dtb | head -1)  # Ultimate fallback
    fi
    echo "Selected DTB for decomp: $DTB_FILE (prioritized kernel over uboot)" >> $LOGFILE
    echo "Decompiling $DTB_FILE fully to $DTB_DUMP for analysis..." >> $LOGFILE
    dtc -I dtb -O dts "$DTB_FILE" > "$DTB_DUMP" 2>> $LOGFILE
    if [ -f "$DTB_DUMP" ]; then
        echo "Full DTB decomp saved to $DTB_DUMP. Key audio excerpts:" >> $LOGFILE
        grep -iE 'audio|sound|codec|hdmi|i2s|spdif|headphone|jack|rk817|es8316|spk|speaker|hp|widget|routing|simple-audio-card|asoc' "$DTB_DUMP" >> $LOGFILE || echo "No audio-related DTB nodes found" >> $LOGFILE
    else
        echo "Failed to decompile DTB" >> $LOGFILE
    fi
else
    echo "dtc not available or no DTB file found; install device-tree-compiler if needed" >> $LOGFILE
fi

echo "\n=== ALSA Configuration Files ===" >> $LOGFILE
cat /etc/asound.conf >> $LOGFILE 2>&1 || echo "No /etc/asound.conf" >> $LOGFILE
cat ~/.asoundrc >> $LOGFILE 2>&1 || echo "No ~/.asoundrc" >> $LOGFILE

# Attempt Fixes (RK817-Specific)
echo "\n=== Attempting Fixes for Speaker Audio (RK817 Focus) ===" >> $LOGFILE

# Fix 1: Remove ~/.asoundrc if it exists
if [ -f ~/.asoundrc ]; then
    echo "Removing ~/.asoundrc to reset ALSA config..." >> $LOGFILE
    rm ~/.asoundrc >> $LOGFILE 2>&1
    echo "Removed ~/.asoundrc" >> $LOGFILE
else
    echo "No ~/.asoundrc to remove" >> $LOGFILE
fi

# Fix 2: RK817-specific mixer settings (card 0 assumed; dynamic check)
CARD=$(cat /proc/asound/cards | grep -oP '^\s*\K\d+' | head -1 || echo 0)
echo "Setting ALSA mixer controls for speakers (card $CARD) - RK817 mode..." >> $LOGFILE
amixer -c $CARD sset 'Playback Path' SPK >> $LOGFILE 2>&1 || echo "'Playback Path' not found or failed" >> $LOGFILE
amixer -c $CARD sset Playback unmute 100% >> $LOGFILE 2>&1 || echo "Playback control not found" >> $LOGFILE
amixer -c $CARD sset Record unmute 100% >> $LOGFILE 2>&1 || echo "Record not found" >> $LOGFILE

# Fallback for ES8316-like if RK817 not matched
amixer -c $CARD sset Master unmute 80% >> $LOGFILE 2>&1 || echo "Master control not found" >> $LOGFILE
amixer -c $CARD sset Speaker unmute 100% >> $LOGFILE 2>&1 || echo "Speaker control not found" >> $LOGFILE
amixer -c $CARD sset Headphone unmute 100% >> $LOGFILE 2>&1 || echo "Headphone control not found" >> $LOGFILE

echo "\n=== ALSA Mixer Controls After Fixes (amixer) ===" >> $LOGFILE
amixer >> $LOGFILE 2>&1

# Test Audio
TEST_SOUND="/usr/share/sounds/alsa/Front_Center.wav"
if [ -f "$TEST_SOUND" ]; then
    echo "Playing test sound on speakers (Front_Center.wav)..." >> $LOGFILE
    aplay "$TEST_SOUND" >> $LOGFILE 2>&1 || echo "aplay failed; check if speakers work manually" >> $LOGFILE
else
    echo "Test sound file not found; test audio manually (e.g., in EmulationStation menus)" >> $LOGFILE
fi

echo "\n=== Journalctl Audio Logs (if systemd) ===" >> $LOGFILE
if command -v journalctl >/dev/null 2>&1; then
    journalctl | grep -iE 'audio|sound|alsa|codec|rk817|es8316|spk|hp|speaker|headphone' | tail -n 100 >> $LOGFILE 2>&1
else
    echo "journalctl not available" >> $LOGFILE
fi

echo "\nEnhanced analysis and fix attempt complete. Log saved to $LOGFILE" | tee -a $LOGFILE
echo "DTB full decomp at $DTB_DUMP - Check for missing speaker widgets/routing (e.g., no 'Speaker' in widgets)." >> $LOGFILE
echo "If speakers still don't work:" >> $LOGFILE
echo "- Reboot and test audio." >> $LOGFILE
echo "- DTB mismatch likely: Since original eMMC DTB unavailable, try R36S-specific DTBs from https://github.com/AeolusUX/R36S-DTB (test Panel 4/8/9 first for G80CA clones)." >> $LOGFILE
echo "- Or from ArkOS issues: e.g., rf3536k4ka.dtb for sound fixes (search GitHub arkos r36s dtb)." >> $LOGFILE
echo "- Manually: amixer -c $CARD sset 'Playback Path' SPK; aplay /path/to/test.wav" >> $LOGFILE
echo "- If RK817 routing errors in dmesg, patch DTB to add speaker widgets/routing." >> $LOGFILE