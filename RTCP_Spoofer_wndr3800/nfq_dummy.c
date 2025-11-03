#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <arpa/inet.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <linux/types.h>
#include <linux/netfilter.h>
#include <libnetfilter_queue/libnetfilter_queue.h>
#include <time.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define RTCP_RR   201
#define RTCP_SR   200
#define RTCP_SDES 202

// Global flag for graceful shutdown
static volatile int keep_running = 1;

// RTCP Receiver Report structure (RFC 3550)
struct rtcp_rr_packet {
    uint8_t version_p_count;  // Version (2 bits) | P | RC (5 bits)
    uint8_t packet_type;      // 201 for RR
    uint16_t length;          // Length in 32-bit words - 1
    uint32_t ssrc;            // SSRC of sender
    uint32_t ssrc_1;          // SSRC of first source being reported
    uint32_t fraction_lost;   // Fraction lost (8 bits) + Cumulative packets lost (24 bits)
    uint32_t extended_high_seq; // Extended highest sequence number received
    uint32_t jitter;          // Interarrival jitter
    uint32_t lsr;             // Last SR timestamp
    uint32_t dlsr;            // Delay since last SR
};

// Operational modes
typedef enum {
    MODE_ACCEPT_ALL = 0,      // Accept all real RR packets
    MODE_REPLACE = 1,         // Drop real RR, inject fake instead
    MODE_BOTH = 2             // Accept real RR AND inject fake (debugging)
} operation_mode_t;

void signal_handler(int signum) {
    printf("\n\nReceived signal %d, shutting down gracefully...\n", signum);
    keep_running = 0;
}

// Enhanced checksum calculation with detailed debugging
static uint16_t calculate_ip_checksum_debug(struct iphdr *iph, const char *debug_prefix) {
    uint16_t *ip_data = (uint16_t *)iph;
    int ip_header_len = iph->ihl * 4;
    uint32_t sum = 0;
    
    printf("    %sCalculating IP checksum for %d words (%d bytes):\n", 
           debug_prefix, ip_header_len / 2, ip_header_len);
    
    for (int i = 0; i < ip_header_len / 2; i++) {
        uint16_t word = ntohs(ip_data[i]);
        
        // Skip the checksum field itself (word 5 in standard 20-byte header)
        if (i == 5) {
            printf("    %s  Word %2d: 0x%04X [checksum field - skipping]\n", debug_prefix, i, word);
            continue;
        }
        
        printf("    %s  Word %2d: 0x%04X", debug_prefix, i, word);
        sum += word;
        printf(" -> sum: 0x%08X", sum);
        
        // Handle carry
        while (sum >> 16) {
            sum = (sum & 0xFFFF) + (sum >> 16);
            printf(" -> carry: 0x%08X", sum);
        }
        printf("\n");
    }
    
    uint16_t result = ~sum;
    printf("    %sFinal sum: 0x%08X, One's complement: 0x%04X\n", debug_prefix, sum, result);
    return result;
}

// Calculate UDP checksum including pseudo-header
static uint16_t calculate_udp_checksum(struct iphdr *iph, struct udphdr *udph, 
                                      const unsigned char *payload, int payload_len) {
    uint32_t sum = 0;
    uint16_t *data;
    int i;
    
    printf("    [UDP CHECKSUM] Calculating UDP checksum:\n");
    
    // Pseudo-header: source IP (2 words)
    data = (uint16_t*)&iph->saddr;
    sum += ntohs(data[0]);
    sum += ntohs(data[1]);
    printf("    [UDP CHECKSUM]   Source IP: 0x%04X + 0x%04X -> sum: 0x%08X\n", 
           ntohs(data[0]), ntohs(data[1]), sum);
    
    // Pseudo-header: destination IP (2 words)
    data = (uint16_t*)&iph->daddr;
    sum += ntohs(data[0]);
    sum += ntohs(data[1]);
    printf("    [UDP CHECKSUM]   Dest IP: 0x%04X + 0x%04X -> sum: 0x%08X\n", 
           ntohs(data[0]), ntohs(data[1]), sum);
    
    // Pseudo-header: protocol and UDP length
    sum += IPPROTO_UDP;
    sum += udph->uh_ulen;
    printf("    [UDP CHECKSUM]   Protocol+Length: 0x%04X + 0x%04X -> sum: 0x%08X\n", 
           IPPROTO_UDP, ntohs(udph->uh_ulen), sum);
    
    // UDP header (excluding checksum field)
    sum += ntohs(udph->uh_sport);
    sum += ntohs(udph->uh_dport);
    sum += ntohs(udph->uh_ulen);
    printf("    [UDP CHECKSUM]   UDP header: 0x%04X + 0x%04X + 0x%04X -> sum: 0x%08X\n", 
           ntohs(udph->uh_sport), ntohs(udph->uh_dport), ntohs(udph->uh_ulen), sum);
    
    // UDP payload
    data = (uint16_t*)payload;
    for (i = 0; i < payload_len / 2; i++) {
        uint16_t word = ntohs(data[i]);
        sum += word;
        printf("    [UDP CHECKSUM]   Payload word %d: 0x%04X -> sum: 0x%08X\n", i, word, sum);
        
        // Handle carry
        while (sum >> 16) {
            sum = (sum & 0xFFFF) + (sum >> 16);
            printf("    [UDP CHECKSUM]     -> carry: 0x%08X\n", sum);
        }
    }
    
    // If payload length is odd, add the last byte
    if (payload_len % 2) {
        uint16_t last_byte = ((uint16_t)payload[payload_len - 1]) << 8;
        sum += last_byte;
        printf("    [UDP CHECKSUM]   Last byte: 0x%04X -> sum: 0x%08X\n", last_byte, sum);
        
        while (sum >> 16) {
            sum = (sum & 0xFFFF) + (sum >> 16);
            printf("    [UDP CHECKSUM]     -> carry: 0x%08X\n", sum);
        }
    }
    
    uint16_t result = ~sum;
    printf("    [UDP CHECKSUM] Final sum: 0x%08X, One's complement: 0x%04X\n", sum, result);
    return result;
}

// Send packet using raw socket
static int send_raw_packet(const unsigned char *packet_data, int packet_len) {
    int sockfd;
    struct sockaddr_in dest_addr;
    int bytes_sent;
    
    // Create raw socket
    sockfd = socket(AF_INET, SOCK_RAW, IPPROTO_RAW);
    if (sockfd < 0) {
        perror("socket");
        return -1;
    }
    
    // Tell kernel we're providing the IP header
    int one = 1;
    if (setsockopt(sockfd, IPPROTO_IP, IP_HDRINCL, &one, sizeof(one)) < 0) {
        perror("setsockopt");
        close(sockfd);
        return -1;
    }
    
    // Set up destination address
    struct iphdr *iph = (struct iphdr *)packet_data;
    memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_addr.s_addr = iph->daddr;
    
    // Send the packet
    bytes_sent = sendto(sockfd, packet_data, packet_len, 0,
                       (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    
    close(sockfd);
    
    if (bytes_sent < 0) {
        perror("sendto");
        return -1;
    }
    
    return bytes_sent;
}

// Print detailed RTCP RR information
static void print_rtcp_rr_details(const unsigned char *rtcp_data, int rtcp_len, const char *prefix) {
    if (rtcp_len < 32) {
        printf("    %s[WARN] RTCP packet too short for full RR (%d bytes)\n", prefix, rtcp_len);
        return;
    }
    
    struct rtcp_rr_packet *rr = (struct rtcp_rr_packet *)rtcp_data;
    
    uint8_t version = (rr->version_p_count >> 6) & 0x03;
    uint8_t padding = (rr->version_p_count >> 5) & 0x01;
    uint8_t rc = rr->version_p_count & 0x1F;
    uint16_t length = ntohs(rr->length);
    
    // Convert network byte order to host byte order for decimal display
    uint32_t sender_ssrc = ntohl(rr->ssrc);
    uint32_t source_ssrc = ntohl(rr->ssrc_1);
    uint32_t extended_seq = ntohl(rr->extended_high_seq);
    uint32_t jitter = ntohl(rr->jitter);
    uint32_t lsr = ntohl(rr->lsr);
    uint32_t dlsr = ntohl(rr->dlsr);
    
    printf("    %sRTCP RR Details:\n", prefix);
    printf("    %s  Version: %u, Padding: %u, Report Count: %u\n", prefix, version, padding, rc);
    printf("    %s  Packet Type: %u (Receiver Report)\n", prefix, rr->packet_type);
    printf("    %s  Length: %u (in 32-bit words - 1)\n", prefix, length);
    printf("    %s  Sender SSRC: %u (0x%08X)\n", prefix, sender_ssrc, sender_ssrc);
    printf("    %s  Source SSRC: %u (0x%08X)\n", prefix, source_ssrc, source_ssrc);
    
    // Parse fraction lost and cumulative lost
    uint32_t fraction_lost = ntohl(rr->fraction_lost);
    uint8_t fraction = (fraction_lost >> 24) & 0xFF;
    uint32_t cumulative_lost = fraction_lost & 0xFFFFFF;
    // Convert from 24-bit signed to 32-bit signed
    if (cumulative_lost & 0x800000) {
        cumulative_lost |= 0xFF000000;
    }
    
    printf("    %s  Fraction Lost: %u/256 (%u%%)\n", prefix, fraction, (fraction * 100) / 256);
    printf("    %s  Cumulative Packets Lost: %d\n", prefix, (int32_t)cumulative_lost);
    printf("    %s  Extended Highest Seq: %u\n", prefix, extended_seq);
    printf("    %s  Jitter: %u\n", prefix, jitter);
    printf("    %s  Last SR Timestamp: %u (0x%08X)\n", prefix, lsr, lsr);
    printf("    %s  Delay Since Last SR: %u units\n", prefix, dlsr);
}

// Test function to verify packet integrity
static void verify_packet_integrity(const unsigned char *packet, int len, const char *label) {
    struct iphdr *iph = (struct iphdr *)packet;
    int ip_header_len = iph->ihl * 4;
    
    printf("    [VERIFY %s]\n", label);
    printf("    IP Version: %d, IHL: %d, Total Length: %d\n", 
           iph->version, iph->ihl, ntohs(iph->tot_len));
    printf("    Protocol: %d, Checksum: 0x%04X\n", iph->protocol, ntohs(iph->check));
    printf("    Source: %s\n", inet_ntoa(*(struct in_addr*)&iph->saddr));
    printf("    Dest: %s\n", inet_ntoa(*(struct in_addr*)&iph->daddr));
    
    // Verify IP checksum
    uint16_t calculated = calculate_ip_checksum_debug((struct iphdr *)packet, "[VERIFY]");
    printf("    IP Checksum %s: calculated=0x%04X, packet=0x%04X\n",
           (calculated == ntohs(iph->check)) ? "VALID" : "INVALID",
           calculated, ntohs(iph->check));
    
    if (iph->protocol == IPPROTO_UDP) {
        struct udphdr *udph = (struct udphdr *)(packet + ip_header_len);
        printf("    UDP Source Port: %d, Dest Port: %d\n", 
               ntohs(udph->uh_sport), ntohs(udph->uh_dport));
        printf("    UDP Length: %d, Checksum: 0x%04X\n", 
               ntohs(udph->uh_ulen), ntohs(udph->uh_sum));
        
        // Print some RTCP info
        if (len >= ip_header_len + 8 + 8) {
            const unsigned char *rtcp_data = packet + ip_header_len + 8;
            unsigned char version = (rtcp_data[0] >> 6) & 0x03;
            unsigned char packet_type = rtcp_data[1];
            printf("    RTCP Version: %d, Type: %d\n", version, packet_type);
        }
    }
    printf("\n");
}

// Create a fake RTCP Receiver Report with UDP checksum calculation
static int create_fake_rr(const unsigned char *original_packet, int packet_len,
                         unsigned char **fake_packet, int *fake_len,
                         uint32_t fixed_jitter, uint32_t fixed_fraction_lost) {
    
    struct iphdr *iph = (struct iphdr *)original_packet;
    int ip_header_len = iph->ihl * 4;
    struct udphdr *udph = (struct udphdr *)(original_packet + ip_header_len);
    
    printf("    [CHECKSUM DEBUG] === START ===\n");
    printf("    [CHECKSUM] Original IP checksum: 0x%04X\n", ntohs(iph->check));
    printf("    [CHECKSUM] Original UDP checksum: 0x%04X\n", ntohs(udph->uh_sum));
    
    // Verify original checksum calculation
    uint16_t verify_orig = calculate_ip_checksum_debug((struct iphdr *)original_packet, "[ORIGINAL]");
    printf("    [CHECKSUM] Verified original checksum: 0x%04X (%s)\n", 
           verify_orig, (verify_orig == ntohs(iph->check)) ? "MATCH" : "MISMATCH!");
    
    // Allocate memory for fake packet (same size as original)
    *fake_len = packet_len;
    *fake_packet = malloc(*fake_len);
    if (!*fake_packet) {
        return -1;
    }
    
    // Copy original packet EXACTLY
    memcpy(*fake_packet, original_packet, packet_len);
    
    // Get pointers to the headers in the fake packet
    struct iphdr *fake_iph = (struct iphdr *)*fake_packet;
    struct udphdr *fake_udph = (struct udphdr *)(*fake_packet + ip_header_len);
    struct rtcp_rr_packet *fake_rr = (struct rtcp_rr_packet *)(*fake_packet + ip_header_len + 8);
    
    printf("    [MODIFY] Before modification:\n");
    printf("    [MODIFY]   Jitter: %u (0x%08X)\n", ntohl(fake_rr->jitter), ntohl(fake_rr->jitter));
    printf("    [MODIFY]   Fraction Lost: 0x%08X\n", ntohl(fake_rr->fraction_lost));
    
    // ONLY modify jitter and fraction lost - preserve everything else!
    fake_rr->jitter = htonl(fixed_jitter);
    
    // Set fixed fraction lost but preserve cumulative lost from original
    uint32_t original_fraction_lost = ntohl(fake_rr->fraction_lost);
    uint32_t cumulative_lost = original_fraction_lost & 0xFFFFFF;
    uint32_t new_fraction_lost = (fixed_fraction_lost << 24) | cumulative_lost;
    fake_rr->fraction_lost = htonl(new_fraction_lost);
    
    printf("    [MODIFY] After modification:\n");
    printf("    [MODIFY]   Jitter: %u (0x%08X)\n", ntohl(fake_rr->jitter), ntohl(fake_rr->jitter));
    printf("    [MODIFY]   Fraction Lost: 0x%08X\n", ntohl(fake_rr->fraction_lost));
    
    // Recalculate IP checksum
    fake_iph->check = 0;
    uint16_t new_ip_checksum = calculate_ip_checksum_debug(fake_iph, "[NEW IP]");
    fake_iph->check = htons(new_ip_checksum);
    
    printf("    [CHECKSUM] New IP checksum: 0x%04X\n", new_ip_checksum);
    
    // Recalculate UDP checksum properly
    int udp_payload_len = ntohs(fake_udph->uh_ulen) - 8;
    const unsigned char *udp_payload = (unsigned char *)fake_rr;
    
    fake_udph->uh_sum = 0; // Must be zero for calculation
    uint16_t new_udp_checksum = calculate_udp_checksum(fake_iph, fake_udph, udp_payload, udp_payload_len);
    fake_udph->uh_sum = htons(new_udp_checksum);
    
    printf("    [CHECKSUM] New UDP checksum: 0x%04X\n", new_udp_checksum);
    printf("    [CHECKSUM DEBUG] === END ===\n\n");
    return 0;
}

// Check if packet is an RTCP Receiver Report
static int is_rtcp_receiver_report(const unsigned char *packet_data, int packet_len,
                                   struct in_addr *src_ip, struct in_addr *dst_ip,
                                   uint16_t *src_port, uint16_t *dst_port,
                                   uint32_t *ssrc, uint32_t *extended_seq)
{
    struct iphdr *iph;
    struct udphdr *udph;
    const unsigned char *rtcp_data;
    int ip_header_len;
    int udp_payload_len;
    
    // Minimum size check: IP header (20) + UDP header (8) + RTCP header (8)
    if (packet_len < 20 + 8 + 8) {
        return 0;
    }
    
    // Parse IP header
    iph = (struct iphdr *)packet_data;
    
    // Verify IPv4
    if (iph->version != 4) {
        return 0;
    }
    
    // Verify UDP protocol
    if (iph->protocol != IPPROTO_UDP) {
        return 0;
    }
    
    ip_header_len = iph->ihl * 4;
    
    // Check if we have enough data for UDP header
    if (packet_len < ip_header_len + 8) {
        return 0;
    }
    
    // Parse UDP header
    udph = (struct udphdr *)(packet_data + ip_header_len);
    
    // Get UDP payload length
    udp_payload_len = ntohs(udph->uh_ulen) - 8;
    
    // Check if we have enough data for RTCP header (at least 8 bytes)
    if (udp_payload_len < 8) {
        return 0;
    }
    
    // Check buffer bounds
    if (packet_len < ip_header_len + 8 + 8) {
        return 0;
    }
    
    // Get RTCP data (after UDP header)
    rtcp_data = packet_data + ip_header_len + 8;
    
    // Parse RTCP header
    unsigned char version = (rtcp_data[0] >> 6) & 0x03;
    unsigned char packet_type = rtcp_data[1];
    
    if (version == 2 && packet_type == RTCP_RR) {
        // Extract IP addresses and ports
        src_ip->s_addr = iph->saddr;
        dst_ip->s_addr = iph->daddr;
        *src_port = ntohs(udph->uh_sport);
        *dst_port = ntohs(udph->uh_dport);
        
        // Extract SSRC and extended sequence number if requested
        if (ssrc && extended_seq && udp_payload_len >= 32) {
            struct rtcp_rr_packet *rr = (struct rtcp_rr_packet *)rtcp_data;
            *ssrc = ntohl(rr->ssrc);  // Convert to host byte order
            *extended_seq = ntohl(rr->extended_high_seq);  // Convert to host byte order
        }
        
        return 1;
    }
    
    return 0;
}

// Inject a fake RR packet
static int inject_fake_rr(const unsigned char *original_packet, int packet_len,
                         uint32_t fixed_jitter, uint32_t fixed_fraction_lost,
                         const char *debug_prefix) {
    
    unsigned char *fake_packet;
    int fake_len;
    
    printf("    %s=== PACKET INTEGRITY CHECK ===\n", debug_prefix);
    verify_packet_integrity(original_packet, packet_len, "ORIGINAL");
    
    if (create_fake_rr(original_packet, packet_len, &fake_packet, &fake_len,
                      fixed_jitter, fixed_fraction_lost) == 0) {
        
        verify_packet_integrity(fake_packet, fake_len, "FAKE");
        
        printf("    %sINJECTING FAKE RR (Jitter: %u, Fraction Lost: %u/256)\n",
               debug_prefix, fixed_jitter, fixed_fraction_lost);
        
        // Print fake RR details
        int ip_header_len = ((struct iphdr *)fake_packet)->ihl * 4;
        const unsigned char *fake_rtcp = fake_packet + ip_header_len + 8;
        int fake_rtcp_len = fake_len - ip_header_len - 8;
        print_rtcp_rr_details(fake_rtcp, fake_rtcp_len, "    [FAKE] ");
        
        // Send via raw socket
        int result = send_raw_packet(fake_packet, fake_len);
        free(fake_packet);
        
        if (result > 0) {
            printf("    %s✅ FAKE RR INJECTION SUCCESSFUL (%d bytes sent)\n", debug_prefix, result);
            return 1;
        } else {
            printf("    %s❌ FAKE RR INJECTION FAILED\n", debug_prefix);
            return 0;
        }
    } else {
        printf("    %s❌ FAILED TO CREATE FAKE RR\n", debug_prefix);
        return 0;
    }
}

// Statistics structure
struct stats {
    unsigned long total_packets;
    unsigned long udp_packets;
    unsigned long rtcp_rr_packets;
    unsigned long rtcp_rr_dropped;
    unsigned long rtcp_rr_faked;
    unsigned long non_udp_packets;
    unsigned long parse_errors;
};

static struct stats packet_stats = {0};

// Global configuration
static struct {
    uint32_t fixed_jitter;
    uint32_t fixed_fraction_lost;
    operation_mode_t mode;
} config = {
    .fixed_jitter = 100,        // Default fixed jitter value
    .fixed_fraction_lost = 10,  // Default fixed fraction lost (10/256 ≈ 3.9%)
    .mode = MODE_REPLACE        // Default mode: replace real RR with fake
};

static int callback(struct nfq_q_handle *qh, struct nfgenmsg *nfmsg,
                   struct nfq_data *nfa, void *data)
{
    int id = 0;
    struct nfqnl_msg_packet_hdr *ph;
    unsigned char *packet_data;
    int packet_len;
    struct in_addr src_ip, dst_ip;
    uint16_t src_port, dst_port;
    uint32_t ssrc, extended_seq;
    
    packet_stats.total_packets++;
    
    ph = nfq_get_msg_packet_hdr(nfa);
    if (ph) {
        id = ntohl(ph->packet_id);
    }
    
    packet_len = nfq_get_payload(nfa, &packet_data);
    
    if (packet_len >= 0) {
        // Quick check if it's UDP
        if (packet_len >= 20) {
            struct iphdr *iph = (struct iphdr *)packet_data;
            if (iph->version == 4 && iph->protocol == IPPROTO_UDP) {
                packet_stats.udp_packets++;
            } else {
                packet_stats.non_udp_packets++;
            }
        }
        
        // Check if it's an RTCP Receiver Report
        if (is_rtcp_receiver_report(packet_data, packet_len, 
                                    &src_ip, &dst_ip, &src_port, &dst_port,
                                    &ssrc, &extended_seq)) {
            packet_stats.rtcp_rr_packets++;
            
            printf("\n>>> RTCP RECEIVER REPORT DETECTED! <<<\n");
            printf("    Source: %s:%u\n", inet_ntoa(src_ip), src_port);
            printf("    Destination: %s:%u\n", inet_ntoa(dst_ip), dst_port);
            printf("    Packet ID: %d, Length: %d bytes\n", id, packet_len);
            printf("    SSRC: %u (0x%08X)\n", ssrc, ssrc);
            printf("    Extended Seq: %u\n", extended_seq);
            
            // Print real RR details
            int ip_header_len = ((struct iphdr *)packet_data)->ihl * 4;
            const unsigned char *rtcp_data = packet_data + ip_header_len + 8;
            int rtcp_len = packet_len - ip_header_len - 8;
            print_rtcp_rr_details(rtcp_data, rtcp_len, "[REAL] ");
            
            // Handle based on operation mode
            switch (config.mode) {
                case MODE_ACCEPT_ALL:
                    printf("    [MODE: ACCEPT_ALL] Accepting real RR packet\n");
                    return nfq_set_verdict(qh, id, NF_ACCEPT, 0, NULL);
                    
                case MODE_REPLACE:
                    printf("    [MODE: REPLACE] Replacing real RR with fake\n");
                    if (inject_fake_rr(packet_data, packet_len, 
                                     config.fixed_jitter, config.fixed_fraction_lost,
                                     "[REPLACE] ")) {
                        packet_stats.rtcp_rr_faked++;
                        packet_stats.rtcp_rr_dropped++;
                        printf("    [REPLACE] Dropping real RR packet\n");
                        return nfq_set_verdict(qh, id, NF_DROP, 0, NULL);
                    } else {
                        // If injection failed, fall back to accepting real packet
                        printf("    [REPLACE] Injection failed, accepting real RR\n");
                        return nfq_set_verdict(qh, id, NF_ACCEPT, 0, NULL);
                    }
                    
                case MODE_BOTH:
                    printf("    [MODE: BOTH] Accepting real RR AND injecting fake\n");
                    if (inject_fake_rr(packet_data, packet_len,
                                     config.fixed_jitter, config.fixed_fraction_lost,
                                     "[BOTH] ")) {
                        packet_stats.rtcp_rr_faked++;
                    }
                    printf("    [BOTH] Also accepting real RR packet\n");
                    return nfq_set_verdict(qh, id, NF_ACCEPT, 0, NULL);
                    
                default:
                    printf("    [ERROR] Unknown operation mode, accepting packet\n");
                    return nfq_set_verdict(qh, id, NF_ACCEPT, 0, NULL);
            }
        } else {
            // Accept non-RR packets
            return nfq_set_verdict(qh, id, NF_ACCEPT, 0, NULL);
        }
        
        // Print stats every 100 packets
        if (packet_stats.total_packets % 100 == 0) {
            printf("[Stats] Total: %lu | UDP: %lu | RTCP-RR: %lu | Dropped: %lu | Faked: %lu\n",
                   packet_stats.total_packets,
                   packet_stats.udp_packets,
                   packet_stats.rtcp_rr_packets,
                   packet_stats.rtcp_rr_dropped,
                   packet_stats.rtcp_rr_faked);
            fflush(stdout);
        }
        
    } else {
        packet_stats.parse_errors++;
        fprintf(stderr, "Warning: Failed to get packet payload (id=%d)\n", id);
        return nfq_set_verdict(qh, id, NF_ACCEPT, 0, NULL);
    }
    
    return 0;
}

void print_usage(const char *program_name) {
    printf("Usage: %s [options] [queue_num]\n", program_name);
    printf("Options:\n");
    printf("  -j jitter      Set fixed jitter value (default: 100)\n");
    printf("  -l fraction    Set fixed fraction lost (0-255, default: 10)\n");
    printf("  -m mode        Operation mode: 0=ACCEPT_ALL, 1=REPLACE, 2=BOTH (default: 1)\n");
    printf("  -h             Show this help\n");
    printf("\nOperation Modes:\n");
    printf("  0 (ACCEPT_ALL): Accept all real RR packets, no injection\n");
    printf("  1 (REPLACE):    Drop real RR, inject fake instead (recommended)\n");
    printf("  2 (BOTH):       Accept real RR AND inject fake (debugging)\n");
    printf("\nExamples:\n");
    printf("  %s 0                    # Use queue 0 with default values (REPLACE mode)\n", program_name);
    printf("  %s -j 50 -l 5 -m 1 0   # Jitter=50, Loss=5/256, REPLACE mode, queue 0\n", program_name);
    printf("  %s -m 0 0              # ACCEPT_ALL mode - no modification\n", program_name);
    printf("  %s -m 2 0              # BOTH mode - debug both real and fake\n", program_name);
}

int main(int argc, char **argv)
{
    struct nfq_handle *h;
    struct nfq_q_handle *qh;
    int fd;
    int rv;
    char buf[4096];
    int queue_num = 0;
    int opt;

    // Parse command line arguments
    while ((opt = getopt(argc, argv, "j:l:m:h")) != -1) {
        switch (opt) {
            case 'j':
                config.fixed_jitter = atoi(optarg);
                break;
            case 'l':
                config.fixed_fraction_lost = atoi(optarg);
                if (config.fixed_fraction_lost > 255) {
                    fprintf(stderr, "Error: Fraction lost must be 0-255\n");
                    return 1;
                }
                break;
            case 'm':
                config.mode = atoi(optarg);
                if (config.mode < 0 || config.mode > 2) {
                    fprintf(stderr, "Error: Mode must be 0, 1, or 2\n");
                    return 1;
                }
                break;
            case 'h':
                print_usage(argv[0]);
                return 0;
            default:
                print_usage(argv[0]);
                return 1;
        }
    }

    // Get queue number from remaining argument
    if (optind < argc) {
        queue_num = atoi(argv[optind]);
        if (queue_num < 0 || queue_num > 65535) {
            fprintf(stderr, "Error: Invalid queue number. Must be 0-65535\n");
            return 1;
        }
    }

    const char *mode_str;
    switch (config.mode) {
        case MODE_ACCEPT_ALL: mode_str = "ACCEPT_ALL"; break;
        case MODE_REPLACE: mode_str = "REPLACE"; break;
        case MODE_BOTH: mode_str = "BOTH"; break;
        default: mode_str = "UNKNOWN"; break;
    }

    printf("RTCP Receiver Report Manipulator\n");
    printf("=================================\n");
    printf("Configuration:\n");
    printf("  Queue Number: %d\n", queue_num);
    printf("  Fixed Jitter: %u\n", config.fixed_jitter);
    printf("  Fixed Fraction Lost: %u/256 (%u%%)\n", 
           config.fixed_fraction_lost, (config.fixed_fraction_lost * 100) / 256);
    printf("  Operation Mode: %d (%s)\n", config.mode, mode_str);
    printf("\nPress Ctrl+C to stop\n\n");
    fflush(stdout);

    // Set up signal handler for graceful shutdown
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    h = nfq_open();
    if (!h) {
        fprintf(stderr, "Error: nfq_open() failed. Are you running as root?\n");
        return 1;
    }

    // Unbind any existing handler for AF_INET (ignore errors)
    nfq_unbind_pf(h, AF_INET);

    if (nfq_bind_pf(h, AF_INET) < 0) {
        fprintf(stderr, "Error: nfq_bind_pf() failed\n");
        nfq_close(h);
        return 1;
    }

    qh = nfq_create_queue(h, queue_num, &callback, NULL);
    if (!qh) {
        fprintf(stderr, "Error: nfq_create_queue() failed. Is queue %d already in use?\n", queue_num);
        nfq_close(h);
        return 1;
    }

    // Request full packet data (up to 65535 bytes)
    if (nfq_set_mode(qh, NFQNL_COPY_PACKET, 0xFFFF) < 0) {
        fprintf(stderr, "Error: nfq_set_mode() failed\n");
        nfq_destroy_queue(qh);
        nfq_close(h);
        return 1;
    }

    fd = nfq_fd(h);

    printf("Successfully initialized. Waiting for packets...\n");
    printf("(Stats will appear every 100 packets or when RTCP RR is detected)\n\n");
    fflush(stdout);

    while (keep_running && (rv = recv(fd, buf, sizeof(buf), 0)) && rv >= 0) {
        nfq_handle_packet(h, buf, rv);
    }

    printf("\n\n=== Final Statistics ===\n");
    printf("Total packets processed: %lu\n", packet_stats.total_packets);
    printf("UDP packets: %lu (%.1f%%)\n", 
           packet_stats.udp_packets,
           packet_stats.total_packets > 0 ? 
           (100.0 * packet_stats.udp_packets / packet_stats.total_packets) : 0);
    printf("RTCP Receiver Reports: %lu\n", packet_stats.rtcp_rr_packets);
    printf("RTCP RR Packets Dropped: %lu\n", packet_stats.rtcp_rr_dropped);
    printf("Fake RR Packets Injected: %lu\n", packet_stats.rtcp_rr_faked);
    printf("Non-UDP packets: %lu\n", packet_stats.non_udp_packets);
    printf("Parse errors: %lu\n", packet_stats.parse_errors);
    printf("========================\n\n");
    
    nfq_destroy_queue(qh);
    nfq_close(h);
    printf("RTCP manipulator stopped\n");
    return 0;
}