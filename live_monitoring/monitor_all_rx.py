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
MA_WINDOW = 10
CORR_SCALE = 1000  
# PLOT_MODE can be "ALL", "TOPTWO", or any other string (defaulting to Variance only)
PLOT_MODE = "ALL" 
VARIANCE_WINDOW = 10
PLOT_TIME_WINDOW = 10.0 # Time window for history plots (in seconds)

log_filename = f"monitoring_log_{datetime.now().strftime('%Y%m%d_%H%M%S')}.csv"

# ===== Shared data & History =====
ampl_data = collections.deque(maxlen=2000)
phase_data = collections.deque(maxlen=2000)
ampl_ma_data = collections.deque(maxlen=2000)
phase_ma_data = collections.deque(maxlen=2000)
phase_deriv_data = collections.deque(maxlen=2000)
snr_data = collections.deque(maxlen=2000)
backlog_data = collections.deque(maxlen=2000)

# Variance tracking for Core 0 and Core 1
core0_ampl_history = collections.deque(maxlen=VARIANCE_WINDOW)
core0_phase_history = collections.deque(maxlen=VARIANCE_WINDOW)
core1_ampl_history = collections.deque(maxlen=VARIANCE_WINDOW)
core1_phase_history = collections.deque(maxlen=VARIANCE_WINDOW)

# Variance data for plotting
core0_ampl_var = collections.deque(maxlen=2000)
core0_phase_var = collections.deque(maxlen=2000)
core1_ampl_var = collections.deque(maxlen=2000)
core1_phase_var = collections.deque(maxlen=2000)

# ===== Frame Delay Ports =====
FRAME_DELAY_SENDER_PORT = 23456
FRAME_DELAY_RECEIVER_PORT = 23457

# Storage for delays
frame_delay_sender = collections.deque(maxlen=2000)
frame_delay_receiver = collections.deque(maxlen=2000)


# IAT Tracking (Still used for logging, but not plotted)
iat_instant_data = collections.deque(maxlen=2000) 
last_recv_time = time.time() 

lock = threading.Lock()

# Last frame subcarrier data
N_SUB = 64
N_CORES = 4
last_ampl_frame = np.zeros(N_SUB)
last_phase_frame = np.zeros(N_SUB)
core_ampl_frames = [np.zeros(N_SUB) for _ in range(N_CORES)]
core_phase_frames = [np.zeros(N_SUB) for _ in range(N_CORES)]

# ===== Logging =====
def init_log_file():
    with open(log_filename, "w", newline="") as f:
        writer = csv.writer(f)
        header = [
            "timestamp_s", "source",
            "amplitude_mean", "phase_std",
            "amplitude_MA", "phase_MA",
            "phase_derivative", "SNR_dB", "backlog", "instant_iat_ms"
        ]
        header += [f"ampl_sc_{i}" for i in range(N_SUB)]
        header += [f"phase_sc_{i}" for i in range(N_SUB)]
        writer.writerow(header)

def log_data(source, amplitude="", phase_std="", amp_ma="", phase_ma="",
             phase_deriv="", snr="", backlog="", iat_ms="", ampl_sc=None, phase_sc=None):
    recv_time = time.time()
    with open(log_filename, "a", newline="") as f:
        writer = csv.writer(f)
        row = [
            f"{recv_time:.6f}", source,
            amplitude, phase_std, amp_ma, phase_ma,
            phase_deriv, snr, backlog, iat_ms 
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

# ===== CSI Receiver (UDP) =====
def csi_receiver():
    global last_ampl_frame, last_phase_frame, core_ampl_frames, core_phase_frames, prev_frames
    global last_recv_time
    
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(('0.0.0.0', CSI_PORT))
    print(f"[CSI] Waiting for UDP packets on port {CSI_PORT}...")

    moving_amp = collections.deque(maxlen=MA_WINDOW)
    moving_phase = collections.deque(maxlen=MA_WINDOW)
    
    global last_recv_time
    last_recv_time = time.time() 

    while True:
        try:
            data, addr = sock.recvfrom(8192)
            now = time.time() 

            # --- IAT CALCULATION (for logging) ---
            iat_seconds = now - last_recv_time
            last_recv_time = now 
            instant_iat_ms = iat_seconds * 1000.0 
            
            with lock:
                iat_instant_data.append((now, instant_iat_ms)) 

            s = data.decode(errors='ignore').strip()
            
            if not s:
                continue
            
            parts = s.split(',')
            if len(parts) < 3 + 2 * N_SUB:
                continue

            try:
                seq_num = int(parts[0]) 
                rxcore = int(parts[1])
            except:
                continue
            if rxcore < 0 or rxcore >= N_CORES:
                continue

            try:
                reals = np.array([float(parts[3 + 2*i]) for i in range(N_SUB)])
                imags = np.array([float(parts[4 + 2*i]) for i in range(N_SUB)])
            except Exception as e:
                continue

            # --- CSI Processing ---
            csi = reals + 1j * imags
            csi = np.fft.fftshift(csi)

            mags = np.abs(csi)
            avg = np.mean(mags[mags != 0]) if np.any(mags != 0) else 0.0

            phase = np.unwrap(np.angle(csi))
            k = np.arange(len(phase))
            if len(k) > 1:
                coeffs = np.polyfit(k, phase, 1)
                resid = phase - np.polyval(coeffs, k)
            else:
                resid = phase.copy()

            std = np.std(resid)

            moving_amp.append(avg)
            moving_phase.append(std)
            avg_ma = np.mean(moving_amp) if moving_amp else avg
            std_ma = np.mean(moving_phase) if moving_phase else std

            # phase derivative
            phase_deriv = 0
            with lock:
                if phase_data:
                    dt_phase = now - phase_data[-1][0]
                    if dt_phase > 0:
                        phase_deriv = (std - phase_data[-1][1]) / dt_phase
                
                # --- Variance calculation (for plotting) ---
                if rxcore == 0:
                    core0_ampl_history.append(mags.copy())
                    core0_phase_history.append(resid.copy())
                    if len(core0_ampl_history) >= 2:
                        ampl_variances = np.var(np.array(core0_ampl_history), axis=0)
                        phase_variances = np.var(np.array(core0_phase_history), axis=0)
                        mean_ampl_var = np.mean(ampl_variances)
                        mean_phase_var = np.mean(phase_variances)
                        core0_ampl_var.append((now, mean_ampl_var))
                        core0_phase_var.append((now, mean_phase_var))
                
                elif rxcore == 1:
                    core1_ampl_history.append(mags.copy())
                    core1_phase_history.append(resid.copy())
                    if len(core1_ampl_history) >= 2:
                        ampl_variances = np.var(np.array(core1_ampl_history), axis=0)
                        phase_variances = np.var(np.array(core1_phase_history), axis=0)
                        mean_ampl_var = np.mean(ampl_variances)
                        mean_phase_var = np.mean(phase_variances)
                        core1_ampl_var.append((now, mean_ampl_var))
                        core1_phase_var.append((now, mean_phase_var))

                # append per-core data for subplots and shared metrics
                ampl_data.append((now, avg))
                phase_data.append((now, std))
                ampl_ma_data.append((now, avg_ma))
                phase_ma_data.append((now, std_ma))
                phase_deriv_data.append((now, phase_deriv))

                # update per-core frames
                core_ampl_frames[rxcore] = mags.copy()
                core_phase_frames[rxcore] = resid.copy()

                last_ampl_frame = mags.copy()
                last_phase_frame = resid.copy()

            # Log CSI line
            log_data("CSI", avg, std, avg_ma, std_ma, phase_deriv,
                        iat_ms=instant_iat_ms, ampl_sc=mags, phase_sc=resid)

        except Exception as e:
            print(f"[CSI] Error: {e}")
            break

# ===== Queue Receiver (TCP) =====
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
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(('', QUEUE_PORT))
    sock.listen(1)
    print(f"[Queue] Waiting for TCP connection on port {QUEUE_PORT}...")
    conn, addr = sock.accept()
    print(f"[Queue] Connected from {addr}")

    buffer = ""
    while True:
        try:
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
        except Exception as e:
            print(f"[Queue] Error: {e}")
            break

# ===== Plot Setup (MODIFIED for TOPTWO mode) =====

N_PLOTS = 0
if PLOT_MODE == "ALL":
    N_PLOTS = 4 # Variance, Queue, Amp/SC, Phase/SC
    fig = plt.figure(figsize=(12, 10)) 
    gs = gridspec.GridSpec(N_PLOTS, 1, height_ratios=[1, 1, 1, 1], hspace=0.3) 
elif PLOT_MODE == "TOPTWO":
    N_PLOTS = 2 # Variance, Queue
    fig = plt.figure(figsize=(12, 6))
    gs = gridspec.GridSpec(N_PLOTS, 1, height_ratios=[1, 1], hspace=0.3)
else:
    N_PLOTS = 1 # Variance only
    fig = plt.figure(figsize=(12, 5))
    gs = gridspec.GridSpec(N_PLOTS, 1, height_ratios=[1], hspace=0.3)

subcarrier_idx = np.arange(-N_SUB//2, N_SUB//2)
guard_mask = ((subcarrier_idx >= -32) & (subcarrier_idx <= -29)) | \
             (subcarrier_idx == 0) | \
             ((subcarrier_idx >= 29) & (subcarrier_idx <= 31))

# --- First plot: Variance indicators (ax0) ---
ax0 = fig.add_subplot(gs[0]) 
ax0.set_title("Mean Variance of Last 10 Packets (Core 0 & Core 1) - Raw Values")
ax0.set_ylabel("Amplitude Variance")
ax0.grid(True)
ax0_phase = ax0.twinx()
ax0_phase.set_ylabel("Phase Variance")

line_core0_ampl_var, = ax0.plot([], [], 'b-', label="Core 0 Amplitude Var", linewidth=2)
line_core1_ampl_var, = ax0.plot([], [], 'r-', label="Core 1 Amplitude Var", linewidth=2)
line_core0_phase_var, = ax0_phase.plot([], [], 'c--', label="Core 0 Phase Var", linewidth=1)
line_core1_phase_var, = ax0_phase.plot([], [], 'm--', label="Core 1 Phase Var", linewidth=1)

ax0.legend(loc='upper left')
ax0_phase.legend(loc='upper right')

# Initialize other axes to None
ax1, ax2, ax3 = None, None, None
line_snr, line_backlog = None, None
line_sub_ampl, line_sub_phase = [], []

# --- Second plot (Queue Data) only if N_PLOTS >= 2 ---
if N_PLOTS >= 2:
    ax1 = fig.add_subplot(gs[1], sharex=ax0) 
    ax1.set_title("Queue Data")
    ax1.set_ylabel("SNR (dB)")
    ax1.grid(True)
    ax1_backlog = ax1.twinx()
    ax1_backlog.set_ylabel("Backlog")
    line_snr, = ax1.plot([], [], 'y-', label="SNR")
    line_backlog, = ax1_backlog.plot([], [], 'g-', label="Backlog")
    ax1.legend(loc='upper left')
    ax1_backlog.legend(loc='upper right')

# --- Subcarrier plots only if N_PLOTS >= 4 (i.e., PLOT_MODE == "ALL") ---
if N_PLOTS >= 4:
    ax2 = fig.add_subplot(gs[2])             
    ax3 = fig.add_subplot(gs[3])             

    ax2.set_title("Amplitude per Subcarrier (current frame) - 4 RX cores")
    ax2.set_ylabel("Amplitude")
    ax2.set_xlabel("Subcarrier Index")
    ax2.grid(True)

    ax3.set_title("Phase Residual per Subcarrier (current frame) - 4 RX cores")
    ax3.set_ylabel("Phase Residual [rad]")
    ax3.set_xlabel("Subcarrier Index")
    ax3.grid(True)

    colors = ['b','r','g','m']
    for c in range(N_CORES):
        ln_a, = ax2.plot([], [], marker='o', linestyle='-', lw=1, color=colors[c], label=f"Core {c}")
        ln_p, = ax3.plot([], [], marker='o', linestyle='-', lw=1, color=colors[c], label=f"Core {c}")
        line_sub_ampl.append(ln_a)
        line_sub_phase.append(ln_p)
    ax2.legend(loc='upper right')
    ax3.legend(loc='upper right')

# ===== Update Function =====
def update(frame):
    with lock:
        all_timestamps = []
        for dq in (core0_ampl_var, core0_phase_var, core1_ampl_var, core1_phase_var):
            all_timestamps.extend(t for t, _ in dq)
        
        # Only include queue data timestamps if we are plotting them
        if N_PLOTS >= 2:
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

        # --- Variance plot (ax0) ---
        x_c0a, y_c0a = xy(core0_ampl_var)
        x_c0p, y_c0p = xy(core0_phase_var)
        x_c1a, y_c1a = xy(core1_ampl_var)
        x_c1p, y_c1p = xy(core1_phase_var)

        line_core0_ampl_var.set_data(x_c0a, y_c0a)
        line_core0_phase_var.set_data(x_c0p, y_c0p)
        line_core1_ampl_var.set_data(x_c1a, y_c1a)
        line_core1_phase_var.set_data(x_c1p, y_c1p)

        artists_to_return = [line_core0_ampl_var, line_core0_phase_var, line_core1_ampl_var, line_core1_phase_var]

        # --- Queue Data (ax1) only if N_PLOTS >= 2 ---
        if N_PLOTS >= 2:
            xs, ys = xy(snr_data)
            xb, yb = xy(backlog_data)
            line_snr.set_data(xs, ys)
            line_backlog.set_data(xb, yb)
            artists_to_return.extend([line_snr, line_backlog])
        
        # --- Subcarrier Plots (ax2, ax3) only if N_PLOTS >= 4 (ALL mode) ---
        if N_PLOTS >= 4:
            for c in range(N_CORES):
                ampl_plot = core_ampl_frames[c].copy()
                phase_plot = core_phase_frames[c].copy()
                if np.all(ampl_plot == 0):
                    ampl_plot = np.full_like(ampl_plot, np.nan)
                if np.all(phase_plot == 0):
                    phase_plot = np.full_like(phase_plot, np.nan)

                ampl_plot[guard_mask] = np.nan
                phase_plot[guard_mask] = np.nan

                line_sub_ampl[c].set_data(subcarrier_idx, ampl_plot)
                line_sub_phase[c].set_data(subcarrier_idx, phase_plot)
                artists_to_return.extend([line_sub_ampl[c], line_sub_phase[c]])

        # --- Axis scaling (10s TIME WINDOW) ---
        # Keep x window to last 10 seconds of data (or from 0)
        ax0.set_xlim(max(0, tmax - t0 - PLOT_TIME_WINDOW), tmax - t0 + 1)
        
        # Auto-scale the Y-axes for Variance
        ax0.relim(); ax0.autoscale_view(True, True, True)
        ax0_phase.relim(); ax0_phase.autoscale_view(True, True, True)

        if N_PLOTS >= 2:
            # Auto-scale Queue plot (ax1)
            ax1.relim(); ax1.autoscale_view(True, True, True)
            ax1_backlog.relim(); ax1_backlog.autoscale_view(True, True, True)
        
        if N_PLOTS >= 4:
            # Auto-scale Subcarrier plots (ax2, ax3)
            for axis in (ax2, ax3):
                axis.relim()
                axis.autoscale_view(True, True, True)

    return artists_to_return

# ===== Start Threads and Animation (1ms Interval) =====
threading.Thread(target=csi_receiver, daemon=True).start()
threading.Thread(target=queue_receiver, daemon=True).start()

# Setting interval to 1ms for near instantaneous updates
ani = FuncAnimation(fig, update, interval=1, blit=False) 
plt.tight_layout()
plt.show()