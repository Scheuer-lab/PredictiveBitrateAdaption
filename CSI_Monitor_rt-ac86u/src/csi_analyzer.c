/* csi_sender_tcp.c
   Sends all subcarriers' raw complex CSI values (re + im)
   to Python receiver at DATA_IP:DATA_PORT via TCP.
*/

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <math.h>
#include <complex.h>
#include <errno.h>

#define PORT 5500
#define BUF_SIZE 65535
#define NFFT 64
#define k_tof_unpack_sgn_mask (1<<31)

// Control message configuration
#define CONTROL_IP "192.168.1.1"
#define CONTROL_PORT 9999

// Data (raw CSI) destination (Python listener)
#define DATA_IP "192.168.1.2"
#define DATA_PORT 12346

//#define K_TOF_UNPACK_SGN_MASK (1 << 31)  // same as 1<<11 for 12-bit numbers

#include <stdio.h>
#include <stdint.h>
#include <math.h>

#define K_TOF_UNPACK_SGN_MASK (1u<<31)

void unpack_float_double(int nfft, uint32_t *H, double *Hout_re, double *Hout_im) {
    int nman = 12;
    int nexp = 6;
    int e_p = (1 << (nexp - 1));
    int nbits = 10;
    int autoscale = 1;

    int8_t He[256];
    int maxbit = -e_p;
    int e_zero = -nman;

    //printf("Subc | Hraw(hex)  | vi | vq | e(orig) | e_shifted | RE | IM\n");
    //printf("------------------------------------------------------------\n");

    /* masks & sign-bit detection same as mex */
    uint32_t iq_mask = (1u << (nman - 1)) - 1u;     // 11-bit mask (0x7FF)
    uint32_t e_mask  = (1u << nexp) - 1u;           // exponent mask
    uint32_t sgnr_mask = (1u << (nexp + 2 * nman - 1)); // bit that indicates sign of real in packed word
    uint32_t sgni_mask = (sgnr_mask >> nman);           // sign bit for imag

    /* First pass: extract vi/vq (unsigned 11 bits), exponent, compute He & maxbit (autoscale) */
    for (int i = 0; i < nfft; ++i) {
        uint32_t Hval = H[i];

        /* Extract 11-bit mantissas in the same bit positions as the .mex code */
        int32_t vi = (int32_t)((Hval >> (nexp + nman)) & iq_mask);  // real mantissa raw (0..0x7FF)
        int32_t vq = (int32_t)((Hval >> nexp) & iq_mask);          // imag mantissa raw (0..0x7FF)

        /* exponent in low bits (signed two's complement if >= e_p) */
        int e = (int)(Hval & e_mask);
        if (e >= e_p) e -= (e_p << 1);
        He[i] = (int8_t)e;

        /* autoscale: use absolute magnitude of mantissas (but mantissas still unsigned here) */
        if (autoscale) {
            uint32_t x = (uint32_t)vi | (uint32_t)vq; // still unsigned fields
            if (x) {
                uint32_t m = 0xffff0000u;
                uint32_t b = 0xffffu;
                int s = 16;
                int tmp_e = e;
                while (s > 0) {
                    if (x & m) {
                        tmp_e += s;
                        x >>= s;
                    }
                    s >>= 1;
                    m = (m >> s) & b;
                    b >>= s;
                }
                if (tmp_e > maxbit) maxbit = tmp_e;
            }
        }
    }

    int shft = nbits - maxbit;

    /* Second pass: perform sign marking, sign extraction and shifting identical to .mex */
    for (int i = 0; i < nfft; ++i) {
        uint32_t Hval = H[i];
        int e_scaled = He[i] + shft;

        /* re/im raw (unsigned 11-bit) */
        int32_t re = (int32_t)((Hval >> (nexp + nman)) & iq_mask);
        int32_t im = (int32_t)((Hval >> nexp) & iq_mask);

        /* mark sign bits in same way as mex: set high-bit sentinel if packed sign bit present */
        if (Hval & sgnr_mask) re |= (int32_t)K_TOF_UNPACK_SGN_MASK;
        if (Hval & sgni_mask) im |= (int32_t)K_TOF_UNPACK_SGN_MASK;

        /* now perform sign extraction & shifting exactly like mex's second loop */
        int sgn = 1;
        if (re & (int32_t)K_TOF_UNPACK_SGN_MASK) {
            sgn = -1;
            re &= ~(int32_t)K_TOF_UNPACK_SGN_MASK;
        }
        if (e_scaled < e_zero) {
            re = 0;
        } else if (e_scaled < 0) {
            re = (re >> (-e_scaled));
        } else {
            re = (re << e_scaled);
        }
        re = sgn * re;

        sgn = 1;
        if (im & (int32_t)K_TOF_UNPACK_SGN_MASK) {
            sgn = -1;
            im &= ~(int32_t)K_TOF_UNPACK_SGN_MASK;
        }
        if (e_scaled < e_zero) {
            im = 0;
        } else if (e_scaled < 0) {
            im = (im >> (-e_scaled));
        } else {
            im = (im << e_scaled);
        }
        im = sgn * im;

        Hout_re[i] = (double)re;
        Hout_im[i] = (double)im;

        //printf("[%2d] 0x%08X  %10d  %10d  %3d      %3d  %12.6f  %12.6f\n",
        //       i, Hval, re, im, He[i], e_scaled, Hout_re[i], Hout_im[i]);
    }
}


// --- Fixed unpack function (same as before) ---
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

    
     printf("Subc | Hraw(hex)  | vi        | vq        | e(before) | e_scaled | RE           | IM\n");
    printf("----------------------------------------------------------------------------------------\n");

    
    for (int i = 0; i < nfft; i++) {
        int32_t vi = (int32_t)((H[i] >> (nexp + nman)) & iq_mask);
        int32_t vq = (int32_t)((H[i] >> nexp) & iq_mask);
        int e = (int)(H[i] & e_mask);
        if (e >= e_p)
            e -= (e_p << 1);
        He[i] = (int8_t)e;
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
        if (H[i] & sgnr_mask)
            vi |= k_tof_unpack_sgn_mask;
        if (H[i] & sgni_mask)
            vq |= k_tof_unpack_sgn_mask;
        Hout[i << 1] = vi;
        Hout[(i << 1) + 1] = vq;
    }
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
            vi = (vi >> (-e));
        } else {
            vi = (vi << e);
        }
        *pOut++ = (int32_t)(sgn * vi);
    }
}

// --- CSI header struct ---
typedef struct __attribute__((__packed__)) {
    uint32_t magic;
    uint8_t  src_mac[6];
    uint16_t seq;
    uint16_t core_stream;
    uint16_t chanspec;
    uint16_t chipver;
} csi_header_t;

int main() {

    //Testing dummy csi:
    /*
    uint32_t H_test[] = {
        960268017,295920246,222781046,155145782,82261942,33758582,555686966,
        596316022,742915893,828096053,864259381,943412661,969347573,
        1022281973,1051357877,1014630005,1004288757,968921141,917029941,
        866976309,807485749,712333685,614563253,555063093,108357941,
        252017269,439692981,312487222,331509430,716933040,282379248,
        639837681,1024565424,960511345,179906032,205589872,323197558,
        56608758,698580662,797639542,832739190,859189046,863249654,
        834956150,1053911477,966642677,681380854,642848502,564469622,
        29173558,77407414,131663798,333572981,409576181,465395189,
        513609653,275017014,286539830,296492598,289543542,326774390,
        366893430,380022838,383708278
    };
    
    int32_t Hout_test[NFFT*2];

    // 🚀 USE REAL UNPACK FUNCTION
    //unpack_float_4366c0(NFFT, H_test, Hout_test);
    // print results
    double Hout_re[NFFT], Hout_im[NFFT];

    unpack_float_double(NFFT, H_test, Hout_re, Hout_im);

    for (int i = 0; i < NFFT; i++) {
        double mag = sqrt(Hout_re[i]*Hout_re[i] + Hout_im[i]*Hout_im[i]);
        double ph  = atan2(Hout_im[i], Hout_re[i]);
        printf("[%02d] RE = %10.4f   IM = %10.4f   MAG = %10.4f   PHASE = %.4f rad\n",
               i, Hout_re[i], Hout_im[i], mag, ph);
    }

    return 0;

    */

    int sock;
    struct sockaddr_in addr, src;
    socklen_t srclen = sizeof(src);
    uint8_t buf[BUF_SIZE];
    ssize_t len;

    // UDP socket to receive CSI from Nexmon
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

    // --- TCP socket to Python ---
    int data_sock = socket(AF_INET, SOCK_STREAM, 0); // TCP socket
    if (data_sock < 0) { perror("data socket creation failed"); return 1; }

    struct sockaddr_in data_addr;
    memset(&data_addr, 0, sizeof(data_addr));
    data_addr.sin_family = AF_INET;
    data_addr.sin_port = htons(DATA_PORT);
    inet_pton(AF_INET, DATA_IP, &data_addr.sin_addr);

    if (connect(data_sock, (struct sockaddr*)&data_addr, sizeof(data_addr)) < 0) {
        perror("connect failed");
        return 1;
    }

    printf("Listening for Nexmon CSI packets on UDP port %d...\n", PORT);
    printf("Sending CSI (64 carriers) to %s:%d via TCP\n", DATA_IP, DATA_PORT);

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
        if (payload_len < NFFT*4) continue;

        uint32_t Hraw[NFFT];
        memcpy(Hraw, payload, NFFT * sizeof(uint32_t));

        int32_t Hout[NFFT*2];
        unpack_float_4366c0(NFFT, Hraw, Hout);

        // zero guard subcarriers
        for (int i = 0; i <= 3; i++) { Hout[2*i]=0; Hout[2*i+1]=0; }
        Hout[2*32]=0; Hout[2*32+1]=0;
        for (int i = 62; i <= 63; i++){ Hout[2*i]=0; Hout[2*i+1]=0; }

        // Build CSV message: seq,core,stream,re0,im0,re1,im1,...
        char msg[4096];
        int pos = snprintf(msg, sizeof(msg), "%u,%d,%d", seq, core, stream);
        for (int i=0; i<NFFT; i++){
            double re = (double)Hout[2*i];
            double im = (double)Hout[2*i+1];
            pos += snprintf(msg+pos, sizeof(msg)-pos, ",%.8f,%.8f", re, im);
            if (pos >= (int)sizeof(msg)-32) break;
        }
        msg[pos++] = '\n';
        send(data_sock, msg, pos, 0);  // TCP send
    }

    close(sock);
    close(data_sock);
    return 0;
}
