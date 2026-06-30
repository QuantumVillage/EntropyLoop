#!/usr/bin/env python3
"""GUI live view of windowed average H_min and R from the serial stream."""
import re
import queue
import threading
import tkinter as tk
from tkinter import ttk
from collections import deque

import serial

PORT = "/dev/tty.usbmodem1101"
BAUD = 115200
WINDOW = 150  # average over the last N samples
LINE_RE = re.compile(r"H_min:\s*([-+]?\d*\.?\d+)\s*\|\s*R:\s*([-+]?\d+)")


def reader(q, stop):
    """Background thread: parse serial lines, push (h, r) tuples onto the queue."""
    try:
        with serial.Serial(PORT, BAUD, timeout=1) as ser:
            while not stop.is_set():
                raw = ser.readline().decode("utf-8", errors="ignore")
                m = LINE_RE.search(raw)
                if m:
                    q.put((float(m.group(1)), int(m.group(2))))
    except serial.SerialException as e:
        q.put(("ERROR", str(e)))


class App:
    def __init__(self, root):
        self.root = root
        self.q = queue.Queue()
        self.stop = threading.Event()
        self.h_win = deque(maxlen=WINDOW)
        self.r_win = deque(maxlen=WINDOW)
        self.total = 0

        root.title("Serial Avg — H_min / R")
        root.configure(padx=24, pady=20)
        root.minsize(360, 240)

        self.v_h = tk.StringVar(value="—")
        self.v_r = tk.StringVar(value="—")
        self.v_cur = tk.StringVar(value="waiting for data…")
        self.v_win = tk.StringVar(value=f"0 / {WINDOW}")
        self.v_total = tk.StringVar(value="0")

        big = ("Helvetica", 34, "bold")
        lbl = ("Helvetica", 11)

        ttk.Label(root, text="avg H_min", font=lbl, foreground="#888").pack(anchor="w")
        ttk.Label(root, textvariable=self.v_h, font=big).pack(anchor="w", pady=(0, 12))
        ttk.Label(root, text="avg R", font=lbl, foreground="#888").pack(anchor="w")
        ttk.Label(root, textvariable=self.v_r, font=big).pack(anchor="w", pady=(0, 16))

        meta = ttk.Frame(root)
        meta.pack(anchor="w", fill="x")
        ttk.Label(meta, text="window:", font=lbl, foreground="#888").grid(row=0, column=0, sticky="w")
        ttk.Label(meta, textvariable=self.v_win, font=lbl).grid(row=0, column=1, sticky="w", padx=(6, 18))
        ttk.Label(meta, text="samples:", font=lbl, foreground="#888").grid(row=0, column=2, sticky="w")
        ttk.Label(meta, textvariable=self.v_total, font=lbl).grid(row=0, column=3, sticky="w", padx=(6, 0))
        ttk.Label(root, textvariable=self.v_cur, font=lbl, foreground="#888").pack(anchor="w", pady=(10, 0))

        self.thread = threading.Thread(target=reader, args=(self.q, self.stop), daemon=True)
        self.thread.start()

        root.protocol("WM_DELETE_WINDOW", self.close)
        self.poll()

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
            h, r = item
            self.h_win.append(h)
            self.r_win.append(r)
            self.total += 1
            last = (h, r)

        if self.h_win:
            self.v_h.set(f"{sum(self.h_win) / len(self.h_win):.4f}")
            self.v_r.set(f"{sum(self.r_win) / len(self.r_win):.2f}")
            self.v_win.set(f"{len(self.h_win)} / {WINDOW}")
            self.v_total.set(str(self.total))
        if last:
            self.v_cur.set(f"latest:  H_min {last[0]:.4f}   |   R {last[1]}")

        self.root.after(100, self.poll)

    def close(self):
        self.stop.set()
        self.root.destroy()


if __name__ == "__main__":
    root = tk.Tk()
    App(root)
    root.mainloop()
