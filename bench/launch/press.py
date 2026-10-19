#!/usr/bin/env python3
"""Press a button on the DS+ from a script: writes a key down, sync, key up, sync to an input
device, which the kernel hands to every program reading it (as root). For timing launches.

    sudo press.py DEVICE-NAME KEY [HOLD-SECONDS]
    sudo press.py "Anbernic RG DS+ Gamepad" BTN_EAST       # A on the DS+
    sudo press.py "Anbernic RG DS+ Gamepad" BTN_MODE       # Menu
    sudo press.py adc-keys-back KEY_BACK                   # Back
    sudo press.py "Anbernic RG DS+ Gamepad" HAT_RIGHT      # the D-pad, a hat on the DS+

Prints CLOCK_BOOTTIME at the key down, the clock DSlot's and DSperate's marks use.
"""
import glob, os, struct, sys, time

CODES = {"BTN_SOUTH": 0x130, "BTN_EAST": 0x131, "BTN_NORTH": 0x133, "BTN_WEST": 0x134,
         "BTN_TL": 0x136, "BTN_TR": 0x137, "BTN_SELECT": 0x13a, "BTN_START": 0x13b,
         "BTN_MODE": 0x13c, "BTN_TL2": 0x138, "BTN_TR2": 0x139, "KEY_BACK": 158, "KEY_VOLUMEUP": 115, "KEY_VOLUMEDOWN": 114}
# A D-pad that is a hat: the axis and the value it takes while pressed.
HATS = {"HAT_LEFT": (0x10, -1), "HAT_RIGHT": (0x10, 1), "HAT_UP": (0x11, -1), "HAT_DOWN": (0x11, 1)}


def device(name):
    for d in glob.glob("/sys/class/input/event*"):
        with open(d + "/device/name") as f:
            if f.read().strip() == name:
                return "/dev/input/" + os.path.basename(d)
    sys.exit(f"no input device named {name!r}")


def event(fd, type_, code, value):
    os.write(fd, struct.pack("llHHi", 0, 0, type_, code, value))


name, key = sys.argv[1], sys.argv[2]
hold = float(sys.argv[3]) if len(sys.argv) > 3 else 0.08
fd = os.open(device(name), os.O_WRONLY)
print(f"{time.clock_gettime(time.CLOCK_BOOTTIME):.3f} s {key} down", flush=True)
kind, code, on = (3, *HATS[key]) if key in HATS else (1, CODES[key], 1)
event(fd, kind, code, on)
event(fd, 0, 0, 0)
time.sleep(hold)
event(fd, kind, code, 0)
event(fd, 0, 0, 0)
