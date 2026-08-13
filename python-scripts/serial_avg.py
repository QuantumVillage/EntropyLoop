#!/usr/bin/env python3
"""Live per-channel average of H_min and R from the serial stream."""
import re
import sys
from collections import deque, defaultdict
import serial

PORT = "/dev/tty.usbmodem1101"
BAUD = 115200
WINDOW = 500  # average over the last N samples, per channel
MIN_ENTROPY_FLOOR = 1.6  # keep in step with MIN_ENTROPY_FLOOR in src/main.c

# The "CH<n>" field is optional, so output from firmware predating the per-channel
# tag still parses; those lines are collected under NO_CH.
NO_CH = -1
LINE_RE = re.compile(
    r"H_min:\s*([-+]?\d*\.?\d+)\s*\|\s*R:\s*([-+]?\d+)"
    r"(?:\s*\|\s*CH(\d+))?"
)


def label(ch):
    return "CH?" if ch == NO_CH else f"CH{ch}"


def main():
    # Channels are created as they are seen, so any number of them works.
    h_win = defaultdict(lambda: deque(maxlen=WINDOW))
    r_win = defaultdict(lambda: deque(maxlen=WINDOW))
    blocked = defaultdict(int)
    counted = defaultdict(int)
    total = 0
    drawn = 0

    with serial.Serial(PORT, BAUD, timeout=1) as ser:
        while True:
            raw = ser.readline().decode("utf-8", errors="ignore")
            m = LINE_RE.search(raw)
            if not m:
                continue
            h = float(m.group(1))
            r = int(m.group(2))
            ch = int(m.group(3)) if m.group(3) else NO_CH

            h_win[ch].append(h)
            r_win[ch].append(r)
            counted[ch] += 1
            if h < MIN_ENTROPY_FLOOR:
                blocked[ch] += 1
            total += 1

            lines = [f"samples: {total:6d}   channels: {len(h_win)}"]
            for c in sorted(h_win):
                hw, rw = h_win[c], r_win[c]
                lines.append(
                    f"  {label(c):4s} {len(hw):4d}/{WINDOW}"
                    f"   avg H_min {sum(hw) / len(hw):8.4f}"
                    f"   avg R {sum(rw) / len(rw):9.2f}"
                    f"   below floor {blocked[c]:5d}/{counted[c]:<6d}"
                )

            # Redraw the block in place; it grows if a new channel shows up.
            if drawn:
                sys.stdout.write(f"\x1b[{drawn}A")
            sys.stdout.write("".join(f"\x1b[2K{ln}\n" for ln in lines))
            sys.stdout.flush()
            drawn = len(lines)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print()  # leave the final block intact on Ctrl-C
