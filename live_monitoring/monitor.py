#!/usr/bin/env python3
# csi_receiver_plot.py
# Receives UDP packets with all subcarriers' real+imag from Nexmon C sender.
# Plots (1) smoothed average magnitude and (2) residual phase std (for LoS blocking).

import socket
import threading
import collections
import time
import numpy as np
import matplotlib.pyplot as plt
from matplotlib.animation import FuncAnimation

LISTEN_IP = '0.0.0.0'
LISTEN_PORT = 5600
HISTORY = 500
SMOOTH_WINDOW = 20  # moving average window

lock = threading.Lock()
timestamps = collections.deque(maxlen=HISTORY)
avg_amp_hist = collections.deque(maxlen=HISTORY)
phase_std_hist = collections.deque(maxlen=HISTORY)

def moving_average(x, n=10):
    if len(x) < n:
        return np.array(x)
    return np.convolve(x, np.ones(n)/n, mode='valid')

def recv_thread():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((LISTEN_IP, LISTEN_PORT))
    print(f"Listening for CSI packets on UDP {LISTEN_IP}:{LISTEN_PORT} ...")

    while True:
        data, _ = sock.recvfrom(8192)
        s = data.decode('ascii', errors='ignore').strip()
        if not s:
            continue
        parts = s.split(',')
        if len(parts) < 3 + 2*64:
            continue
        try:
            seq = int(parts[0])
            core = int(parts[1])
            stream = int(parts[2])
            reals = np.array([float(parts[3 + 2*i]) for i in range(64)])
            imags = np.array([float(parts[4 + 2*i]) for i in range(64)])
            csi = reals + 1j * imags
        except Exception:
            continue

        mags = np.abs(csi)
        avg_amp = np.mean(mags)

        # unwrap and detrend phase across subcarriers
        phase = np.unwrap(np.angle(csi))
        k = np.arange(len(phase))
        coeffs = np.polyfit(k, phase, 1)
        phase_resid = phase - np.polyval(coeffs, k)
        phase_std = np.std(phase_resid)

        with lock:
            timestamps.append(time.time())
            avg_amp_hist.append(avg_amp)
            phase_std_hist.append(phase_std)

def start_receiver():
    t = threading.Thread(target=recv_thread, daemon=True)
    t.start()

def animate(frame):
    with lock:
        if not timestamps:
            return []
        x = np.arange(-len(timestamps) + 1, 1, 1)

        ax1.clear()
        ax2.clear()

        # Smooth amplitude over time
        amp_smooth = moving_average(np.array(avg_amp_hist), SMOOTH_WINDOW)
        phase_smooth = moving_average(np.array(phase_std_hist), SMOOTH_WINDOW)

        ax1.set_title("Smoothed Average Amplitude (LoS strength)")
        ax1.set_xlabel("Samples")
        ax1.set_ylabel("Amplitude")
        ax1.plot(x[-len(amp_smooth):], amp_smooth, color='tab:blue')
        ax1.grid(True)

        ax2.set_title("Residual Phase Std (Multipath / Blocking indicator)")
        ax2.set_xlabel("Samples")
        ax2.set_ylabel("Std Dev (rad)")
        ax2.plot(x[-len(phase_smooth):], phase_smooth, color='tab:orange')
        ax2.grid(True)

    return []

if __name__ == "__main__":
    start_receiver()
    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(10, 6))
    ani = FuncAnimation(fig, animate, interval=200)
    plt.tight_layout()
    plt.show()
