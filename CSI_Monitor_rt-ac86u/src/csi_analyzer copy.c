#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <math.h>

#define PORT 5500
#define BUF_SIZE 65535
#define NFFT 64  // number of subcarriers
#define k_tof_unpack_sgn_mask (1<<31)

(274 bytes) Received on Port:
Compare with Matlab results
11 11 11 11 04 33 c2 27 31 ed ff ff 00 00 0a 10
6a 00 af a3 81 11 b5 a2 4d 38 36 83 b9 26 36 a7
6d 02 f6 75 d9 09 f6 3d 91 0e 76 02 bd 13 f6 c1
4c 17 76 65 18 1a b6 08 88 1b 36 5a 56 1b b6 be
52 19 36 0e ef 16 36 6a 93 12 76 a4 27 0d f6 ae
bb 05 b6 a2 c7 20 36 7b bb 26 b6 37 bf 2b 35 c7
bf 3d f6 a2 2a 32 36 47 f6 33 36 11 b0 34 36 7c
5c 33 f6 e0 3c 30 f6 4d d1 2a 76 af 9d 21 f0 25
33 29 b1 1b 0c 38 af e9 c1 08 ef 5d 5d 24 ee a3
01 00 af 17 7f 31 af 17 9f 3e 2e 00 7c 31 2f 00
de 15 f1 80 27 05 af 87 61 12 35 72 81 11 b5 d9
78 16 b5 53 b0 17 75 27 6e 18 75 92 6e 16 75 fc
92 12 35 49 d3 0d f5 73 63 08 f5 be 77 03 b5 fc
53 22 b6 02 e3 25 b5 f0 2f 34 b5 cd 77 3c f6 b3
7e 31 b6 75 b6 33 b6 21 aa 34 36 26 8c 35 36 6b
5c 34 f6 a7 1c 33 76 ed 44 30 b6 22 21 2d 36 3f
ed 27 36 70 e1 22 b6 8e 4d 04 b6 4e f5 0b 75 21
ed 1e


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
    uint32_t iq_mask = (1 << (nman - 1)) - 1;  // Fixed: nman-1, not nman
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
            int e_start = e;
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
            if (e > maxbit) {
                maxbit = e;
                if (i < 10) {  // Debug first 10
                    printf("DEBUG: i=%d vi=%u vq=%u x_orig=%u e_start=%d e_final=%d\n", 
                           i, (uint32_t)vi, (uint32_t)vq, (uint32_t)vi | (uint32_t)vq, e_start, e);
                }
            }
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
    printf("DEBUG: maxbit=%d shft=%d e_zero=%d\n", maxbit, shft, e_zero);
    printf("DEBUG: He[0]=%d He[1]=%d\n", He[0], He[1]);
    for (int i = 0; i < n_out; i++) {
        int e = He[(i >> e_shift)] + shft;
        int32_t vi = *pOut;
        int sgn = 1;
        
        if (i < 4) {  // Debug first few
            printf("DEBUG: i=%d He=%d e=%d vi_before_sign=%d\n", 
                   i, He[(i >> e_shift)], e, vi);
        }
        
        if (vi & k_tof_unpack_sgn_mask) {
            sgn = -1;
            vi &= ~k_tof_unpack_sgn_mask;
        }
        
        if (i < 4) {  // Debug first few
            printf("DEBUG: i=%d sgn=%d vi_after_mask=%d e=%d e_zero=%d\n", 
                   i, sgn, vi, e, e_zero);
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
    uint16_t magic;
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

    printf("Listening for Nexmon CSI packets on UDP port %d...\n", PORT);

    while (1) {
        len = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr*)&src, &srclen);
        if (len < (ssize_t)sizeof(csi_header_t)) continue;

        csi_header_t *h = (csi_header_t*)buf;
        if (ntohs(h->magic) != 0x1111) continue;

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

        // Debug: print first few raw values and bit breakdown
        printf("\nRaw H[0]=0x%08x\n", Hraw[0]);
        {
            uint32_t h = Hraw[0];
            uint32_t exp_raw = h & 0x3F;
            uint32_t imag_raw = (h >> 6) & 0x7FF;
            uint32_t sign_i = (h >> 17) & 1;
            uint32_t real_raw = (h >> 18) & 0x7FF;
            uint32_t sign_r = (h >> 29) & 1;
            int exp_signed = exp_raw >= 32 ? exp_raw - 64 : exp_raw;
            printf("  Breakdown: exp=%d real=%u(%c) imag=%u(%c)\n",
                   exp_signed, real_raw, sign_r?'-':'+', imag_raw, sign_i?'-':'+');
        }

        int32_t Hout[NFFT*2];
        unpack_float_4366c0(NFFT, Hraw, Hout);
        
        printf("After unpack: Hout[0]=%d Hout[1]=%d\n", Hout[0], Hout[1]);

        double avg_amp = 0.0;
        for (int i = 0; i < NFFT; i++) {
            double re = (double)Hout[2*i];
            double im = (double)Hout[2*i+1];
            double mag = sqrt(re*re + im*im);
            avg_amp += mag;
            printf("Subcarrier %2d: Re=%.2f Im=%.2f Mag=%.2f\n", i, re, im, mag);
        }
        avg_amp /= NFFT;

        printf("Seq=%u | Core=%d | Spatial=%d | ChanSpec=0x%04x | Subcarriers=%d | AvgAmp=%.2f\n\n",
               seq, core, stream, ntohs(h->chanspec), NFFT, avg_amp);
        fflush(stdout);
    }

    close(sock);
    return 0;
}