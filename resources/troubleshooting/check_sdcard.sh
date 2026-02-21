#!/bin/bash

# SD Card and Mounted Filesystems Debug and Fix Script for RK3326 R36S Clone on dArkOS
# This script collects detailed information about mounted filesystems and inserted SD cards, and attempts to fix issues.
# It targets problems like the second SD card not being detected when inserted.
# Common causes: MMC controller not probing, DTB mismatches, regulator failures, or filesystem errors.
# Logs to sd_card_debug.log in script dir (handles symlinks).
# Run from EmulationStation; reboot may be needed after fixes.

SCRIPT_DIR="$(dirname "$(realpath "$0")")"
LOGFILE="${SCRIPT_DIR}/sd_card_debug.log"

echo "Starting mounted filesystems and SD card analysis and fix attempt..." | tee $LOGFILE
echo "Timestamp: $(date)" >> $LOGFILE
echo "User: $(whoami)" >> $LOGFILE
echo "Hostname: $(hostname)" >> $LOGFILE
cat /proc/cpuinfo | grep -i 'model name\|hardware' >> "$LOGFILE"

echo "\n=== System Overview ===" >> $LOGFILE
uname -a >> $LOGFILE 2>&1
cat /etc/os-release >> $LOGFILE 2>&1 || echo "No /etc/os-release found" >> $LOGFILE

echo "\n=== Kernel Command Line (for DTB/overlay info) ===" >> $LOGFILE
cat /proc/cmdline >> $LOGFILE 2>&1

echo "\n=== Loaded Modules (lsmod | grep mmc/sd/storage) ===" >> $LOGFILE
lsmod | grep -iE 'mmc|sd|storage|disk|block|dwmmc|rockchip' >> $LOGFILE 2>&1 || echo "No matching modules found" >> $LOGFILE

echo "\n=== Full dmesg Output (kernel logs) ===" >> $LOGFILE
dmesg >> $LOGFILE 2>&1

echo "\n=== Filtered dmesg for SD/Storage-Related Messages (Deeper Grep) ===" >> $LOGFILE
dmesg | grep -iE 'mmc|sd|storage|disk|block|dwmmc|rockchip|mount|fsck|fat|exfat|error|fail|probe|regulator|power|domain|defer|detect|insert' >> $LOGFILE 2>&1 || echo "No SD/storage-related dmesg entries" >> $LOGFILE

echo "\n=== Mounted Filesystems (mount) ===" >> $LOGFILE
mount >> $LOGFILE 2>&1 || echo "mount command failed" >> $LOGFILE

echo "\n=== Disk Usage (df -h) ===" >> $LOGFILE
df -h >> $LOGFILE 2>&1 || echo "df command failed" >> $LOGFILE

echo "\n=== Block Devices (lsblk -f) ===" >> $LOGFILE
lsblk -f >> $LOGFILE 2>&1 || echo "lsblk not found" >> $LOGFILE

echo "\n=== Disk UUIDs (blkid) ===" >> $LOGFILE
sudo blkid >> $LOGFILE 2>&1 || echo "blkid not found or sudo failed" >> $LOGFILE

echo "\n=== fstab Contents (/etc/fstab) ===" >> $LOGFILE
cat /etc/fstab >> $LOGFILE 2>&1 || echo "No /etc/fstab found" >> $LOGFILE

echo "\n=== SD Card Devices (/sys/block/mmc*) ===" >> $LOGFILE
ls -l /sys/block/mmc* >> $LOGFILE 2>&1 || echo "No SD card devices found in /sys/block" >> $LOGFILE
for dev in /sys/block/mmc*; do
    if [ -d "$dev" ]; do
        echo "SD Device: $dev" >> $LOGFILE
        cat "$dev/device/name" "$dev/device/type" "$dev/device/vendor" "$dev/size" "$dev/removable" 2>/dev/null >> $LOGFILE
        echo "" >> $LOGFILE
    fi
done

echo "\n=== DTB Storage/SD-Related Info ===" >> $LOGFILE
if [ -f /proc/device-tree/model ]; then
    echo "Device Tree Model: $(cat /proc/device-tree/model)" >> $LOGFILE
    find /proc/device-tree/ -name '*mmc*' -or -name '*sd*' -or -name '*storage*' -or -name '*disk*' -or -name '*dwmmc*' >> $LOGFILE 2>&1
    for node in $(find /proc/device-tree/ -name '*mmc*' -or -name '*sd*' -or -name '*storage*' -or -name '*disk*' -or -name '*dwmmc*'); do
        if [ -d "$node" ]; do
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
    DTB_FILE=$(ls /boot/*.dtb | head -1)
    echo "Decompiling $DTB_FILE for storage/SD nodes:" >> $LOGFILE
    dtc -I dtb -O dts "$DTB_FILE" 2>/dev/null | grep -iE 'mmc|sd|storage|disk|dwmmc|regulator|power|domain|vcc|supply' >> $LOGFILE || echo "No storage/SD-related DTB nodes found" >> $LOGFILE
else
    echo "dtc not available or no DTB file found; install device-tree-compiler if needed" >> $LOGFILE
fi
