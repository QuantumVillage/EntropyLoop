#!/usr/bin/env python3
"""Live one-line average of H_min and R from the serial stream."""
import re
import sys
from collections import deque
import serial

PORT = "/dev/tty.usbmodem1101"
BAUD = 115200
WINDOW = 500  # average over the last N samples
LINE_RE = re.compile(r"H_min:\s*([-+]?\d*\.?\d+)\s*\|\s*R:\s*([-+]?\d+)")

def main():
    h_win = deque(maxlen=WINDOW)
    r_win = deque(maxlen=WINDOW)
    total = 0
    with serial.Serial(PORT, BAUD, timeout=1) as ser:
        while True:
            raw = ser.readline().decode("utf-8", errors="ignore")
            m = LINE_RE.search(raw)
            if not m:
                continue
            h_win.append(float(m.group(1)))
            r_win.append(int(m.group(2)))
            total += 1
            sys.stdout.write(
                f"\rsamples: {total:6d} | window: {len(h_win):3d}/{WINDOW}"
                f" | avg H_min: {sum(h_win) / len(h_win):8.4f}"
                f" | avg R: {sum(r_win) / len(r_win):9.2f}"
            )
            sys.stdout.flush()

if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print()  # leave the final line intact on Ctrl-C
