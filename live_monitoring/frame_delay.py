from scapy.all import sniff, UDP
import time
import matplotlib.pyplot as plt
from matplotlib.animation import FuncAnimation
import collections
import threading
import queue

# --- Frame tracking ---
last_rtp_ts = {}
last_frame_time = {}
frames_in_second = 0
last_second = time.time()

# --- SSRC tracking ---
ssrc_counts = {}
main_ssrc = None

# --- Data for plotting ---
MAX_FRAMES = 100
frame_delays = collections.deque(maxlen=MAX_FRAMES)
timestamps = collections.deque(maxlen=MAX_FRAMES)

# --- Queue for thread-safe plot updates ---
frame_queue = queue.Queue()

# --- RTP parsing ---
def parse_rtp(raw):
    if len(raw) < 12:
        return None
    b = raw[:12]
    seq = int.from_bytes(b[2:4], byteorder='big')
    ts = int.from_bytes(b[4:8], byteorder='big')
    ssrc = int.from_bytes(b[8:12], byteorder='big')
    return {"seq": seq, "ts": ts, "ssrc": ssrc}

# --- Packet handler ---
def packet_handler(pkt):
    global last_rtp_ts, last_frame_time, frames_in_second, last_second, ssrc_counts, main_ssrc

    if UDP in pkt:
        udp_payload = bytes(pkt[UDP].payload)
        rtp_info = parse_rtp(udp_payload)
        if rtp_info:
            ssrc = rtp_info['ssrc']
            ts = rtp_info['ts']
            now = time.time()

            # Count only new timestamps
            if last_rtp_ts.get(ssrc) != ts:
                last_rtp_ts[ssrc] = ts
                frames_in_second += 1

                # Update SSRC counts
                ssrc_counts[ssrc] = ssrc_counts.get(ssrc, 0) + 1
                main_ssrc = max(ssrc_counts, key=ssrc_counts.get)

                # Calculate frame delay for main SSRC only
                if ssrc == main_ssrc:
                    if ssrc in last_frame_time:
                        delta = (now - last_frame_time[ssrc]) * 1000  # ms
                        frame_queue.put((now, delta))
                        print(f"SSRC={ssrc} TS={ts}, Frame duration={delta:.3f} ms")
                    else:
                        print(f"SSRC={ssrc} TS={ts}, first frame")
                    last_frame_time[ssrc] = now

    # One-second reporting
    now_sec = time.time()
    if now_sec - last_second >= 1.0:
        print(f"Frames in last 1 second (main SSRC={main_ssrc}): {frames_in_second}")
        frames_in_second = 0
        last_second = now_sec

# --- Matplotlib setup ---
fig, ax = plt.subplots()
ax.set_xlabel("Time (s)")
ax.set_ylabel("Frame delay (ms)")
ax.set_title("Live Frame Delay")
ax.set_ylim(0, 200)
ax.grid(True)
line, = ax.plot([], [], marker='o', linestyle='-')

# --- Animation update ---
start_time = time.time()
def update_plot(_):
    while not frame_queue.empty():
        t, d = frame_queue.get()
        timestamps.append(t - start_time)
        frame_delays.append(d)
    line.set_data(list(timestamps), list(frame_delays))
    ax.relim()
    ax.autoscale_view(scaley=False)
    return line,

ani = FuncAnimation(fig, update_plot, interval=100)  # update every 100ms

# --- Sniffer thread ---
sniffer_thread = threading.Thread(
    target=lambda: sniff(
        iface="enp0s25",            # Change to your interface
        prn=packet_handler,
        filter="udp and dst host 192.168.1.219",  # Change to your filter
        store=0
    ),
    daemon=True
)
sniffer_thread.start()

plt.show()
