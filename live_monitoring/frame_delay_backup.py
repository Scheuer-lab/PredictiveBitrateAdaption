from scapy.all import sniff, UDP
from datetime import datetime
import time

# Dictionary für letzte RTP-Timestamps pro SSRC
last_rtp_ts = {}
# Dictionary für Zeit des letzten Frames pro SSRC
last_frame_time = {}
# Anzahl der Frames in der aktuellen Sekunde
frames_in_second = 0
# Zeitstempel der letzten Sekunde
last_second = time.time()

def parse_rtp(raw):
    """
    Parse RTP header from raw UDP payload
    Returns a dict with pt, seq, ts, ssrc
    """
    if len(raw) < 12:
        return None
    b = raw[:12]
    seq = int.from_bytes(b[2:4], byteorder='big')
    ts = int.from_bytes(b[4:8], byteorder='big')
    ssrc = int.from_bytes(b[8:12], byteorder='big')
    return {
        "seq": seq,
        "ts": ts,
        "ssrc": ssrc
    }

def packet_handler(pkt):
    global last_rtp_ts, frames_in_second, last_second, last_frame_time

    if UDP in pkt:
        udp_payload = bytes(pkt[UDP].payload)
        rtp_info = parse_rtp(udp_payload)
        if rtp_info:
            ssrc = rtp_info['ssrc']
            ts = rtp_info['ts']
            now = time.time()

            # Nur zählen, wenn Timestamp neu ist
            if last_rtp_ts.get(ssrc) != ts:
                last_rtp_ts[ssrc] = ts
                frames_in_second += 1

                # Interframe-Differenz berechnen
                if ssrc in last_frame_time:
                    delta = (now - last_frame_time[ssrc]) * 1000  # ms
                    print(f"SSRC={ssrc} TS={ts}, Frame duration={delta:.3f} ms")
                else:
                    print(f"SSRC={ssrc} TS={ts}, first frame")
                
                last_frame_time[ssrc] = now

    # Prüfen, ob eine Sekunde vorbei ist
    now_sec = time.time()
    if now_sec - last_second >= 1.0:
        print(f"Frames in last 1 second: {frames_in_second}")
        frames_in_second = 0
        last_second = now_sec

# Sniffer starten
sniff(
    iface="Ethernet 2",          # Passe an deine Schnittstelle an
    prn=packet_handler,
    filter="udp and dst host 192.168.1.2",  # Filter für dein Ziel
    store=0
)
