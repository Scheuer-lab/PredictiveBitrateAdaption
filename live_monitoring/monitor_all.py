#!/usr/bin/env python3
import socket
import threading
import time
import numpy as np
import matplotlib.pyplot as plt
from matplotlib.animation import FuncAnimation
from datetime import datetime
import collections
import csv
from matplotlib import gridspec

CSI_PORT = 12346
QUEUE_PORT = 12345
MA_WINDOW = 100

log_filename = f"monitoring_log_{datetime.now().strftime('%Y%m%d_%H%M%S')}.csv"

# Shared data deques
ampl_data = collections.deque(maxlen=2000)
phase_data = collections.deque(maxlen=2000)
ampl_ma_data = collections.deque(maxlen=2000)
phase_ma_data = collections.deque(maxlen=2000)
phase_deriv_data = collections.deque(maxlen=2000)
snr_data = collections.deque(maxlen=2000)
backlog_data = collections.deque(maxlen=2000)
lock = threading.Lock()

# Last frame subcarrier data
N_SUB = 64
last_ampl_frame = np.zeros(N_SUB)
last_phase_frame = np.zeros(N_SUB)

# ===== Logging Setup =====
def init_log_file():
    """Initialize CSV with all subcarrier columns"""
    with open(log_filename, "w", newline="") as f:
        writer = csv.writer(f)
        header = [
            "timestamp_s", "source",
            "amplitude_mean", "phase_std",
            "amplitude_MA", "phase_MA",
            "phase_derivative", "SNR_dB", "backlog"
        ]
        header += [f"ampl_sc_{i}" for i in range(N_SUB)]
        header += [f"phase_sc_{i}" for i in range(N_SUB)]
        writer.writerow(header)

def log_data(source, amplitude="", phase_std="", amp_ma="", phase_ma="",
             phase_deriv="", snr="", backlog="", ampl_sc=None, phase_sc=None):
    recv_time = time.time()
    with open(log_filename, "a", newline="") as f:
        writer = csv.writer(f)
        row = [
            f"{recv_time:.6f}", source,
            amplitude, phase_std, amp_ma, phase_ma,
            phase_deriv, snr, backlog
        ]
        if ampl_sc is not None:
            row += list(map(lambda x: f"{x:.6f}", ampl_sc))
        else:
            row += [""] * N_SUB
        if phase_sc is not None:
            row += list(map(lambda x: f"{x:.6f}", phase_sc))
        else:
            row += [""] * N_SUB
        writer.writerow(row)

init_log_file()

# ===== CSI Receiver =====
def csi_receiver():
    global last_ampl_frame, last_phase_frame
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.bind(('0.0.0.0', CSI_PORT))
    sock.listen(1)
    print(f"[CSI] Waiting for TCP connection on port {CSI_PORT}...")
    conn, addr = sock.accept()
    print(f"[CSI] Connected from {addr}")

    moving_amp = collections.deque(maxlen=MA_WINDOW)
    moving_phase = collections.deque(maxlen=MA_WINDOW)
    buffer = ""

    while True:
        try:
            data = conn.recv(8192)
            if not data:
                break
            buffer += data.decode(errors='ignore')
            lines = buffer.split('\n')
            buffer = lines.pop() if not buffer.endswith('\n') else ''

            for s in lines:
                if not s:
                    continue
                parts = s.strip().split(',')
                if len(parts) < 3 + 2 * N_SUB:
                    continue

                reals = np.array([float(parts[3 + 2*i]) for i in range(N_SUB)])
                imags = np.array([float(parts[4 + 2*i]) for i in range(N_SUB)])
                csi = reals + 1j * imags

                mags = np.abs(csi)
                avg = np.mean(mags[mags != 0])
                phase = np.unwrap(np.angle(csi))
                k = np.arange(len(phase))
                coeffs = np.polyfit(k, phase, 1)
                resid = phase - np.polyval(coeffs, k)
                std = np.std(resid)

                moving_amp.append(avg)
                moving_phase.append(std)
                avg_ma = np.mean(moving_amp)
                std_ma = np.mean(moving_phase)

                # Phase derivative (LoS approaching indicator)
                phase_deriv = 0
                if len(phase_data) > 1:
                    dt = time.time() - phase_data[-1][0]
                    if dt > 0:
                        phase_deriv = (std - phase_data[-1][1]) / dt

                now = time.time()
                with lock:
                    ampl_data.append((now, avg))
                    phase_data.append((now, std))
                    ampl_ma_data.append((now, avg_ma))
                    phase_ma_data.append((now, std_ma))
                    phase_deriv_data.append((now, phase_deriv))

                    last_ampl_frame = mags
                    last_phase_frame = resid

                # Log full subcarrier frame
                log_data("CSI", avg, std, avg_ma, std_ma, phase_deriv,
                         ampl_sc=mags, phase_sc=resid)

        except Exception as e:
            print(f"[CSI] Error: {e}")
            break

# ===== Queue Receiver =====
def parse_queue_data(line):
    try:
        parts = line.strip().split()
        if len(parts) < 6:
            return None
        ts = int(parts[0])
        fq = int(parts[2].split('=')[1])
        snr = float(parts[5].split('=')[1])
        return ts, fq, snr
    except:
        return None

def queue_receiver():
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.bind(('', QUEUE_PORT))
    sock.listen(1)
    print(f"[Queue] Waiting for TCP connection on port {QUEUE_PORT}...")
    conn, addr = sock.accept()
    print(f"[Queue] Connected from {addr}")

    buffer = ""
    while True:
        data = conn.recv(4096)
        if not data:
            break
        buffer += data.decode(errors='ignore')
        lines = buffer.split('\n')
        buffer = lines.pop() if not buffer.endswith('\n') else ''
        for line in lines:
            res = parse_queue_data(line)
            if res:
                _, fq, snr_val = res
                now = time.time()
                with lock:
                    backlog_data.append((now, fq))
                    snr_data.append((now, snr_val))
                log_data("QUEUE", snr=snr_val, backlog=fq)

# ===== Plot Setup =====
fig = plt.figure(figsize=(12, 10))
gs = gridspec.GridSpec(4, 1, height_ratios=[1, 1, 1, 1], hspace=0.3)

ax1 = fig.add_subplot(gs[0])
ax2 = fig.add_subplot(gs[1], sharex=ax1)
ax3 = fig.add_subplot(gs[2])
ax4 = fig.add_subplot(gs[3])

# --- Subcarrier indices ---
subcarrier_idx = np.arange(-N_SUB//2, N_SUB//2)

# --- 1: CSI amplitude / phase std ---
ax1.set_title("CSI Amplitude / Phase Std / LoS Indicator")
ax1.set_ylabel("Amplitude")
ax1.grid(True)
ax1_phase = ax1.twinx()
ax1_phase.set_ylabel("Phase Std / Derivative")

line_ampl, = ax1.plot([], [], 'b-', label="Amplitude")
line_phase, = ax1_phase.plot([], [], 'r-', label="Phase Std")
line_ampl_ma, = ax1.plot([], [], 'c-', label=f"Amplitude MA{MA_WINDOW}")
line_phase_ma, = ax1_phase.plot([], [], 'y-', label=f"Phase MA{MA_WINDOW}")
line_phase_deriv, = ax1_phase.plot([], [], linestyle=':', color='m', label="d(PhaseSTD)/dt")

ax1.legend(loc='upper left')
ax1_phase.legend(loc='upper right')

# --- 2: Queue info ---
ax2.set_title("Queue Data")
ax2.set_ylabel("SNR (dB)")
ax2.grid(True)
ax2_backlog = ax2.twinx()
ax2_backlog.set_ylabel("Backlog")
line_snr, = ax2.plot([], [], 'g-', label="SNR")
line_backlog, = ax2_backlog.plot([], [], 'm-', label="Backlog")
ax2.legend(loc='upper left')
ax2_backlog.legend(loc='upper right')

# --- 3: Amplitude per subcarrier ---
ax3.set_title("Amplitude per Subcarrier (current frame)")
ax3.set_ylabel("Amplitude")
ax3.set_xlabel("Subcarrier Index")
ax3.grid(True)
line_sub_ampl = ax3.plot([], [], marker='o', linestyle='-', lw=1, color='b')[0]

# --- 4: Phase residual per subcarrier ---
ax4.set_title("Phase Residual per Subcarrier (current frame)")
ax4.set_ylabel("Phase Residual [rad]")
ax4.set_xlabel("Subcarrier Index")
ax4.grid(True)
line_sub_phase = ax4.plot([], [], marker='o', linestyle='-', lw=1, color='r')[0]

# ===== Update Function =====
def update(frame):
    with lock:
        all_timestamps = []
        for dq in (ampl_data, snr_data, backlog_data):
            if dq:
                all_timestamps.extend(t for t, _ in dq)
        if not all_timestamps:
            return []

        t0 = min(all_timestamps)
        tmax = max(all_timestamps)

        def xy(dq):
            if dq:
                t, y = zip(*dq)
                return np.array(t) - t0, np.array(y)
            return np.array([]), np.array([])

        xa, ya = xy(ampl_data)
        xp, yp = xy(phase_data)
        xma, yma = xy(ampl_ma_data)
        xpma, ypma = xy(phase_ma_data)
        xd, yd = xy(phase_deriv_data)
        xs, ys = xy(snr_data)
        xb, yb = xy(backlog_data)

        line_ampl.set_data(xa, ya)
        line_phase.set_data(xp, yp)
        line_ampl_ma.set_data(xma, yma)
        line_phase_ma.set_data(xpma, ypma)
        line_phase_deriv.set_data(xd, yd)
        line_snr.set_data(xs, ys)
        line_backlog.set_data(xb, yb)

        line_sub_ampl.set_data(subcarrier_idx, last_ampl_frame)
        line_sub_phase.set_data(subcarrier_idx, last_phase_frame)

        ax1.set_xlim(max(0, tmax - t0 - 35), tmax - t0 + 1)
        for axis in (ax1, ax1_phase, ax2, ax2_backlog):
            axis.relim()
            axis.autoscale_view(True, True, True)

        ax3.relim()
        ax3.autoscale_view(True, True, True)
        ax4.relim()
        ax4.autoscale_view(True, True, True)

    return [line_ampl, line_phase, line_ampl_ma, line_phase_ma,
            line_phase_deriv, line_snr, line_backlog,
            line_sub_ampl, line_sub_phase]

# ===== Start Threads =====
threading.Thread(target=csi_receiver, daemon=True).start()
threading.Thread(target=queue_receiver, daemon=True).start()

ani = FuncAnimation(fig, update, interval=200, blit=False)
plt.tight_layout()
plt.show()
