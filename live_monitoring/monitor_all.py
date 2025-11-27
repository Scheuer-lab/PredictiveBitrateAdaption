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

# ===== Settings =====
CSI_PORT = 12346
QUEUE_PORT = 12345
MA_WINDOW = 100
CORR_SCALE = 1000  # scale correlation to match amplitude range
PLOT_MODE = "ALL"  # "ALL" or "TOP_ONLY"

log_filename = f"monitoring_log_{datetime.now().strftime('%Y%m%d_%H%M%S')}.csv"

# ===== Shared data =====
ampl_data = collections.deque(maxlen=2000)
phase_data = collections.deque(maxlen=2000)
ampl_ma_data = collections.deque(maxlen=2000)
phase_ma_data = collections.deque(maxlen=2000)
phase_deriv_data = collections.deque(maxlen=2000)
snr_data = collections.deque(maxlen=2000)
backlog_data = collections.deque(maxlen=2000)
los_median_amp = collections.deque(maxlen=2000)
los_phase_var = collections.deque(maxlen=2000)
los_corr = collections.deque(maxlen=2000)
lock = threading.Lock()

# Last frame subcarrier data
N_SUB = 64
last_ampl_frame = np.zeros(N_SUB)
last_phase_frame = np.zeros(N_SUB)
prev_frame = np.zeros(N_SUB, dtype=complex)

# ===== Logging =====
def init_log_file():
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
    global last_ampl_frame, last_phase_frame, prev_frame
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
                csi = np.fft.fftshift(csi)

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

                phase_deriv = 0
                if len(phase_data) > 1:
                    dt = time.time() - phase_data[-1][0]
                    if dt > 0:
                        phase_deriv = (std - phase_data[-1][1]) / dt

                # --- LoS indicators ---
                median_amp = np.median(mags)
                phase_variance = np.var(resid)
                correlation = np.abs(np.vdot(prev_frame, csi)) / (np.linalg.norm(prev_frame)*np.linalg.norm(csi)+1e-12)
                correlation_scaled = correlation * CORR_SCALE
                prev_frame = csi.copy()

                now = time.time()
                with lock:
                    ampl_data.append((now, avg))
                    phase_data.append((now, std))
                    ampl_ma_data.append((now, avg_ma))
                    phase_ma_data.append((now, std_ma))
                    phase_deriv_data.append((now, phase_deriv))
                    los_median_amp.append((now, median_amp))
                    los_phase_var.append((now, phase_variance))
                    los_corr.append((now, correlation_scaled))

                    last_ampl_frame = mags
                    last_phase_frame = resid

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
if PLOT_MODE == "ALL":
    fig = plt.figure(figsize=(12, 10))
    gs = gridspec.GridSpec(4, 1, height_ratios=[1, 1, 1, 1], hspace=0.3)
else:
    fig = plt.figure(figsize=(12, 4))
    gs = gridspec.GridSpec(1, 1)

subcarrier_idx = np.arange(-N_SUB//2, N_SUB//2)
guard_mask = ((subcarrier_idx >= -32) & (subcarrier_idx <= -29)) | \
             (subcarrier_idx == 0) | \
             ((subcarrier_idx >= 29) & (subcarrier_idx <= 31))

# --- Top plot (LoS indicators) ---
ax1 = fig.add_subplot(gs[0])
ax1.set_title("LoS Indicators: Median Amplitude / Phase Variance / Scaled CSI Correlation")
ax1.set_ylabel("Median Amp / Correlation")
ax1.grid(True)
ax1_phase = ax1.twinx()
ax1_phase.set_ylabel("Phase Variance")

line_los_amp, = ax1.plot([], [], 'b-', label="Median Amplitude")
line_los_corr, = ax1.plot([], [], 'g-', label=f"Correlation x{CORR_SCALE}")
line_los_phasevar, = ax1_phase.plot([], [], 'r-', label="Phase Variance")

ax1.legend(loc='upper left')
ax1_phase.legend(loc='upper right')

# --- Other plots only if PLOT_MODE == "ALL" ---
if PLOT_MODE == "ALL":
    ax2 = fig.add_subplot(gs[1], sharex=ax1)
    ax3 = fig.add_subplot(gs[2])
    ax4 = fig.add_subplot(gs[3])

    ax2.set_title("Queue Data")
    ax2.set_ylabel("SNR (dB)")
    ax2.grid(True)
    ax2_backlog = ax2.twinx()
    ax2_backlog.set_ylabel("Backlog")
    line_snr, = ax2.plot([], [], 'g-', label="SNR")
    line_backlog, = ax2_backlog.plot([], [], 'm-', label="Backlog")
    ax2.legend(loc='upper left')
    ax2_backlog.legend(loc='upper right')

    ax3.set_title("Amplitude per Subcarrier (current frame)")
    ax3.set_ylabel("Amplitude")
    ax3.set_xlabel("Subcarrier Index")
    ax3.grid(True)
    line_sub_ampl = ax3.plot([], [], marker='o', linestyle='-', lw=1, color='b')[0]

    ax4.set_title("Phase Residual per Subcarrier (current frame)")
    ax4.set_ylabel("Phase Residual [rad]")
    ax4.set_xlabel("Subcarrier Index")
    ax4.grid(True)
    line_sub_phase = ax4.plot([], [], marker='o', linestyle='-', lw=1, color='r')[0]

# ===== Update Function =====
def update(frame):
    with lock:
        all_timestamps = []
        for dq in (los_median_amp, los_phase_var, los_corr):
            all_timestamps.extend(t for t, _ in dq)
        if PLOT_MODE == "ALL":
            for dq in (snr_data, backlog_data):
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

        # --- Top plot ---
        x_amp, y_amp = xy(los_median_amp)
        x_phase, y_phase = xy(los_phase_var)
        x_corr, y_corr = xy(los_corr)

        line_los_amp.set_data(x_amp, y_amp)
        line_los_corr.set_data(x_corr, y_corr)
        line_los_phasevar.set_data(x_phase, y_phase)

        if PLOT_MODE == "ALL":
            xs, ys = xy(snr_data)
            xb, yb = xy(backlog_data)
            line_snr.set_data(xs, ys)
            line_backlog.set_data(xb, yb)

            ampl_plot = last_ampl_frame.copy()
            phase_plot = last_phase_frame.copy()
            ampl_plot[guard_mask] = np.nan
            phase_plot[guard_mask] = np.nan
            line_sub_ampl.set_data(subcarrier_idx, ampl_plot)
            line_sub_phase.set_data(subcarrier_idx, phase_plot)

        # --- Axis scaling ---
        ax1.set_xlim(max(0, tmax - t0 - 35), tmax - t0 + 1)
        ax1.relim(); ax1.autoscale_view(True, True, True)
        ax1_phase.relim(); ax1_phase.autoscale_view(True, True, True)

        if PLOT_MODE == "ALL":
            for axis in (ax2, ax2_backlog, ax3, ax4):
                axis.relim()
                axis.autoscale_view(True, True, True)

    if PLOT_MODE == "ALL":
        return [line_los_amp, line_los_corr, line_los_phasevar,
                line_snr, line_backlog, line_sub_ampl, line_sub_phase]
    else:
        return [line_los_amp, line_los_corr, line_los_phasevar]

# ===== Start Threads =====
threading.Thread(target=csi_receiver, daemon=True).start()
threading.Thread(target=queue_receiver, daemon=True).start()

ani = FuncAnimation(fig, update, interval=200, blit=False)
plt.tight_layout()
plt.show()
