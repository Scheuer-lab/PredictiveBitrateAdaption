#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <math.h>
#include <complex.h>

#define PORT 5500
#define BUF_SIZE 65535
#define NFFT 64  // number of subcarriers
#define k_tof_unpack_sgn_mask (1<<31)

// Control message configuration
#define CONTROL_IP "192.168.2.10"  // IP of router running rtp_spoofer
#define CONTROL_PORT 9999
#define DC_AMPLITUDE_THRESHOLD 20.0  // Threshold for blocking detection

// Fixed 4366C0 unpack function (format=1, nman=12, nexp=6)
void unpack_float_4366c0(int nfft, uint32_t *H, int32_t *Hout) {
    int nbits = 10;
    int autoscale = 1;
    int nman = 12;
    int nexp = 6;
    
    int e_p = (1 << (nexp - 1));
    int maxbit = -e_p;
    int e_zero = -nman;
    int n_out = (nfft << 1);
    int e_shift = 1;
    
    int8_t He[256];
    uint32_t iq_mask = (1 << (nman - 1)) - 1;
    uint32_t e_mask = (1 << nexp) - 1;
    uint32_t sgnr_mask = (1 << (nexp + 2*nman - 1));
    uint32_t sgni_mask = (sgnr_mask >> nman);
    
    int32_t *pOut = Hout;
    
    // First pass: extract mantissas, exponents, and find maxbit
    for (int i = 0; i < nfft; i++) {
        int32_t vi = (int32_t)((H[i] >> (nexp + nman)) & iq_mask);
        int32_t vq = (int32_t)((H[i] >> nexp) & iq_mask);
        int e = (int)(H[i] & e_mask);
        
        if (e >= e_p)
            e -= (e_p << 1);
        
        He[i] = (int8_t)e;
        
        // Autoscaling: find maxbit BEFORE applying sign mask
        uint32_t x = (uint32_t)vi | (uint32_t)vq;
        if (autoscale && x) {
            uint32_t m = 0xffff0000, b = 0xffff;
            int s = 16;
            while (s > 0) {
                if (x & m) {
                    e += s;
                    x >>= s;
                }
                s >>= 1;
                m = (m >> s) & b;
                b >>= s;
            }
            if (e > maxbit)
                maxbit = e;
        }
        
        // Apply sign mask AFTER autoscaling
        if (H[i] & sgnr_mask)
            vi |= k_tof_unpack_sgn_mask;
        if (H[i] & sgni_mask)
            vq |= k_tof_unpack_sgn_mask;
        
        Hout[i << 1] = vi;
        Hout[(i << 1) + 1] = vq;
    }
    
    // Second pass: apply scaling
    int shft = nbits - maxbit;
    for (int i = 0; i < n_out; i++) {
        int e = He[(i >> e_shift)] + shft;
        int32_t vi = *pOut;
        int sgn = 1;
        
        if (vi & k_tof_unpack_sgn_mask) {
            sgn = -1;
            vi &= ~k_tof_unpack_sgn_mask;
        }
        
        if (e < e_zero) {
            vi = 0;
        } else if (e < 0) {
            e = -e;
            vi = (vi >> e);
        } else {
            vi = (vi << e);
        }
        
        *pOut++ = (int32_t)(sgn * vi);
    }
}

typedef struct __attribute__((__packed__)) {
    uint32_t magic;      // 4 bytes: 0x11111111
    uint8_t  src_mac[6];
    uint16_t seq;
    uint16_t core_stream;
    uint16_t chanspec;
    uint16_t chipver;
} csi_header_t;

int main() {
    int sock;
    struct sockaddr_in addr, src;
    socklen_t srclen = sizeof(src);
    uint8_t buf[BUF_SIZE];
    ssize_t len;

    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { perror("socket"); return 1; }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT);

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }

    // Setup control socket for sending messages to rtp_spoofer
    int control_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (control_sock < 0) {
        perror("control socket creation failed");
        return 1;
    }

    struct sockaddr_in control_addr;
    memset(&control_addr, 0, sizeof(control_addr));
    control_addr.sin_family = AF_INET;
    control_addr.sin_port = htons(CONTROL_PORT);
    if (inet_pton(AF_INET, CONTROL_IP, &control_addr.sin_addr) <= 0) {
        perror("invalid control IP address");
        return 1;
    }

    printf("Listening for Nexmon CSI packets on UDP port %d...\n", PORT);
    printf("Will send blocking alerts to %s:%d\n", CONTROL_IP, CONTROL_PORT);

    while (1) {
        len = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr*)&src, &srclen);
        if (len < (ssize_t)sizeof(csi_header_t)) continue;

        csi_header_t *h = (csi_header_t*)buf;
        if (ntohl(h->magic) != 0x11111111) continue;

        uint16_t seq = ntohs(h->seq);
        int core = ntohs(h->core_stream) & 0x7;
        int stream = (ntohs(h->core_stream) >> 3) & 0x7;

        uint8_t *payload = buf + sizeof(csi_header_t);
        size_t payload_len = len - sizeof(csi_header_t);

        if (payload_len < NFFT*4) {
            printf("[SKIP] payload too small: %zu\n", payload_len);
            continue;
        }

        uint32_t Hraw[NFFT];
        for (int i = 0; i < NFFT; i++) {
            Hraw[i] = ((uint32_t*)payload)[i];
        }

        int32_t Hout[NFFT*2];
        unpack_float_4366c0(NFFT, Hraw, Hout);

        // Zero out guard subcarriers: 0-3, 32, 62-63
        for (int i = 0; i <= 3; i++) {
            Hout[2*i] = 0;
            Hout[2*i+1] = 0;
        }
        Hout[2*32] = 0;
        Hout[2*32+1] = 0;
        for (int i = 62; i <= 63; i++) {
            Hout[2*i] = 0;
            Hout[2*i+1] = 0;
        }

        // Debug: Check if first few raw values match expected format
        // static int debug_count = 0;
        // if (debug_count < 1) {
        //     printf("  DEBUG: Raw H values: 0x%08x 0x%08x 0x%08x 0x%08x\n", 
        //            Hraw[0], Hraw[1], Hraw[2], Hraw[3]);
        //     debug_count++;
        // }

        // Convert to complex doubles (matching MATLAB: cmplx = double(Hout(:,1))+1j*double(Hout(:,2)))
        double complex csi[NFFT];
        double avg_amp = 0.0;
        for (int i = 0; i < NFFT; i++) {
            double re = (double)Hout[2*i];
            double im = (double)Hout[2*i+1];
            csi[i] = re + im * I;  // Create complex number
            double mag = sqrt(re*re + im*im);
            avg_amp += mag;
        }
        avg_amp /= NFFT;

        // Calculate average amplitude around DC (subcarriers 29-35, excluding 32)
        // DC subcarrier (32) is already zeroed, so we check 29,30,31,33,34,35
        double dc_amp_sum = 0.0;
        int dc_count = 0;
        int dc_indices[] = {29, 30, 31, 33, 34, 35};
        for (int i = 0; i < 6; i++) {
            int idx = dc_indices[i];
            double re = (double)Hout[2*idx];
            double im = (double)Hout[2*idx+1];
            double mag = sqrt(re*re + im*im);
            dc_amp_sum += mag;
            dc_count++;
        }
        double dc_avg_amp = dc_amp_sum / dc_count;

        // Check if DC amplitude is below threshold (blocking detected)
        if (dc_avg_amp < DC_AMPLITUDE_THRESHOLD) {
            const char *msg = "BLOCKING_DETECTED";
            ssize_t sent = sendto(control_sock, msg, strlen(msg), 0,
                                 (struct sockaddr *)&control_addr, sizeof(control_addr));
            if (sent < 0) {
                perror("sendto control socket failed");
            } else {
                printf("*** BLOCKING DETECTED (DC_Amp=%.2f < %.2f) - Alert sent to %s:%d ***\n",
                       dc_avg_amp, DC_AMPLITUDE_THRESHOLD, CONTROL_IP, CONTROL_PORT);
            }
        }

        printf("Seq=%u | Core=%d | Spatial=%d | ChanSpec=0x%04x | AvgAmp=%.2f | DC_Amp=%.2f\n",
               seq, core, stream, ntohs(h->chanspec), avg_amp, dc_avg_amp);
        
        // // Print entire UDP payload in hex for comparison with tcpdump/pcap
        // printf("  UDP Payload (hex, %zu bytes):\n", (size_t)len);
        // for (size_t i = 0; i < (size_t)len; i++) {
        //     printf("%02x ", buf[i]);
        //     if ((i + 1) % 16 == 0) printf("\n");
        // }
        // if (len % 16 != 0) printf("\n");
        
        // Optional: print first few subcarriers for verification
        // printf("  First 4 subcarriers (int): ");
        // for (int i = 0; i < 4; i++) {
        //     printf("[%d: %d,%d] ", i, Hout[2*i], Hout[2*i+1]);
        // }
        // printf("\n");
        
        // Optional: print as complex doubles (like MATLAB)
        // printf("  First 4 subcarriers (complex): ");
        // for (int i = 0; i < 4; i++) {
        //     printf("[%d: %.0f%+.0fi] ", i, creal(csi[i]), cimag(csi[i]));
        // }
        // printf("\n");
        
        fflush(stdout);
    }

    close(sock);
    close(control_sock);
    return 0;
}