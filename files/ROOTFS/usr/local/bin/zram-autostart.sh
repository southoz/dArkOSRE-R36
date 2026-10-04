#!/bin/bash

ZRAM_SIZE_MB=512
ZRAM_ALGORITHM=lz4

if [ -f /etc/zram.conf ]; then
    source /etc/zram.conf
fi

modprobe zram num_devices=1 2>/dev/null

for i in {1..10}; do
    if [ -b /dev/zram0 ]; then
        break
    fi
    sleep 0.5
done

if [ ! -b /dev/zram0 ]; then
    echo "ERROR: Cannot create /dev/zram0"
    exit 1
fi

if [ -f /sys/block/zram0/comp_algorithm ]; then
    if grep -q "$ZRAM_ALGORITHM" /sys/block/zram0/comp_algorithm; then
        echo "$ZRAM_ALGORITHM" > /sys/block/zram0/comp_algorithm 2>/dev/null
    fi
fi

SIZE_BYTES=$((ZRAM_SIZE_MB * 1024 * 1024))
echo "$SIZE_BYTES" > /sys/block/zram0/disksize

mkswap /dev/zram0 >/dev/null 2>&1
swapon -p 5 /dev/zram0

exit 0
