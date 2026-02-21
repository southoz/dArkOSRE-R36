#!/bin/bash

# memory_debug.sh - Script to diagnose and debug memory issues on RK3326-based devices

SCRIPT_DIR="$(dirname "$(realpath "$0")")"
LOGFILE="${SCRIPT_DIR}/memory_report.txt"

# Function to check if a command exists
command_exists() {
    command -v "$1" >/dev/null 2>&1
}

# Function to display memory and swap usage (human-readable)
show_memory() {
    echo "Current Memory and Swap Usage:" >> "$LOGFILE"
    free -h >> "$LOGFILE"
    echo "Swap details:" >> "$LOGFILE"
    swapon --show >> "$LOGFILE" || echo "No swap configured." >> "$LOGFILE"
    echo "" >> "$LOGFILE"
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

# 2. Current Memory Usage
echo "2. Initial Memory and Swap Usage:" >> "$LOGFILE"
show_memory
cat /proc/meminfo | grep -E 'MemTotal|MemFree|MemAvailable|CmaTotal|CmaFree' >> "$LOGFILE"
echo "" >> "$LOGFILE"

# 3. Filtered dmesg Output (memory-related)
echo "3. Memory-Related dmesg Output:" >> "$LOGFILE"
dmesg | grep -iE 'memory|cma|ddr|ram|oom' >> "$LOGFILE" || echo "No relevant memory logs found." >> "$LOGFILE"
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

# 5. Reserved Memory Regions
echo "5. Reserved Memory Regions:" >> "$LOGFILE"
cat /proc/iomem | grep -i reserved >> "$LOGFILE"
echo "" >> "$LOGFILE"

# 6. Comprehensive Memory Test (Attempt install if missing, but check before running)
echo "6. Memory Stress Test:" >> "$LOGFILE"
if ! command_exists memtester || ! command_exists stress; then
    echo "Attempting to install memtester and/or stress (requires network)..." >> "$LOGFILE"
    sudo apt update >> "$LOGFILE" 2>&1
    sudo apt install -y memtester stress >> "$LOGFILE" 2>&1
fi

# Get available physical memory in MB (80% for safety)
TOTAL_MEM_MB=$(free -m | awk '/^Mem:/ {print $2}')
TEST_MEM_MB=$((TOTAL_MEM_MB * 80 / 100))
echo "Planned test: $TEST_MEM_MB MB of RAM (80% of total $TOTAL_MEM_MB MB)..." >> "$LOGFILE"

# Test RAM with memtester if available
if command_exists memtester; then
    echo "Running memtester on RAM:" >> "$LOGFILE"
    sudo memtester ${TEST_MEM_MB}M 2 >> "$LOGFILE" 2>&1
    echo "Post-memtester Memory Usage:" >> "$LOGFILE"
    show_memory
else
    echo "memtester not available (installation may have failed due to no network). Skipping RAM test." >> "$LOGFILE"
fi

# 7. Swap Test (if enabled and stress available)
echo "7. Swap Test:" >> "$LOGFILE"
if swapon --show &> /dev/null; then
    if command_exists stress; then
        echo "Swap detected. Testing overcommit to engage swap..." >> "$LOGFILE"
        CORES=$(nproc)
        echo "Spawning $((CORES + 1)) stress workers, each allocating ~$((TEST_MEM_MB * 3 / 2))MB for 60s..." >> "$LOGFILE"
        stress --vm $((CORES + 1)) --vm-bytes $((TEST_MEM_MB * 3 / 2))M --vm-keep --timeout 60s >> "$LOGFILE" 2>&1 &
        STRESS_PID=$!
        wait $STRESS_PID
        echo "Stress test completed." >> "$LOGFILE"
        echo "Post-stress Memory Usage:" >> "$LOGFILE"
        show_memory
    else
        echo "stress not available (installation may have failed due to no network). Skipping swap overcommit test." >> "$LOGFILE"
    fi
else
    echo "No swap configured. Skipping swap test." >> "$LOGFILE"
    echo "To enable swap: sudo fallocate -l 1G /swapfile; sudo chmod 600 /swapfile; sudo mkswap /swapfile; sudo swapon /swapfile" >> "$LOGFILE"
fi
echo "" >> "$LOGFILE"

# 8. Additional Memory Diagnostics
echo "8. Additional Memory Diagnostics:" >> "$LOGFILE"
echo "vmstat output (5 samples, 1s interval):" >> "$LOGFILE"
vmstat 1 5 >> "$LOGFILE"
echo "" >> "$LOGFILE"
echo "slabinfo (top 10 allocators):" >> "$LOGFILE"
cat /proc/slabinfo | sort -k3nr | head -n 10 >> "$LOGFILE"
echo "" >> "$LOGFILE"
echo "Full OOM killer logs (from dmesg):" >> "$LOGFILE"
dmesg | grep -i 'out of memory' >> "$LOGFILE" || echo "No OOM events found." >> "$LOGFILE"
echo "" >> "$LOGFILE"

echo "=== End of Report ===" >> "$LOGFILE"

echo "Memory debug report complete. Log saved to $LOGFILE"