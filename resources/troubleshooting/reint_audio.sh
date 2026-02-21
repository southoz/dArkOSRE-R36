#!/bin/bash
# /usr/local/bin/audio-reinit.sh
# Forces full audio wake-up (mimics power-button resume)

echo "=== Audio reinitialization started ==="

# 1. Force runtime PM resume (the most important line)
echo on | sudo tee /sys/class/sound/card0/power/control > /dev/null 2>&1
echo "Runtime PM forced to ON"

# 2. Restore saved ALSA state (default path = SPK)
sudo alsactl restore 0 2>/dev/null || echo "ALSA restore failed"

# 3. Manual fallback settings (from your asound.state)
amixer -c 0 sset 'Playback Path' SPK
amixer -c 0 sset 'Playback' 237
amixer -c 0 sset 'Record' 0

# 4. Short test tone so you can hear it works
TEST_SOUND="/usr/share/sounds/alsa/Front_Center.wav"
if [ -f "$TEST_SOUND" ]; then
    echo "Playing test sound (Front_Center.wav)..."
    aplay "$TEST_SOUND" || echo "aplay failed; check if speakers work manually"
else
    echo "Test sound file not found; skipping aplay test"
fi

echo "=== Audio reinitialized ==="