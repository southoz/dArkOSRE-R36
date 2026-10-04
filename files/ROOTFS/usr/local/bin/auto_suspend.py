#!/usr/bin/env python3

import os
import select
import subprocess
import time
from evdev import InputDevice, ecodes, list_devices

TIMEOUT_FILE = "/home/ark/.config/.TIMEOUT"
AC_STATUS_FILE = "/sys/devices/platform/rockchip-system-monitor/subsystem/devices/rk817-charger/power_supply/ac/status"
PROCESS_CHECK = "/usr/local/bin/processcheck.sh"
WAKE_KEYSTROKE = "sudo /usr/local/bin/keystroke.py || true"
SUSPEND_CMD = "sudo /bin/systemctl suspend || true"

DEFAULT_TIMEOUT_MINUTES = 30
POLL_INTERVAL = 0.25

# Ignore very tiny analog noise.
ABS_DEADZONE = 500


def read_timeout_minutes():
    try:
        with open(TIMEOUT_FILE, "r", encoding="utf-8") as f:
            return int(f.read().strip())
    except Exception:
        return DEFAULT_TIMEOUT_MINUTES


def run_cmd(cmd):
    return subprocess.check_output(cmd, shell=True, text=True)


def is_charging():
    try:
        with open(AC_STATUS_FILE, "r", encoding="utf-8") as f:
            return "Charging" in f.readline()
    except Exception:
        return False


def game_is_not_running():
    try:
        return "Nothing" in run_cmd(PROCESS_CHECK)
    except Exception:
        return False


def should_count_event(event):
    # Count key/button press and release events.
    if event.type == ecodes.EV_KEY:
        return True

    # Count meaningful analog/dpad movement.
    if event.type == ecodes.EV_ABS:
        return abs(event.value) > ABS_DEADZONE

    return False


def open_input_devices():
    devices = {}

    for path in list_devices():
        try:
            dev = InputDevice(path)

            # Avoid grabbing devices. Just observe them.
            caps = dev.capabilities()

            has_keys = ecodes.EV_KEY in caps
            has_abs = ecodes.EV_ABS in caps

            if has_keys or has_abs:
                dev.fd
                devices[dev.fd] = dev
                print(f"Monitoring {path}: {dev.name}")

        except Exception as e:
            print(f"Skipping {path}: {e}")

    return devices


def suspend_now():
    print("Going into suspend mode now")
    run_cmd(WAKE_KEYSTROKE)
    run_cmd(SUSPEND_CMD)
    time.sleep(5)


def main():
    timeout_minutes = read_timeout_minutes()
    timeout_seconds = timeout_minutes * 60

    devices = open_input_devices()
    last_activity = time.monotonic()

    while True:
        # Re-scan if no devices are open.
        if not devices:
            devices = open_input_devices()
            time.sleep(1)
            continue

        readable, _, _ = select.select(list(devices.keys()), [], [], POLL_INTERVAL)

        for fd in readable:
            dev = devices.get(fd)
            if not dev:
                continue

            try:
                for event in dev.read():
                    if should_count_event(event):
                        last_activity = time.monotonic()

            except OSError:
                print(f"Device disappeared: {dev.path}")
                try:
                    dev.close()
                except Exception:
                    pass
                devices.pop(fd, None)

        idle_seconds = time.monotonic() - last_activity

        if idle_seconds >= timeout_seconds:
            if not is_charging() and game_is_not_running():
                suspend_now()

            # Reset timer after checking so it does not immediately loop suspend attempts.
            last_activity = time.monotonic()


if __name__ == "__main__":
    main()