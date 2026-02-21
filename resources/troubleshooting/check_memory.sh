#!/bin/bash

# memory_debug.sh - Script to diagnose and debug memory issues on RK3326-based devices

SCRIPT_DIR="$(dirname "$(realpath "$0")")"
LOGFILE="${SCRIPT_DIR}/memory_report.txt"
KERNEL_DTS="${SCRIPT_DIR}/kernel_decompiled.dts"
UBOOT_DTS="${SCRIPT_DIR}/uboot_decompiled.dts"

# Function to check if a command exists
command_exists() {
    command -v "$1" >/dev/null 2>&1
}

# Function to decompile DTB if dtc is available
decompile_dtb() {
    local dtb_file="$1"
    local output_file="$2"  # Now pass output path explicitly
    if command_exists dtc; then
        dtc -I dtb -O dts -o "$output_file" "$dtb_file" 2>/dev/null
        if [ $? -eq 0 ]; then
            echo "Decompiled $dtb_file to $output_file" >> "$LOGFILE"
            cat "$output_file" >> "$LOGFILE"
        else
            echo "Failed to decompile $dtb_file" >> "$LOGFILE"
        fi
    else
        echo "dtc not found. Install device-tree-compiler to decompile DTBs." >> "$LOGFILE"
    fi
}

echo "Starting memory debug report generation..." | tee "$LOGFILE"
echo "Generated on: $(date)" >> "$LOGFILE"
echo ""

echo "=== Memory Debug Report ===" >> "$LOGFILE"
echo "Generated on: $(date)" >> "$LOGFILE"
echo "" >> "$LOGFILE"

# 1. Basic System Info
echo "1. System Info:" >> "$LOGFILE"
uname -a >> "$LOGFILE"
cat /proc/cpuinfo | grep -i 'model name\|hardware' >> "$LOGFILE"
echo "" >> "$LOGFILE"

# 2. Memory Usage from /proc/meminfo
echo "2. Current Memory Usage:" >> "$LOGFILE"
cat /proc/meminfo | grep -E 'MemTotal|MemFree|MemAvailable' >> "$LOGFILE"
echo "" >> "$LOGFILE"

# 3. Full dmesg Output (may be verbose)
echo "3. Full dmesg Output:" >> "$LOGFILE"
dmesg >> "$LOGFILE"
echo "" >> "$LOGFILE"

# 4. Device Tree Memory Node (if accessible)
echo "4. Device Tree Memory Node:" >> "$LOGFILE"
if [ -d /proc/device-tree/memory@0 ]; then
    echo "reg property (hex):" >> "$LOGFILE"
    reg_hex=$(xxd -p /proc/device-tree/memory@0/reg | tr -d '\n')
    echo "$reg_hex" >> "$LOGFILE"
    echo "Translated to human-readable:" >> "$LOGFILE"
    len=${#reg_hex}
    if [ $len -eq 32 ]; then  # 16 bytes: typical for arm64 (#address-cells=2, #size-cells=2)
        addr_hi=$(echo "$reg_hex" | cut -c 1-8)
        addr_lo=$(echo "$reg_hex" | cut -c 9-16)
        size_hi=$(echo "$reg_hex" | cut -c 17-24)
        size_lo=$(echo "$reg_hex" | cut -c 25-32)
        address_hex="${addr_hi}${addr_lo}"
        size_hex="${size_hi}${size_lo}"
        address_dec=$((0x${address_hex}))
        size_dec=$((0x${size_hex}))
        size_mb=$((size_dec / 1024 / 1024))
        echo "Base Address: 0x${address_hex} (${address_dec} decimal)" >> "$LOGFILE"
        echo "Size: 0x${size_hex} (${size_dec} bytes, ${size_mb} MB)" >> "$LOGFILE"
    elif [ $len -eq 16 ]; then  # 8 bytes: typical for 32-bit (#address-cells=1, #size-cells=1)
        address_hex=$(echo "$reg_hex" | cut -c 1-8)
        size_hex=$(echo "$reg_hex" | cut -c 9-16)
        address_dec=$((0x${address_hex}))
        size_dec=$((0x${size_hex}))
        size_mb=$((size_dec / 1024 / 1024))
        echo "Base Address: 0x${address_hex} (${address_dec} decimal)" >> "$LOGFILE"
        echo "Size: 0x${size_hex} (${size_dec} bytes, ${size_mb} MB)" >> "$LOGFILE"
    else
        echo "Unexpected reg property length: ${len} chars (${len}/2 bytes). Manual inspection needed." >> "$LOGFILE"
    fi
else
    echo "No /proc/device-tree/memory@0 found. Memory node may not be defined or propagated." >> "$LOGFILE"
fi
echo "" >> "$LOGFILE"

# 5. Reserved Memory
echo "5. Reserved Memory Regions:" >> "$LOGFILE"
cat /proc/iomem | grep -i reserved >> "$LOGFILE"
echo "" >> "$LOGFILE"

# 6. Check for DTB files in common locations
echo "6. Searching for DTB Files:" >> "$LOGFILE"
dtb_locations=("/boot" "/boot/dtbs")  # Focus on directories; block devices need mounting
for loc in "${dtb_locations[@]}"; do
    if [ -d "$loc" ]; then
        echo "Checking $loc..." >> "$LOGFILE"
        find "$loc" -name "*.dtb" 2>/dev/null >> "$LOGFILE"
    fi
done
echo "Note: For block devices like /dev/mmcblk0p1, mount them first (e.g., mkdir /mnt/boot; mount /dev/mmcblk0p1 /mnt/boot) and search there." >> "$LOGFILE"
echo "" >> "$LOGFILE"

# 7. Decompile and Check Kernel DTB
kernel_dtb="/boot/rk3326-g80ca-linux.dtb"  # Specific target for RK3326 device
if [ -f "$kernel_dtb" ]; then
    echo "7. Decompiling Kernel DTB ($kernel_dtb):" >> "$LOGFILE"
    decompile_dtb "$kernel_dtb" "$KERNEL_DTS"
    echo "Checking for memory node in decompiled DTS:" >> "$LOGFILE"
    grep -A 5 -i 'memory@0' "$KERNEL_DTS" 2>/dev/null >> "$LOGFILE" || echo "No memory@0 node found." >> "$LOGFILE"
else
    echo "Kernel DTB not found at $kernel_dtb. Verify path or mount boot partition." >> "$LOGFILE"
fi
echo "" >> "$LOGFILE"

# 8. Decompile and Check U-Boot DTB
uboot_dtb="/boot/rg351v-uboot.dtb"  # Specific target for RG351V device
if [ -f "$uboot_dtb" ]; then
    echo "8. Decompiling U-Boot DTB ($uboot_dtb):" >> "$LOGFILE"
    decompile_dtb "$uboot_dtb" "$UBOOT_DTS"
    echo "Checking for memory node in decompiled DTS:" >> "$LOGFILE"
    grep -A 5 -i 'memory@0' "$UBOOT_DTS" 2>/dev/null >> "$LOGFILE" || echo "No memory@0 node found." >> "$LOGFILE"
else
    echo "U-Boot DTB not found at $uboot_dtb. Verify path or mount boot partition." >> "$LOGFILE"
fi
echo "" >> "$LOGFILE"

echo "=== End of Report ===" >> "$LOGFILE"

echo "Memory debug report complete. Log saved to $LOGFILE"
echo "Decompiled kernel DTS (if successful): $KERNEL_DTS"
echo "Decompiled U-Boot DTS (if successful): $UBOOT_DTS"