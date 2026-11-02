#!/usr/bin/env python3
"""GUI live view of windowed per-channel average H_min and R from the serial stream."""
import re
import queue
import threading
import tkinter as tk
from tkinter import ttk
from collections import deque, defaultdict

import serial

PORT = "/dev/tty.usbmodem1101"
BAUD = 115200
WINDOW = 150  # average over the last N samples, per channel
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


def reader(q, stop):
    """Background thread: parse serial lines, push (ch, h, r) tuples onto the queue."""
    try:
        with serial.Serial(PORT, BAUD, timeout=1) as ser:
            while not stop.is_set():
                raw = ser.readline().decode("utf-8", errors="ignore")
                m = LINE_RE.search(raw)
                if m:
                    ch = int(m.group(3)) if m.group(3) else NO_CH
                    q.put((ch, float(m.group(1)), int(m.group(2))))
    except serial.SerialException as e:
        q.put(("ERROR", str(e), None))


class App:
    def __init__(self, root):
        self.root = root
        self.q = queue.Queue()
        self.stop = threading.Event()
        # Channels are created as they are seen, so any number of them works.
        self.h_win = defaultdict(lambda: deque(maxlen=WINDOW))
        self.r_win = defaultdict(lambda: deque(maxlen=WINDOW))
        self.blocked = defaultdict(int)
        self.counted = defaultdict(int)
        self.rows = {}  # ch -> StringVars for that channel's row
        self.total = 0

        root.title("Serial Avg — H_min / R per channel")
        root.configure(padx=24, pady=20)
        root.minsize(620, 240)

        self.big = ("Helvetica", 26, "bold")
        self.lbl = ("Helvetica", 11)

        self.table = ttk.Frame(root)
        self.table.pack(anchor="w", fill="x")
        for col, text in enumerate(("", "avg H_min", "avg R", "window", "below floor")):
            ttk.Label(self.table, text=text, font=self.lbl, foreground="#888").grid(
                row=0, column=col, sticky="w", padx=(0, 18), pady=(0, 4)
            )
        self.table.grid_columnconfigure(1, minsize=150)
        self.table.grid_columnconfigure(2, minsize=140)

        meta = ttk.Frame(root)
        meta.pack(anchor="w", fill="x", pady=(14, 0))
        self.v_total = tk.StringVar(value="0")
        self.v_chans = tk.StringVar(value="0")
        ttk.Label(meta, text="samples:", font=self.lbl, foreground="#888").grid(row=0, column=0, sticky="w")
        ttk.Label(meta, textvariable=self.v_total, font=self.lbl).grid(row=0, column=1, sticky="w", padx=(6, 18))
        ttk.Label(meta, text="channels:", font=self.lbl, foreground="#888").grid(row=0, column=2, sticky="w")
        ttk.Label(meta, textvariable=self.v_chans, font=self.lbl).grid(row=0, column=3, sticky="w", padx=(6, 0))

        self.v_cur = tk.StringVar(value="waiting for data…")
        ttk.Label(root, textvariable=self.v_cur, font=self.lbl, foreground="#888").pack(anchor="w", pady=(10, 0))

        self.thread = threading.Thread(target=reader, args=(self.q, self.stop), daemon=True)
        self.thread.start()

        root.protocol("WM_DELETE_WINDOW", self.close)
        self.poll()

    def _row(self, ch):
        """Build this channel's row the first time the channel is seen."""
        if ch in self.rows:
            return self.rows[ch]
        r = len(self.rows) + 1  # row 0 is the header
        v = {k: tk.StringVar(value="—") for k in ("h", "r", "win", "blk")}
        ttk.Label(self.table, text=label(ch), font=self.lbl, foreground="#888").grid(
            row=r, column=0, sticky="w", padx=(0, 18)
        )
        ttk.Label(self.table, textvariable=v["h"], font=self.big).grid(row=r, column=1, sticky="w", padx=(0, 18))
        ttk.Label(self.table, textvariable=v["r"], font=self.big).grid(row=r, column=2, sticky="w", padx=(0, 18))
        ttk.Label(self.table, textvariable=v["win"], font=self.lbl).grid(row=r, column=3, sticky="w", padx=(0, 18))
        ttk.Label(self.table, textvariable=v["blk"], font=self.lbl).grid(row=r, column=4, sticky="w")
        self.rows[ch] = v
        return v

    def poll(self):
        last = None
        while True:
            try:
                item = self.q.get_nowait()
            except queue.Empty:
                break
            if item[0] == "ERROR":
                self.v_cur.set(f"serial error: {item[1]}")
                continue
            ch, h, r = item
            self.h_win[ch].append(h)
            self.r_win[ch].append(r)
            self.counted[ch] += 1
            if h < MIN_ENTROPY_FLOOR:
                self.blocked[ch] += 1
            self.total += 1
            last = (ch, h, r)

        for ch in sorted(self.h_win):
            hw, rw = self.h_win[ch], self.r_win[ch]
            v = self._row(ch)
            v["h"].set(f"{sum(hw) / len(hw):.4f}")
            v["r"].set(f"{sum(rw) / len(rw):.2f}")
            v["win"].set(f"{len(hw)} / {WINDOW}")
            v["blk"].set(f"{self.blocked[ch]} / {self.counted[ch]}")

        self.v_total.set(str(self.total))
        self.v_chans.set(str(len(self.h_win)))
        if last:
            self.v_cur.set(f"latest:  {label(last[0])}   H_min {last[1]:.4f}   |   R {last[2]}")

        self.root.after(100, self.poll)

    def close(self):
        self.stop.set()
        self.root.destroy()


if __name__ == "__main__":
    root = tk.Tk()
    App(root)
    root.mainloop()
