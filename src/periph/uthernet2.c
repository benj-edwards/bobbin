//  periph/uthernet2.c
//
//  Uthernet II (W5100) emulation for Bobbin Apple II Emulator
//
//  Copyright (c) 2026 Claude Code Project Contributors
//  This code is licensed under the MIT license.
//  See the accompanying LICENSE file for details.

#include "bobbin-internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>

// W5100 Register Addresses (internal memory map)
#define W5100_MR        0x0000  // Mode Register
#define W5100_GAR       0x0001  // Gateway Address (4 bytes)
#define W5100_SUBR      0x0005  // Subnet Mask (4 bytes)
#define W5100_SHAR      0x0009  // Source Hardware Address - MAC (6 bytes)
#define W5100_SIPR      0x000F  // Source IP Address (4 bytes)
#define W5100_IR        0x0015  // Interrupt Register
#define W5100_IMR       0x0016  // Interrupt Mask Register
#define W5100_RTR       0x0017  // Retry Time (2 bytes)
#define W5100_RCR       0x0019  // Retry Count
#define W5100_RMSR      0x001A  // RX Memory Size
#define W5100_TMSR      0x001B  // TX Memory Size
#define W5100_PPTLR     0x0028  // PPP LCP Request Timer (virtual detection)

// Socket Registers base addresses
#define W5100_S0_BASE   0x0400
#define W5100_S1_BASE   0x0500
#define W5100_S2_BASE   0x0600
#define W5100_S3_BASE   0x0700

// Socket Register offsets
#define Sn_MR           0x00    // Socket Mode
#define Sn_CR           0x01    // Socket Command
#define Sn_IR           0x02    // Socket Interrupt
#define Sn_SR           0x03    // Socket Status
#define Sn_PORT         0x04    // Source Port (2 bytes)
#define Sn_DHAR         0x06    // Destination Hardware Address (6 bytes)
#define Sn_DIPR         0x0C    // Destination IP (4 bytes)
#define Sn_DPORT        0x10    // Destination Port (2 bytes)
#define Sn_MSSR         0x12    // Maximum Segment Size (2 bytes)
#define Sn_PROTO        0x14    // IP Protocol (raw mode)
#define Sn_TOS          0x15    // Type of Service
#define Sn_TTL          0x16    // Time to Live
#define Sn_TX_FSR       0x20    // TX Free Size (2 bytes)
#define Sn_TX_RD        0x22    // TX Read Pointer (2 bytes)
#define Sn_TX_WR        0x24    // TX Write Pointer (2 bytes)
#define Sn_RX_RSR       0x26    // RX Received Size (2 bytes)
#define Sn_RX_RD        0x28    // RX Read Pointer (2 bytes)

// Socket Modes (Sn_MR)
#define Sn_MR_CLOSE     0x00
#define Sn_MR_TCP       0x01
#define Sn_MR_UDP       0x02
#define Sn_MR_IPRAW     0x03
#define Sn_MR_MACRAW    0x04

// Socket Commands (Sn_CR)
#define Sn_CR_OPEN      0x01
#define Sn_CR_LISTEN    0x02
#define Sn_CR_CONNECT   0x04
#define Sn_CR_DISCON    0x08
#define Sn_CR_CLOSE     0x10
#define Sn_CR_SEND      0x20
#define Sn_CR_RECV      0x40

// Socket Status (Sn_SR)
#define Sn_SR_CLOSED        0x00
#define Sn_SR_INIT          0x13
#define Sn_SR_LISTEN        0x14
#define Sn_SR_SYNSENT       0x15
#define Sn_SR_SYNRECV       0x16
#define Sn_SR_ESTABLISHED   0x17
#define Sn_SR_FIN_WAIT      0x18
#define Sn_SR_CLOSING       0x1A
#define Sn_SR_TIME_WAIT     0x1B
#define Sn_SR_CLOSE_WAIT    0x1C
#define Sn_SR_LAST_ACK      0x1D
#define Sn_SR_UDP           0x22
#define Sn_SR_IPRAW         0x32
#define Sn_SR_MACRAW        0x42

// TX/RX Buffer addresses (default: 2KB per socket)
#define W5100_TX_BASE   0x4000  // TX buffer base
#define W5100_TX_SIZE   0x2000  // 8KB total TX buffer
#define W5100_RX_BASE   0x6000  // RX buffer base
#define W5100_RX_SIZE   0x2000  // 8KB total RX buffer

// Per-socket buffer sizes (default 2KB each)
#define SOCK_BUF_SIZE   0x0800  // 2KB per socket

// Apple II I/O soft switch offsets
// For slot N: $C0n4 = Mode, $C0n5 = Addr Hi, $C0n6 = Addr Lo, $C0n7 = Data
#define SW_MODE_REG     0x04
#define SW_ADDR_HI      0x05
#define SW_ADDR_LO      0x06
#define SW_DATA_REG     0x07

// Mode register bits
#define MR_RST          0x80    // Reset
#define MR_PB           0x10    // Ping Block
#define MR_PPPoE        0x08    // PPPoE mode
#define MR_AI           0x02    // Address auto-increment
#define MR_IND          0x01    // Indirect bus mode

// Socket state for host-side bridging
typedef struct {
    int fd;                     // Host BSD socket fd (-1 if not open)
    bool connecting;            // Non-blocking connect in progress
    byte rx_buf[4096];          // Local receive buffer
    word rx_head;               // Receive buffer head
    word rx_tail;               // Receive buffer tail
    bool macraw_mode;           // In MACRAW mode (raw Ethernet)
} SocketState;

// Virtual DHCP state
typedef enum {
    DHCP_IDLE,
    DHCP_DISCOVER_SEEN,
    DHCP_OFFER_SENT,
    DHCP_REQUEST_SEEN,
    DHCP_COMPLETE
} DhcpState;

// W5100 emulation state
typedef struct {
    byte memory[0x8000];        // 32KB W5100 internal memory
    word addr_ptr;              // Current address pointer
    byte mode;                  // Access mode register
    SocketState sockets[4];     // Host socket state
    bool initialized;           // Has been reset/initialized
    DhcpState dhcp_state;       // Virtual DHCP state machine
    byte dhcp_xid[4];           // Transaction ID from DHCP discover
    byte client_mac[6];         // Client MAC from discover
} Uthernet2State;

// Virtual network configuration
static const byte VIRTUAL_SERVER_MAC[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
static const byte VIRTUAL_CLIENT_IP[4] = {192, 168, 65, 100};
static const byte VIRTUAL_SERVER_IP[4] = {192, 168, 65, 1};
static const byte VIRTUAL_GATEWAY[4] = {192, 168, 65, 1};
static const byte VIRTUAL_SUBNET[4] = {255, 255, 255, 0};
static const byte VIRTUAL_DNS[4] = {8, 8, 8, 8};

static Uthernet2State u2;
static unsigned int slot_num = 3;  // Default to slot 3

// Forward declarations
static void socket_command(int socknum, byte cmd);
static void socket_poll(int socknum);
static void virtual_tcp_poll(int socknum);
static word get_socket_base(int socknum);
static word get_tx_base(int socknum);
static word get_rx_base(int socknum);
static void handle_macraw_send(int socknum);
static void inject_dhcp_response(int socknum, bool is_ack);

//=============================================================================
// Virtual DHCP Implementation
//=============================================================================

// Ethernet frame offsets
#define ETH_DST         0       // Destination MAC (6 bytes)
#define ETH_SRC         6       // Source MAC (6 bytes)
#define ETH_TYPE        12      // EtherType (2 bytes)
#define ETH_HEADER_LEN  14

// IP header offsets (from start of IP header)
#define IPH_VER_IHL     0
#define IPH_TOS         1
#define IPH_LEN         2       // Total length (2 bytes)
#define IPH_ID          4
#define IPH_FRAG        6
#define IPH_TTL         8
#define IPH_PROTO       9
#define IPH_CHECKSUM    10
#define IPH_SRC         12      // Source IP (4 bytes)
#define IPH_DST         16      // Destination IP (4 bytes)
#define IPH_HEADER_LEN  20

// UDP header offsets (from start of UDP header)
#define UDP_SRC_PORT    0
#define UDP_DST_PORT    2
#define UDP_LEN         4
#define UDP_CHECKSUM    6
#define UDP_HEADER_LEN  8

// DHCP offsets (from start of DHCP payload)
#define DHCP_OP         0       // Message type (1=request, 2=reply)
#define DHCP_HTYPE      1       // Hardware type (1=Ethernet)
#define DHCP_HLEN       2       // Hardware address length (6)
#define DHCP_HOPS       3
#define DHCP_XID        4       // Transaction ID (4 bytes)
#define DHCP_SECS       8
#define DHCP_FLAGS      10
#define DHCP_CIADDR     12      // Client IP (4 bytes)
#define DHCP_YIADDR     16      // Your (client) IP (4 bytes)
#define DHCP_SIADDR     20      // Server IP (4 bytes)
#define DHCP_GIADDR     24      // Gateway IP (4 bytes)
#define DHCP_CHADDR     28      // Client hardware address (16 bytes)
#define DHCP_SNAME      44      // Server name (64 bytes)
#define DHCP_FILE       108     // Boot filename (128 bytes)
#define DHCP_MAGIC      236     // Magic cookie (4 bytes: 99, 130, 83, 99)
#define DHCP_OPTIONS    240     // Options start here

// DHCP message types
#define DHCP_DISCOVER   1
#define DHCP_OFFER      2
#define DHCP_REQUEST    3
#define DHCP_ACK        5

// ARP offsets (from start of ARP payload, after Ethernet header)
#define ARP_HTYPE       0       // Hardware type (2 bytes, 0x0001 = Ethernet)
#define ARP_PTYPE       2       // Protocol type (2 bytes, 0x0800 = IPv4)
#define ARP_HLEN        4       // Hardware address length (1 byte, 6)
#define ARP_PLEN        5       // Protocol address length (1 byte, 4)
#define ARP_OPER        6       // Operation (2 bytes, 1=request, 2=reply)
#define ARP_SHA         8       // Sender hardware address (6 bytes)
#define ARP_SPA         14      // Sender protocol address (4 bytes)
#define ARP_THA         18      // Target hardware address (6 bytes)
#define ARP_TPA         24      // Target protocol address (4 bytes)
#define ARP_FRAME_LEN   28      // Total ARP payload length

// TCP header offsets (from start of TCP header)
#define TCP_SRC_PORT    0       // Source port (2 bytes)
#define TCP_DST_PORT    2       // Destination port (2 bytes)
#define TCP_SEQ         4       // Sequence number (4 bytes)
#define TCP_ACK         8       // Acknowledgment number (4 bytes)
#define TCP_OFFSET      12      // Data offset (4 bits), reserved (6 bits), flags (6 bits)
#define TCP_FLAGS       13      // Flags byte
#define TCP_WINDOW      14      // Window size (2 bytes)
#define TCP_CHECKSUM    16      // Checksum (2 bytes)
#define TCP_URGENT      18      // Urgent pointer (2 bytes)
#define TCP_HEADER_LEN  20      // Minimum header length

// TCP flags
#define TCP_FIN         0x01
#define TCP_SYN         0x02
#define TCP_RST         0x04
#define TCP_PSH         0x08
#define TCP_ACK_FLAG    0x10
#define TCP_URG         0x20

// Virtual gateway MAC address (for ARP responses)
static const byte VIRTUAL_GATEWAY_MAC[6] = {0x02, 0x00, 0xDE, 0xAD, 0xBE, 0x01};

// Virtual TCP connection state (single connection for simplicity)
static struct {
    int fd;                     // Host socket file descriptor (-1 = closed)
    byte remote_mac[6];         // Client MAC address
    byte remote_ip[4];          // Client IP address
    byte local_ip[4];           // IP address client connected to (for response)
    word remote_port;           // Client port
    word local_port;            // Server port (target port on gateway)
    uint32_t our_seq;           // Our sequence number
    uint32_t their_seq;         // Their sequence number (next expected)
    bool established;           // Connection is established
    bool fin_sent;              // We sent FIN
    bool fin_received;          // We received FIN
} virtual_tcp = { .fd = -1 };

// Forward declarations for TCP
static void handle_arp_packet(int socknum, byte *frame, int len);
static void handle_tcp_packet(int socknum, byte *frame, int len);
static void inject_tcp_response(int socknum, byte flags, byte *data, int data_len);
static void inject_arp_reply(int socknum, byte *request_frame);

// Calculate IP checksum
static word ip_checksum(byte *data, int len)
{
    unsigned long sum = 0;
    for (int i = 0; i < len; i += 2) {
        word w = (data[i] << 8);
        if (i + 1 < len) w |= data[i + 1];
        sum += w;
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return ~sum & 0xFFFF;
}

// Check if this is a DHCP packet and what type
static int detect_dhcp_type(byte *frame, int len)
{
    // Minimum: Ethernet(14) + IP(20) + UDP(8) + DHCP(240+4) = 286 bytes
    if (len < 286) return -1;

    // Check EtherType = IPv4 (0x0800)
    if (frame[ETH_TYPE] != 0x08 || frame[ETH_TYPE + 1] != 0x00) return -1;

    byte *ip = frame + ETH_HEADER_LEN;

    // Check IP protocol = UDP (17)
    if (ip[IPH_PROTO] != 17) return -1;

    byte *udp = ip + IPH_HEADER_LEN;

    // Check UDP ports: src=68 (client), dst=67 (server) for DHCP requests
    word src_port = (udp[UDP_SRC_PORT] << 8) | udp[UDP_SRC_PORT + 1];
    word dst_port = (udp[UDP_DST_PORT] << 8) | udp[UDP_DST_PORT + 1];
    if (src_port != 68 || dst_port != 67) return -1;

    byte *dhcp = udp + UDP_HEADER_LEN;

    // Check DHCP magic cookie
    if (dhcp[DHCP_MAGIC] != 99 || dhcp[DHCP_MAGIC + 1] != 130 ||
        dhcp[DHCP_MAGIC + 2] != 83 || dhcp[DHCP_MAGIC + 3] != 99) return -1;

    // Find DHCP message type option (option 53)
    byte *opt = dhcp + DHCP_OPTIONS;
    byte *end = frame + len;
    while (opt < end && *opt != 255) {
        if (*opt == 0) { opt++; continue; }  // Padding
        if (*opt == 53 && opt + 2 < end) {   // Message type
            return opt[2];  // Return message type
        }
        opt += 2 + opt[1];  // Skip option
    }

    return -1;
}

// Build and inject a DHCP response (offer or ack)
static void inject_dhcp_response(int socknum, bool is_ack)
{
    SocketState *ss = &u2.sockets[socknum];
    word base = get_socket_base(socknum);

    // Build response packet in rx_buf
    byte *pkt = ss->rx_buf;
    int pos = 0;

    // W5100 MACRAW prepends 2 bytes for packet length
    pkt[pos++] = 0;  // Length high (will fill later)
    pkt[pos++] = 0;  // Length low

    int frame_start = pos;

    // Ethernet header
    // For DHCP, always use broadcast MAC (FF:FF:FF:FF:FF:FF)
    // since clients may not have set their IP yet
    pkt[pos++] = 0xFF; pkt[pos++] = 0xFF; pkt[pos++] = 0xFF;
    pkt[pos++] = 0xFF; pkt[pos++] = 0xFF; pkt[pos++] = 0xFF;
    memcpy(pkt + pos, VIRTUAL_SERVER_MAC, 6);  // Source = server MAC
    pos += 6;
    pkt[pos++] = 0x08;  // EtherType = IPv4
    pkt[pos++] = 0x00;

    int ip_start = pos;

    // IP header
    pkt[pos++] = 0x45;  // Version 4, IHL 5
    pkt[pos++] = 0x00;  // TOS
    pkt[pos++] = 0x00;  // Total length (fill later)
    pkt[pos++] = 0x00;
    pkt[pos++] = 0x00;  // ID
    pkt[pos++] = 0x00;
    pkt[pos++] = 0x00;  // Flags, Fragment
    pkt[pos++] = 0x00;
    pkt[pos++] = 64;    // TTL
    pkt[pos++] = 17;    // Protocol = UDP
    pkt[pos++] = 0x00;  // Checksum (fill later)
    pkt[pos++] = 0x00;
    memcpy(pkt + pos, VIRTUAL_SERVER_IP, 4);  // Source IP
    pos += 4;
    // Destination IP = broadcast for OFFER, client IP for ACK
    if (is_ack) {
        memcpy(pkt + pos, VIRTUAL_CLIENT_IP, 4);
        pos += 4;
    } else {
        pkt[pos++] = 255; pkt[pos++] = 255; pkt[pos++] = 255; pkt[pos++] = 255;
    }

    int udp_start = pos;

    // UDP header
    pkt[pos++] = 0x00;  // Source port 67
    pkt[pos++] = 67;
    pkt[pos++] = 0x00;  // Dest port 68
    pkt[pos++] = 68;
    pkt[pos++] = 0x00;  // UDP length (fill later)
    pkt[pos++] = 0x00;
    pkt[pos++] = 0x00;  // Checksum (optional, set to 0)
    pkt[pos++] = 0x00;

    int dhcp_start = pos;

    // DHCP payload
    pkt[pos++] = 2;     // Op: BOOTREPLY
    pkt[pos++] = 1;     // HType: Ethernet
    pkt[pos++] = 6;     // HLen
    pkt[pos++] = 0;     // Hops
    memcpy(pkt + pos, u2.dhcp_xid, 4);  // XID
    pos += 4;
    pkt[pos++] = 0; pkt[pos++] = 0;  // Secs
    pkt[pos++] = 0; pkt[pos++] = 0;  // Flags
    pkt[pos++] = 0; pkt[pos++] = 0; pkt[pos++] = 0; pkt[pos++] = 0;  // CIAddr
    memcpy(pkt + pos, VIRTUAL_CLIENT_IP, 4);  // YIAddr (your IP)
    pos += 4;
    memcpy(pkt + pos, VIRTUAL_SERVER_IP, 4);  // SIAddr
    pos += 4;
    pkt[pos++] = 0; pkt[pos++] = 0; pkt[pos++] = 0; pkt[pos++] = 0;  // GIAddr
    memcpy(pkt + pos, u2.client_mac, 6);  // CHAddr
    pos += 6;
    memset(pkt + pos, 0, 10);  // CHAddr padding
    pos += 10;
    memset(pkt + pos, 0, 64);  // SName
    pos += 64;
    memset(pkt + pos, 0, 128); // File
    pos += 128;

    // DHCP Magic cookie
    pkt[pos++] = 99; pkt[pos++] = 130; pkt[pos++] = 83; pkt[pos++] = 99;

    // DHCP Options
    // Option 53: DHCP Message Type
    pkt[pos++] = 53; pkt[pos++] = 1;
    pkt[pos++] = is_ack ? DHCP_ACK : DHCP_OFFER;

    // Option 54: Server Identifier
    pkt[pos++] = 54; pkt[pos++] = 4;
    memcpy(pkt + pos, VIRTUAL_SERVER_IP, 4);
    pos += 4;

    // Option 51: Lease Time (1 day)
    pkt[pos++] = 51; pkt[pos++] = 4;
    pkt[pos++] = 0x00; pkt[pos++] = 0x01; pkt[pos++] = 0x51; pkt[pos++] = 0x80;

    // Option 1: Subnet Mask
    pkt[pos++] = 1; pkt[pos++] = 4;
    memcpy(pkt + pos, VIRTUAL_SUBNET, 4);
    pos += 4;

    // Option 3: Router (Gateway)
    pkt[pos++] = 3; pkt[pos++] = 4;
    memcpy(pkt + pos, VIRTUAL_GATEWAY, 4);
    pos += 4;

    // Option 6: DNS
    pkt[pos++] = 6; pkt[pos++] = 4;
    memcpy(pkt + pos, VIRTUAL_DNS, 4);
    pos += 4;

    // End option
    pkt[pos++] = 255;

    // Pad to minimum DHCP size
    while (pos - dhcp_start < 300) pkt[pos++] = 0;

    int frame_len = pos - frame_start;

    // Fill in lengths
    int udp_len = pos - udp_start;
    pkt[udp_start + 4] = udp_len >> 8;
    pkt[udp_start + 5] = udp_len & 0xFF;

    int ip_len = pos - ip_start;
    pkt[ip_start + 2] = ip_len >> 8;
    pkt[ip_start + 3] = ip_len & 0xFF;

    // Calculate IP checksum
    word cksum = ip_checksum(pkt + ip_start, IPH_HEADER_LEN);
    pkt[ip_start + 10] = cksum >> 8;
    pkt[ip_start + 11] = cksum & 0xFF;

    // W5100 length prefix: total size INCLUDING the 2-byte header
    // IP65 reads this value and subtracts 2 to get the frame length
    pkt[0] = (pos >> 8) & 0xFF;
    pkt[1] = pos & 0xFF;

    // Update RX buffer state
    ss->rx_head = 0;
    ss->rx_tail = pos;

    DEBUG("Uthernet II: Injected DHCP %s (%d bytes)\n",
          is_ack ? "ACK" : "OFFER", pos);
}

//=============================================================================
// Virtual ARP Implementation
//=============================================================================

static void handle_arp_packet(int socknum, byte *frame, int len)
{
    // Minimum ARP packet: Ethernet(14) + ARP(28) = 42 bytes
    if (len < 42) return;

    byte *arp = frame + ETH_HEADER_LEN;

    // Check ARP request (operation = 1)
    word oper = (arp[ARP_OPER] << 8) | arp[ARP_OPER + 1];
    if (oper != 1) return;  // Only handle requests

    // Check target IP is our gateway (192.168.65.1)
    if (arp[ARP_TPA] != 192 || arp[ARP_TPA + 1] != 168 ||
        arp[ARP_TPA + 2] != 65 || arp[ARP_TPA + 3] != 1) {
        DEBUG("Uthernet II: ARP for %d.%d.%d.%d (not gateway)\n",
              arp[ARP_TPA], arp[ARP_TPA + 1], arp[ARP_TPA + 2], arp[ARP_TPA + 3]);
        return;
    }

    DEBUG("Uthernet II: ARP request for gateway -> sending reply\n");
    inject_arp_reply(socknum, frame);
}

static void inject_arp_reply(int socknum, byte *request_frame)
{
    SocketState *ss = &u2.sockets[socknum];
    // Write to ss->rx_buf (same as DHCP does)
    byte *pkt = ss->rx_buf;

    byte *req_arp = request_frame + ETH_HEADER_LEN;
    int pos = 2;  // Skip W5100 length prefix

    // Ethernet header
    // Destination MAC = sender of request
    memcpy(&pkt[pos + ETH_DST], &req_arp[ARP_SHA], 6);
    // Source MAC = virtual gateway MAC
    memcpy(&pkt[pos + ETH_SRC], VIRTUAL_GATEWAY_MAC, 6);
    // EtherType = ARP (0x0806)
    pkt[pos + ETH_TYPE] = 0x08;
    pkt[pos + ETH_TYPE + 1] = 0x06;
    pos += ETH_HEADER_LEN;

    // ARP reply
    int arp_start = pos;
    pkt[pos++] = 0x00; pkt[pos++] = 0x01;  // Hardware type: Ethernet
    pkt[pos++] = 0x08; pkt[pos++] = 0x00;  // Protocol type: IPv4
    pkt[pos++] = 6;                         // Hardware size
    pkt[pos++] = 4;                         // Protocol size
    pkt[pos++] = 0x00; pkt[pos++] = 0x02;  // Operation: reply

    // Sender hardware address (gateway MAC)
    memcpy(&pkt[pos], VIRTUAL_GATEWAY_MAC, 6);
    pos += 6;

    // Sender protocol address (gateway IP: 192.168.65.1)
    pkt[pos++] = 192; pkt[pos++] = 168; pkt[pos++] = 65; pkt[pos++] = 1;

    // Target hardware address (from request sender)
    memcpy(&pkt[pos], &req_arp[ARP_SHA], 6);
    pos += 6;

    // Target protocol address (from request sender)
    memcpy(&pkt[pos], &req_arp[ARP_SPA], 4);
    pos += 4;

    // W5100 length prefix
    pkt[0] = (pos >> 8) & 0xFF;
    pkt[1] = pos & 0xFF;

    // Update RX buffer state
    ss->rx_head = 0;
    ss->rx_tail = pos;

    DEBUG("Uthernet II: Injected ARP reply (%d bytes)\n", pos);
}

//=============================================================================
// Virtual TCP Implementation
//=============================================================================

// TCP pseudo-header checksum helper
static word tcp_checksum(byte *ip, byte *tcp, int tcp_len)
{
    unsigned long sum = 0;

    // Pseudo-header: source IP, dest IP, zero, protocol (6), TCP length
    sum += ((ip[IPH_SRC] << 8) | ip[IPH_SRC + 1]);
    sum += ((ip[IPH_SRC + 2] << 8) | ip[IPH_SRC + 3]);
    sum += ((ip[IPH_DST] << 8) | ip[IPH_DST + 1]);
    sum += ((ip[IPH_DST + 2] << 8) | ip[IPH_DST + 3]);
    sum += 6;  // Protocol = TCP
    sum += tcp_len;

    // TCP header + data
    for (int i = 0; i < tcp_len; i += 2) {
        word w = (tcp[i] << 8);
        if (i + 1 < tcp_len) w |= tcp[i + 1];
        sum += w;
    }

    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return ~sum & 0xFFFF;
}

static void handle_tcp_packet(int socknum, byte *frame, int len)
{
    if (len < ETH_HEADER_LEN + IPH_HEADER_LEN + TCP_HEADER_LEN) return;

    byte *ip = frame + ETH_HEADER_LEN;
    byte *tcp = ip + IPH_HEADER_LEN;

    word src_port = (tcp[TCP_SRC_PORT] << 8) | tcp[TCP_SRC_PORT + 1];
    word dst_port = (tcp[TCP_DST_PORT] << 8) | tcp[TCP_DST_PORT + 1];
    byte flags = tcp[TCP_FLAGS];
    int tcp_header_len = ((tcp[TCP_OFFSET] >> 4) & 0x0F) * 4;
    int ip_total_len = (ip[IPH_LEN] << 8) | ip[IPH_LEN + 1];
    int tcp_data_len = ip_total_len - IPH_HEADER_LEN - tcp_header_len;

    uint32_t their_seq = ((uint32_t)tcp[TCP_SEQ] << 24) |
                         ((uint32_t)tcp[TCP_SEQ + 1] << 16) |
                         ((uint32_t)tcp[TCP_SEQ + 2] << 8) |
                         (uint32_t)tcp[TCP_SEQ + 3];

    DEBUG("Uthernet II: TCP %d.%d.%d.%d:%d -> port %d, flags=0x%02X, seq=%u, data=%d\n",
          ip[IPH_SRC], ip[IPH_SRC + 1], ip[IPH_SRC + 2], ip[IPH_SRC + 3],
          src_port, dst_port, flags, their_seq, tcp_data_len);
    // Hex dump incoming packet
    DEBUG("Uthernet II: RX PKT (%d bytes): ", len);
    for (int i = 0; i < len && i < 60; i++) DEBUG("%02X ", frame[i]);
    DEBUG("\n");

    // Handle SYN (connection request)
    if ((flags & TCP_SYN) && !(flags & TCP_ACK_FLAG)) {
        DEBUG("Uthernet II: TCP SYN received, opening connection to localhost:%d\n", dst_port);

        // Close any existing connection
        if (virtual_tcp.fd >= 0) {
            close(virtual_tcp.fd);
        }

        // Open socket to localhost:dst_port
        virtual_tcp.fd = socket(AF_INET, SOCK_STREAM, 0);
        if (virtual_tcp.fd < 0) {
            DEBUG("Uthernet II: TCP socket() failed\n");
            // Send RST
            inject_tcp_response(socknum, TCP_RST | TCP_ACK_FLAG, NULL, 0);
            return;
        }

        // Set non-blocking
        int flags_sock = fcntl(virtual_tcp.fd, F_GETFL, 0);
        fcntl(virtual_tcp.fd, F_SETFL, flags_sock | O_NONBLOCK);

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(0x7F000001);  // 127.0.0.1
        addr.sin_port = htons(dst_port);

        int ret = connect(virtual_tcp.fd, (struct sockaddr *)&addr, sizeof(addr));
        if (ret < 0 && errno != EINPROGRESS) {
            DEBUG("Uthernet II: TCP connect() failed: %s\n", strerror(errno));
            close(virtual_tcp.fd);
            virtual_tcp.fd = -1;
            inject_tcp_response(socknum, TCP_RST | TCP_ACK_FLAG, NULL, 0);
            return;
        }

        // Wait a bit for connection (non-blocking, but give it a chance)
        struct pollfd pfd = { virtual_tcp.fd, POLLOUT, 0 };
        poll(&pfd, 1, 100);  // 100ms timeout

        int err = 0;
        socklen_t errlen = sizeof(err);
        getsockopt(virtual_tcp.fd, SOL_SOCKET, SO_ERROR, &err, &errlen);
        if (err != 0) {
            DEBUG("Uthernet II: TCP connect failed: %s\n", strerror(err));
            close(virtual_tcp.fd);
            virtual_tcp.fd = -1;
            inject_tcp_response(socknum, TCP_RST | TCP_ACK_FLAG, NULL, 0);
            return;
        }

        // Save connection info
        memcpy(virtual_tcp.remote_mac, frame + ETH_SRC, 6);
        DEBUG("Uthernet II: TCP client MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
              frame[ETH_SRC], frame[ETH_SRC+1], frame[ETH_SRC+2],
              frame[ETH_SRC+3], frame[ETH_SRC+4], frame[ETH_SRC+5]);
        memcpy(virtual_tcp.remote_ip, ip + IPH_SRC, 4);
        memcpy(virtual_tcp.local_ip, ip + IPH_DST, 4);  // Remember what IP they connected to
        DEBUG("Uthernet II: TCP target IP: %d.%d.%d.%d\n",
              virtual_tcp.local_ip[0], virtual_tcp.local_ip[1],
              virtual_tcp.local_ip[2], virtual_tcp.local_ip[3]);
        virtual_tcp.remote_port = src_port;
        virtual_tcp.local_port = dst_port;
        virtual_tcp.our_seq = 12345;  // Initial sequence number
        virtual_tcp.their_seq = their_seq + 1;  // SYN counts as 1 byte
        virtual_tcp.established = false;
        virtual_tcp.fin_sent = false;
        virtual_tcp.fin_received = false;

        // Send SYN-ACK
        DEBUG("Uthernet II: TCP sending SYN-ACK\n");
        inject_tcp_response(socknum, TCP_SYN | TCP_ACK_FLAG, NULL, 0);
        virtual_tcp.our_seq++;  // SYN counts as 1 byte
        return;
    }

    // Handle ACK (completing handshake or acknowledging data)
    if (flags & TCP_ACK_FLAG) {
        if (!virtual_tcp.established && (flags & TCP_ACK_FLAG) && !(flags & TCP_SYN)) {
            DEBUG("Uthernet II: TCP handshake complete, connection established\n");
            virtual_tcp.established = true;
        }

        // Handle incoming data
        if (tcp_data_len > 0) {
            byte *data = tcp + tcp_header_len;
            DEBUG("Uthernet II: TCP received %d bytes of data\n", tcp_data_len);

            // Forward data to host socket
            if (virtual_tcp.fd >= 0) {
                ssize_t sent = send(virtual_tcp.fd, data, tcp_data_len, 0);
                if (sent > 0) {
                    DEBUG("Uthernet II: TCP forwarded %zd bytes to host\n", sent);
                }
            }

            // Update their sequence number
            virtual_tcp.their_seq = their_seq + tcp_data_len;

            // Send ACK
            inject_tcp_response(socknum, TCP_ACK_FLAG, NULL, 0);

            // Check for response data from host
            if (virtual_tcp.fd >= 0) {
                struct pollfd pfd = { virtual_tcp.fd, POLLIN, 0 };
                while (poll(&pfd, 1, 50) > 0) {  // Small timeout to gather data
                    byte recv_buf[1400];
                    ssize_t got = recv(virtual_tcp.fd, recv_buf, sizeof(recv_buf), 0);
                    if (got > 0) {
                        DEBUG("Uthernet II: TCP received %zd bytes from host\n", got);
                        inject_tcp_response(socknum, TCP_ACK_FLAG | TCP_PSH, recv_buf, got);
                        virtual_tcp.our_seq += got;
                    } else if (got == 0) {
                        // Connection closed by host
                        DEBUG("Uthernet II: TCP host closed connection\n");
                        inject_tcp_response(socknum, TCP_FIN | TCP_ACK_FLAG, NULL, 0);
                        virtual_tcp.fin_sent = true;
                        virtual_tcp.our_seq++;
                        break;
                    } else {
                        break;
                    }
                }
            }
        }
    }

    // Handle FIN
    if (flags & TCP_FIN) {
        DEBUG("Uthernet II: TCP FIN received\n");
        virtual_tcp.fin_received = true;
        virtual_tcp.their_seq++;  // FIN counts as 1 byte
        inject_tcp_response(socknum, TCP_ACK_FLAG, NULL, 0);

        if (!virtual_tcp.fin_sent) {
            inject_tcp_response(socknum, TCP_FIN | TCP_ACK_FLAG, NULL, 0);
            virtual_tcp.fin_sent = true;
            virtual_tcp.our_seq++;
        }

        if (virtual_tcp.fd >= 0) {
            close(virtual_tcp.fd);
            virtual_tcp.fd = -1;
        }
        virtual_tcp.established = false;
    }
}

static void inject_tcp_response(int socknum, byte flags, byte *data, int data_len)
{
    SocketState *ss = &u2.sockets[socknum];
    // Write to ss->rx_buf, APPENDING to existing data
    byte *pkt = ss->rx_buf;

    // Start writing at current tail position (append, don't overwrite)
    int pkt_base = ss->rx_tail;
    int pos = pkt_base + 2;  // Skip W5100 length prefix for this packet

    // Ethernet header
    memcpy(&pkt[pos + ETH_DST], virtual_tcp.remote_mac, 6);
    memcpy(&pkt[pos + ETH_SRC], VIRTUAL_GATEWAY_MAC, 6);
    pkt[pos + ETH_TYPE] = 0x08;
    pkt[pos + ETH_TYPE + 1] = 0x00;  // IPv4
    pos += ETH_HEADER_LEN;

    int ip_start = pos;
    int tcp_len = TCP_HEADER_LEN + data_len;
    int ip_len = IPH_HEADER_LEN + tcp_len;

    // IP header
    pkt[pos++] = 0x45;  // Version 4, IHL 5
    pkt[pos++] = 0x00;  // TOS
    pkt[pos++] = (ip_len >> 8) & 0xFF;
    pkt[pos++] = ip_len & 0xFF;
    pkt[pos++] = 0x00; pkt[pos++] = 0x01;  // ID
    pkt[pos++] = 0x00; pkt[pos++] = 0x00;  // Flags, fragment
    pkt[pos++] = 64;   // TTL
    pkt[pos++] = 6;    // Protocol = TCP
    pkt[pos++] = 0x00; pkt[pos++] = 0x00;  // Checksum placeholder

    // Source IP (the IP client connected to)
    memcpy(&pkt[pos], virtual_tcp.local_ip, 4);
    pos += 4;

    // Destination IP
    memcpy(&pkt[pos], virtual_tcp.remote_ip, 4);
    pos += 4;

    // Calculate IP checksum
    word ip_cksum = ip_checksum(&pkt[ip_start], IPH_HEADER_LEN);
    pkt[ip_start + 10] = ip_cksum >> 8;
    pkt[ip_start + 11] = ip_cksum & 0xFF;

    int tcp_start = pos;

    // TCP header
    pkt[pos++] = (virtual_tcp.local_port >> 8) & 0xFF;
    pkt[pos++] = virtual_tcp.local_port & 0xFF;
    pkt[pos++] = (virtual_tcp.remote_port >> 8) & 0xFF;
    pkt[pos++] = virtual_tcp.remote_port & 0xFF;

    // Sequence number
    pkt[pos++] = (virtual_tcp.our_seq >> 24) & 0xFF;
    pkt[pos++] = (virtual_tcp.our_seq >> 16) & 0xFF;
    pkt[pos++] = (virtual_tcp.our_seq >> 8) & 0xFF;
    pkt[pos++] = virtual_tcp.our_seq & 0xFF;

    // ACK number
    pkt[pos++] = (virtual_tcp.their_seq >> 24) & 0xFF;
    pkt[pos++] = (virtual_tcp.their_seq >> 16) & 0xFF;
    pkt[pos++] = (virtual_tcp.their_seq >> 8) & 0xFF;
    pkt[pos++] = virtual_tcp.their_seq & 0xFF;

    // Data offset (5 = 20 bytes), reserved, flags
    pkt[pos++] = 0x50;  // Data offset = 5 (20 bytes)
    pkt[pos++] = flags;

    // Window size (8KB)
    pkt[pos++] = 0x20; pkt[pos++] = 0x00;

    // Checksum placeholder
    pkt[pos++] = 0x00; pkt[pos++] = 0x00;

    // Urgent pointer
    pkt[pos++] = 0x00; pkt[pos++] = 0x00;

    // Data
    if (data && data_len > 0) {
        memcpy(&pkt[pos], data, data_len);
        pos += data_len;
    }

    // Calculate TCP checksum
    word tcp_cksum = tcp_checksum(&pkt[ip_start], &pkt[tcp_start], tcp_len);
    pkt[tcp_start + TCP_CHECKSUM] = tcp_cksum >> 8;
    pkt[tcp_start + TCP_CHECKSUM + 1] = tcp_cksum & 0xFF;

    // W5100 length prefix for THIS packet (at pkt_base)
    int pkt_len = pos - pkt_base;  // Length of this packet including prefix
    pkt[pkt_base + 0] = (pkt_len >> 8) & 0xFF;
    pkt[pkt_base + 1] = pkt_len & 0xFF;

    // Update RX buffer state - APPEND, don't reset head
    ss->rx_tail = pos;  // New tail is at end of this packet

    DEBUG("Uthernet II: Injected TCP response (flags=0x%02X, data=%d, pkt=%d bytes) RX: head=%d tail=%d\n",
          flags, data_len, pkt_len, ss->rx_head, ss->rx_tail);
}

// Poll virtual TCP connection for incoming data from host
static void virtual_tcp_poll(int socknum)
{
    if (virtual_tcp.fd < 0 || !virtual_tcp.established) {
        return;
    }

    SocketState *ss = &u2.sockets[socknum];

    // Check for data from host
    struct pollfd pfd = { virtual_tcp.fd, POLLIN, 0 };
    if (poll(&pfd, 1, 0) > 0) {
        byte recv_buf[1400];
        ssize_t got = recv(virtual_tcp.fd, recv_buf, sizeof(recv_buf), 0);

        if (got > 0) {
            DEBUG("Uthernet II: TCP received %zd bytes from host (poll)\n", got);
            inject_tcp_response(socknum, TCP_ACK_FLAG | TCP_PSH, recv_buf, got);
            virtual_tcp.our_seq += got;
        } else if (got == 0) {
            // Host closed connection
            DEBUG("Uthernet II: TCP host closed connection (poll)\n");
            if (!virtual_tcp.fin_sent) {
                inject_tcp_response(socknum, TCP_FIN | TCP_ACK_FLAG, NULL, 0);
                virtual_tcp.fin_sent = true;
                virtual_tcp.our_seq++;
            }
            close(virtual_tcp.fd);
            virtual_tcp.fd = -1;
        }
    }
}

// Handle MACRAW mode SEND - check for DHCP and respond virtually
static void handle_macraw_send(int socknum)
{
    word base = get_socket_base(socknum);
    SocketState *ss = &u2.sockets[socknum];

    // Get TX pointers
    word tx_rd = WORD(u2.memory[base + Sn_TX_RD + 1], u2.memory[base + Sn_TX_RD]);
    word tx_wr = WORD(u2.memory[base + Sn_TX_WR + 1], u2.memory[base + Sn_TX_WR]);
    word tx_base = get_tx_base(socknum);
    word tx_mask = SOCK_BUF_SIZE - 1;

    // Calculate frame size
    int frame_len = (tx_wr - tx_rd) & tx_mask;
    if (frame_len <= 0 || frame_len > 1600) {
        DEBUG("Uthernet II: MACRAW invalid frame len %d\n", frame_len);
        return;
    }

    // Read frame from TX buffer
    byte frame[1600];
    for (int i = 0; i < frame_len; i++) {
        word addr = tx_base + ((tx_rd - tx_base + i) & tx_mask);
        frame[i] = u2.memory[addr];
    }

    // Update TX read pointer
    u2.memory[base + Sn_TX_RD + 0] = HI(tx_wr);
    u2.memory[base + Sn_TX_RD + 1] = LO(tx_wr);

    DEBUG("Uthernet II: MACRAW send %d bytes\n", frame_len);

    // Check for DHCP
    int dhcp_type = detect_dhcp_type(frame, frame_len);
    if (dhcp_type > 0) {
        DEBUG("Uthernet II: Detected DHCP type %d\n", dhcp_type);

        byte *dhcp = frame + ETH_HEADER_LEN + IPH_HEADER_LEN + UDP_HEADER_LEN;

        // Save transaction ID and client MAC
        memcpy(u2.dhcp_xid, dhcp + DHCP_XID, 4);
        memcpy(u2.client_mac, dhcp + DHCP_CHADDR, 6);

        if (dhcp_type == DHCP_DISCOVER) {
            DEBUG("Uthernet II: DHCP DISCOVER -> sending OFFER\n");
            u2.dhcp_state = DHCP_DISCOVER_SEEN;
            inject_dhcp_response(socknum, false);  // Send OFFER
            u2.dhcp_state = DHCP_OFFER_SENT;
        } else if (dhcp_type == DHCP_REQUEST) {
            DEBUG("Uthernet II: DHCP REQUEST -> sending ACK\n");
            u2.dhcp_state = DHCP_REQUEST_SEEN;
            inject_dhcp_response(socknum, true);   // Send ACK
            u2.dhcp_state = DHCP_COMPLETE;

            // Also update the W5100 IP configuration
            memcpy(&u2.memory[W5100_SIPR], VIRTUAL_CLIENT_IP, 4);
            memcpy(&u2.memory[W5100_GAR], VIRTUAL_GATEWAY, 4);
            memcpy(&u2.memory[W5100_SUBR], VIRTUAL_SUBNET, 4);
        }
        return;  // Handled DHCP
    }

    // Check EtherType
    if (frame_len >= ETH_HEADER_LEN) {
        word ethertype = (frame[ETH_TYPE] << 8) | frame[ETH_TYPE + 1];

        // Check for ARP (0x0806)
        if (ethertype == 0x0806) {
            handle_arp_packet(socknum, frame, frame_len);
            return;
        }

        // Check for IPv4 (0x0800)
        if (ethertype == 0x0800 && frame_len >= ETH_HEADER_LEN + IPH_HEADER_LEN) {
            byte *ip = frame + ETH_HEADER_LEN;
            byte protocol = ip[IPH_PROTO];

            // Check for TCP (protocol 6)
            if (protocol == 6) {
                // Only handle TCP to gateway IPs (192.168.64.x or 192.168.65.x)
                byte dst_ip0 = ip[IPH_DST];
                byte dst_ip1 = ip[IPH_DST + 1];
                byte dst_ip2 = ip[IPH_DST + 2];

                if (dst_ip0 == 192 && dst_ip1 == 168 && (dst_ip2 == 64 || dst_ip2 == 65)) {
                    handle_tcp_packet(socknum, frame, frame_len);
                    return;
                }
            }
        }
    }
}

//=============================================================================
// W5100 Core Implementation
//=============================================================================

static void w5100_reset(void)
{
    // First, close any open sockets (only if they're valid fds, not stdin/stdout/stderr)
    for (int i = 0; i < 4; i++) {
        if (u2.initialized && u2.sockets[i].fd > 2) {
            close(u2.sockets[i].fd);
        }
    }

    memset(&u2, 0, sizeof(u2));

    // Set default values
    u2.mode = 0;
    u2.addr_ptr = 0;

    // Default MAC address (locally administered)
    u2.memory[W5100_SHAR + 0] = 0x02;
    u2.memory[W5100_SHAR + 1] = 0x00;
    u2.memory[W5100_SHAR + 2] = 0xDE;
    u2.memory[W5100_SHAR + 3] = 0xAD;
    u2.memory[W5100_SHAR + 4] = 0xBE;
    u2.memory[W5100_SHAR + 5] = 0xEF;

    // Default IP configuration (can be changed by software)
    // IP: 192.168.1.100
    u2.memory[W5100_SIPR + 0] = 192;
    u2.memory[W5100_SIPR + 1] = 168;
    u2.memory[W5100_SIPR + 2] = 1;
    u2.memory[W5100_SIPR + 3] = 100;

    // Gateway: 192.168.1.1
    u2.memory[W5100_GAR + 0] = 192;
    u2.memory[W5100_GAR + 1] = 168;
    u2.memory[W5100_GAR + 2] = 1;
    u2.memory[W5100_GAR + 3] = 1;

    // Subnet: 255.255.255.0
    u2.memory[W5100_SUBR + 0] = 255;
    u2.memory[W5100_SUBR + 1] = 255;
    u2.memory[W5100_SUBR + 2] = 255;
    u2.memory[W5100_SUBR + 3] = 0;

    // Retry Time Register (RTR) at $0017-$0018 = 2000 ($07D0)
    // IP65 checks this during init!
    u2.memory[W5100_RTR + 0] = 0x07;     // High byte
    u2.memory[W5100_RTR + 1] = 0xD0;     // Low byte

    // Retry Count Register (RCR) at $0019 = 8 (default)
    u2.memory[W5100_RCR] = 0x08;

    // Default buffer sizes (2KB per socket)
    u2.memory[W5100_RMSR] = 0x55;  // 2KB each socket
    u2.memory[W5100_TMSR] = 0x55;  // 2KB each socket

    // PPTLR = 0x00 indicates virtual/emulated W5100
    u2.memory[W5100_PPTLR] = 0x00;

    // Initialize all sockets
    for (int i = 0; i < 4; i++) {
        word base = get_socket_base(i);
        u2.memory[base + Sn_SR] = Sn_SR_CLOSED;
        u2.memory[base + Sn_TTL] = 128;  // Default TTL

        // Initialize TX pointers
        word tx_base = get_tx_base(i);
        u2.memory[base + Sn_TX_RD + 0] = HI(tx_base);
        u2.memory[base + Sn_TX_RD + 1] = LO(tx_base);
        u2.memory[base + Sn_TX_WR + 0] = HI(tx_base);
        u2.memory[base + Sn_TX_WR + 1] = LO(tx_base);

        // TX Free Size = full buffer
        u2.memory[base + Sn_TX_FSR + 0] = HI(SOCK_BUF_SIZE);
        u2.memory[base + Sn_TX_FSR + 1] = LO(SOCK_BUF_SIZE);

        // Initialize RX pointers
        word rx_base = get_rx_base(i);
        u2.memory[base + Sn_RX_RD + 0] = HI(rx_base);
        u2.memory[base + Sn_RX_RD + 1] = LO(rx_base);

        // RX Received Size = 0
        u2.memory[base + Sn_RX_RSR + 0] = 0;
        u2.memory[base + Sn_RX_RSR + 1] = 0;

        // Initialize socket state (fd was already closed above if needed)
        u2.sockets[i].fd = -1;
        u2.sockets[i].connecting = false;
        u2.sockets[i].rx_head = 0;
        u2.sockets[i].rx_tail = 0;
    }

    u2.initialized = true;
    DEBUG("Uthernet II: Reset complete\n");
}

static word get_socket_base(int socknum)
{
    return W5100_S0_BASE + (socknum * 0x100);
}

static word get_tx_base(int socknum)
{
    return W5100_TX_BASE + (socknum * SOCK_BUF_SIZE);
}

static word get_rx_base(int socknum)
{
    return W5100_RX_BASE + (socknum * SOCK_BUF_SIZE);
}

static byte w5100_read(word addr)
{
    // Bounds check
    if (addr >= 0x8000) {
        return 0;
    }


    // Special handling for socket status - poll for updates
    if (addr >= W5100_S0_BASE && addr < W5100_TX_BASE) {
        int socknum = (addr - W5100_S0_BASE) / 0x100;
        int offset = (addr - W5100_S0_BASE) % 0x100;

        if (socknum < 4) {
            // Poll for socket state changes
            socket_poll(socknum);
            // Also poll virtual TCP for MACRAW mode
            if (u2.sockets[socknum].macraw_mode) {
                virtual_tcp_poll(socknum);
            }

            // Handle special read-only registers
            word base = get_socket_base(socknum);

            if (offset == Sn_TX_FSR || offset == Sn_TX_FSR + 1) {
                // TX Free Size - calculate from pointers
                word tx_rd = WORD(u2.memory[base + Sn_TX_RD + 1],
                                 u2.memory[base + Sn_TX_RD]);
                word tx_wr = WORD(u2.memory[base + Sn_TX_WR + 1],
                                 u2.memory[base + Sn_TX_WR]);
                word fsr = SOCK_BUF_SIZE - ((tx_wr - tx_rd) & (SOCK_BUF_SIZE - 1));

                if (offset == Sn_TX_FSR) {
                    return HI(fsr);
                } else {
                    return LO(fsr);
                }
            }

            if (offset == Sn_RX_RSR || offset == Sn_RX_RSR + 1) {
                // RX Received Size - return buffered amount
                SocketState *ss = &u2.sockets[socknum];
                word rsr = (ss->rx_tail - ss->rx_head) & 0x0FFF;

                if (rsr > 0) {
                    DEBUG("Uthernet II: Socket %d RX_RSR=%d (tail=%d head=%d)\n",
                          socknum, rsr, ss->rx_tail, ss->rx_head);
                }

                if (offset == Sn_RX_RSR) {
                    return HI(rsr);
                } else {
                    return LO(rsr);
                }
            }
        }
    }

    // RX buffer read - return from local buffer or u2.memory (for MACRAW virtual)
    if (addr >= W5100_RX_BASE && addr < W5100_RX_BASE + W5100_RX_SIZE) {
        int socknum = (addr - W5100_RX_BASE) / SOCK_BUF_SIZE;
        if (socknum < 4) {
            SocketState *ss = &u2.sockets[socknum];

            // Direct offset into socket's RX buffer
            word rx_base = get_rx_base(socknum);
            word buf_offset = (addr - rx_base) & (SOCK_BUF_SIZE - 1);

            if (buf_offset < sizeof(ss->rx_buf)) {
                return ss->rx_buf[buf_offset];
            }
        }
    }

    return u2.memory[addr];
}

static void w5100_write(word addr, byte val)
{
    // Bounds check
    if (addr >= 0x8000) {
        return;
    }

    // Mode Register special handling
    if (addr == W5100_MR) {
        if (val & MR_RST) {
            w5100_reset();
            return;
        }
        u2.memory[addr] = val;
        return;
    }

    // Socket command register - execute commands
    if (addr >= W5100_S0_BASE && addr < W5100_TX_BASE) {
        int socknum = (addr - W5100_S0_BASE) / 0x100;
        int offset = (addr - W5100_S0_BASE) % 0x100;

        if (socknum < 4 && offset == Sn_CR) {
            INFO("Uthernet II: Socket %d cmd write 0x%02X\n", socknum, val);
            socket_command(socknum, val);
            return;
        }
        if (socknum < 4 && offset == Sn_MR) {
            INFO("Uthernet II: Socket %d mode write 0x%02X\n", socknum, val);
        }
        if (socknum < 4 && (offset == Sn_RX_RD || offset == Sn_RX_RD + 1)) {
            INFO("Uthernet II: Socket %d RX_RD[%d] write 0x%02X\n",
                 socknum, offset - Sn_RX_RD, val);
        }
    }

    // TX buffer write
    if (addr >= W5100_TX_BASE && addr < W5100_TX_BASE + W5100_TX_SIZE) {
        // Store in W5100 memory (will be sent on SEND command)
        u2.memory[addr] = val;
        return;
    }

    // Trace common register writes
    if (addr < 0x0030) {
        INFO("Uthernet II: Common reg write addr=0x%04X val=0x%02X\n",
             addr, val);
    }

    // Default: store in memory
    u2.memory[addr] = val;
}

static void socket_command(int socknum, byte cmd)
{
    word base = get_socket_base(socknum);
    SocketState *ss = &u2.sockets[socknum];
    byte mode = u2.memory[base + Sn_MR];

    DEBUG("Uthernet II: Socket %d command 0x%02X (mode=0x%02X)\n",
          socknum, cmd, mode);

    switch (cmd) {
        case Sn_CR_OPEN: {
            // Open socket based on mode
            if (mode == Sn_MR_TCP) {
                ss->fd = socket(AF_INET, SOCK_STREAM, 0);
                if (ss->fd >= 0) {
                    // Set non-blocking
                    int flags = fcntl(ss->fd, F_GETFL, 0);
                    fcntl(ss->fd, F_SETFL, flags | O_NONBLOCK);

                    u2.memory[base + Sn_SR] = Sn_SR_INIT;
                    DEBUG("Uthernet II: Socket %d opened (TCP), fd=%d\n",
                          socknum, ss->fd);
                } else {
                    DEBUG("Uthernet II: Socket %d open failed: %s\n",
                          socknum, strerror(errno));
                }
            } else if (mode == Sn_MR_UDP) {
                ss->fd = socket(AF_INET, SOCK_DGRAM, 0);
                if (ss->fd >= 0) {
                    // Set non-blocking
                    int flags = fcntl(ss->fd, F_GETFL, 0);
                    fcntl(ss->fd, F_SETFL, flags | O_NONBLOCK);

                    u2.memory[base + Sn_SR] = Sn_SR_UDP;
                    DEBUG("Uthernet II: Socket %d opened (UDP), fd=%d\n",
                          socknum, ss->fd);
                }
            } else if ((mode & 0x0F) == Sn_MR_MACRAW && socknum == 0) {
                // MACRAW mode - only valid on socket 0
                // No actual host socket needed for virtual DHCP
                ss->fd = -1;  // No real socket
                ss->macraw_mode = true;
                ss->rx_head = 0;
                ss->rx_tail = 0;

                // Initialize RX_RD pointer to buffer base
                word rx_base = get_rx_base(socknum);
                u2.memory[base + Sn_RX_RD + 0] = HI(rx_base);
                u2.memory[base + Sn_RX_RD + 1] = LO(rx_base);

                u2.memory[base + Sn_SR] = Sn_SR_MACRAW;
                INFO("Uthernet II: Socket 0 opened (MACRAW mode=0x%02X) RX_RD=0x%04X\n",
                     mode, rx_base);
            }
            break;
        }

        case Sn_CR_LISTEN: {
            if (ss->fd >= 0 && u2.memory[base + Sn_SR] == Sn_SR_INIT) {
                // Bind and listen
                struct sockaddr_in addr;
                memset(&addr, 0, sizeof(addr));
                addr.sin_family = AF_INET;
                addr.sin_addr.s_addr = INADDR_ANY;
                addr.sin_port = htons(WORD(u2.memory[base + Sn_PORT + 1],
                                          u2.memory[base + Sn_PORT]));

                if (bind(ss->fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
                    if (listen(ss->fd, 1) == 0) {
                        u2.memory[base + Sn_SR] = Sn_SR_LISTEN;
                        DEBUG("Uthernet II: Socket %d listening on port %d\n",
                              socknum, ntohs(addr.sin_port));
                    }
                }
            }
            break;
        }

        case Sn_CR_CONNECT: {
            if (ss->fd >= 0 && u2.memory[base + Sn_SR] == Sn_SR_INIT) {
                // Connect to destination
                struct sockaddr_in addr;
                memset(&addr, 0, sizeof(addr));
                addr.sin_family = AF_INET;

                // Build destination IP from W5100 registers
                byte ip0 = u2.memory[base + Sn_DIPR + 0];
                byte ip1 = u2.memory[base + Sn_DIPR + 1];
                byte ip2 = u2.memory[base + Sn_DIPR + 2];
                byte ip3 = u2.memory[base + Sn_DIPR + 3];

                // Virtual network redirect: 192.168.64.x or 192.168.65.x -> localhost
                // This allows Apple II software to connect to "gateway" addresses
                // which actually reach the host running the emulator
                if (ip0 == 192 && ip1 == 168 && (ip2 == 64 || ip2 == 65)) {
                    DEBUG("Uthernet II: Redirecting %d.%d.%d.%d to localhost\n",
                          ip0, ip1, ip2, ip3);
                    addr.sin_addr.s_addr = htonl(0x7F000001);  // 127.0.0.1
                } else {
                    addr.sin_addr.s_addr = htonl(
                        (ip0 << 24) | (ip1 << 16) | (ip2 << 8) | ip3);
                }

                addr.sin_port = htons(WORD(u2.memory[base + Sn_DPORT + 1],
                                          u2.memory[base + Sn_DPORT]));

                DEBUG("Uthernet II: Socket %d connecting to %d.%d.%d.%d:%d\n",
                      socknum,
                      u2.memory[base + Sn_DIPR + 0],
                      u2.memory[base + Sn_DIPR + 1],
                      u2.memory[base + Sn_DIPR + 2],
                      u2.memory[base + Sn_DIPR + 3],
                      ntohs(addr.sin_port));

                int ret = connect(ss->fd, (struct sockaddr *)&addr, sizeof(addr));
                if (ret == 0) {
                    // Immediate success
                    u2.memory[base + Sn_SR] = Sn_SR_ESTABLISHED;
                    DEBUG("Uthernet II: Socket %d connected immediately\n", socknum);
                } else if (errno == EINPROGRESS) {
                    // Connection in progress
                    ss->connecting = true;
                    u2.memory[base + Sn_SR] = Sn_SR_SYNSENT;
                    DEBUG("Uthernet II: Socket %d connecting...\n", socknum);
                } else {
                    DEBUG("Uthernet II: Socket %d connect failed: %s\n",
                          socknum, strerror(errno));
                    u2.memory[base + Sn_SR] = Sn_SR_CLOSED;
                }
            }
            break;
        }

        case Sn_CR_DISCON:
        case Sn_CR_CLOSE: {
            if (ss->fd >= 0) {
                close(ss->fd);
                ss->fd = -1;
            }
            ss->connecting = false;
            ss->macraw_mode = false;
            ss->rx_head = 0;
            ss->rx_tail = 0;
            u2.memory[base + Sn_SR] = Sn_SR_CLOSED;
            DEBUG("Uthernet II: Socket %d closed\n", socknum);
            break;
        }

        case Sn_CR_SEND: {
            // MACRAW mode - handle virtual network
            if (u2.memory[base + Sn_SR] == Sn_SR_MACRAW && ss->macraw_mode) {
                handle_macraw_send(socknum);
                break;
            }

            if (ss->fd >= 0 && u2.memory[base + Sn_SR] == Sn_SR_ESTABLISHED) {
                // Get TX pointers
                word tx_rd = WORD(u2.memory[base + Sn_TX_RD + 1],
                                 u2.memory[base + Sn_TX_RD]);
                word tx_wr = WORD(u2.memory[base + Sn_TX_WR + 1],
                                 u2.memory[base + Sn_TX_WR]);

                // Calculate send size
                word tx_base = get_tx_base(socknum);
                word send_size = (tx_wr - tx_rd) & (SOCK_BUF_SIZE - 1);

                if (send_size > 0) {
                    // Gather data from circular buffer
                    byte sendbuf[SOCK_BUF_SIZE];
                    for (word i = 0; i < send_size; i++) {
                        word addr = tx_base + ((tx_rd - tx_base + i) & (SOCK_BUF_SIZE - 1));
                        sendbuf[i] = u2.memory[addr];
                    }

                    // Send to host socket
                    ssize_t sent = send(ss->fd, sendbuf, send_size, 0);
                    if (sent > 0) {
                        // Update TX read pointer
                        tx_rd = (tx_rd + sent);
                        u2.memory[base + Sn_TX_RD + 0] = HI(tx_rd);
                        u2.memory[base + Sn_TX_RD + 1] = LO(tx_rd);
                        DEBUG("Uthernet II: Socket %d sent %zd bytes\n",
                              socknum, sent);
                    }
                }
            }
            break;
        }

        case Sn_CR_RECV: {
            // Works for both regular sockets and MACRAW mode
            if (ss->fd >= 0 || ss->macraw_mode) {
                // RECV command: software is acknowledging it has read data
                word rx_rd = WORD(u2.memory[base + Sn_RX_RD + 1],
                                 u2.memory[base + Sn_RX_RD]);
                word rx_base = get_rx_base(socknum);

                // Calculate what the software claims to have read
                word claimed_read = (rx_rd - rx_base) & (SOCK_BUF_SIZE - 1);

                INFO("Uthernet II: Socket %d RECV: rx_rd=0x%04X, head=%d->%d, tail=%d\n",
                     socknum, rx_rd, ss->rx_head, claimed_read, ss->rx_tail);

                // For MACRAW mode: advance head by amount consumed
                // Only clear buffer if completely empty
                if (ss->macraw_mode) {
                    int consumed = (claimed_read - ss->rx_head) & (SOCK_BUF_SIZE - 1);
                    if (consumed > 0) {
                        ss->rx_head += consumed;
                        INFO("Uthernet II: MACRAW consumed %d bytes, head=%d tail=%d remaining=%d\n",
                             consumed, ss->rx_head, ss->rx_tail, ss->rx_tail - ss->rx_head);

                        // If buffer is now empty, reset to start for efficiency
                        if (ss->rx_head >= ss->rx_tail) {
                            ss->rx_head = 0;
                            ss->rx_tail = 0;
                            u2.memory[base + Sn_RX_RD + 0] = HI(rx_base);
                            u2.memory[base + Sn_RX_RD + 1] = LO(rx_base);
                            INFO("Uthernet II: MACRAW buffer empty, reset\n");
                        }
                    }
                } else if (claimed_read != ss->rx_head) {
                    ss->rx_head = claimed_read;
                }
            }
            break;
        }
    }

    // Command register always reads as 0 after command completes
    u2.memory[base + Sn_CR] = 0;
}

static void socket_poll(int socknum)
{
    SocketState *ss = &u2.sockets[socknum];
    word base = get_socket_base(socknum);

    if (ss->fd < 0) {
        return;
    }

    // Check for connect completion
    if (ss->connecting) {
        struct pollfd pfd = { ss->fd, POLLOUT, 0 };
        if (poll(&pfd, 1, 0) > 0) {
            int err = 0;
            socklen_t len = sizeof(err);
            getsockopt(ss->fd, SOL_SOCKET, SO_ERROR, &err, &len);

            if (err == 0) {
                u2.memory[base + Sn_SR] = Sn_SR_ESTABLISHED;
                DEBUG("Uthernet II: Socket %d connected\n", socknum);
            } else {
                u2.memory[base + Sn_SR] = Sn_SR_CLOSED;
                DEBUG("Uthernet II: Socket %d connect failed: %s\n",
                      socknum, strerror(err));
            }
            ss->connecting = false;
        }
    }

    // Check for incoming data (if established)
    if (u2.memory[base + Sn_SR] == Sn_SR_ESTABLISHED) {
        struct pollfd pfd = { ss->fd, POLLIN, 0 };
        if (poll(&pfd, 1, 0) > 0) {
            // Room in local buffer?
            word space = sizeof(ss->rx_buf) - ((ss->rx_tail - ss->rx_head) & 0x0FFF);
            if (space > 0) {
                // Read into circular buffer
                word write_pos = ss->rx_tail & (sizeof(ss->rx_buf) - 1);
                word can_read = sizeof(ss->rx_buf) - write_pos;
                if (can_read > space) can_read = space;

                ssize_t got = recv(ss->fd, &ss->rx_buf[write_pos], can_read, 0);
                if (got > 0) {
                    ss->rx_tail = (ss->rx_tail + got) & 0x0FFF;
                    DEBUG("Uthernet II: Socket %d received %zd bytes\n",
                          socknum, got);
                } else if (got == 0) {
                    // Connection closed by peer
                    u2.memory[base + Sn_SR] = Sn_SR_CLOSE_WAIT;
                    DEBUG("Uthernet II: Socket %d peer disconnected\n", socknum);
                }
            }
        }
    }

    // Check for incoming connections (if listening)
    if (u2.memory[base + Sn_SR] == Sn_SR_LISTEN) {
        struct pollfd pfd = { ss->fd, POLLIN, 0 };
        if (poll(&pfd, 1, 0) > 0) {
            struct sockaddr_in client_addr;
            socklen_t addrlen = sizeof(client_addr);
            int newfd = accept(ss->fd, (struct sockaddr *)&client_addr, &addrlen);
            if (newfd >= 0) {
                // Close listening socket, use accepted socket
                close(ss->fd);
                ss->fd = newfd;

                // Set non-blocking
                int flags = fcntl(ss->fd, F_GETFL, 0);
                fcntl(ss->fd, F_SETFL, flags | O_NONBLOCK);

                u2.memory[base + Sn_SR] = Sn_SR_ESTABLISHED;
                DEBUG("Uthernet II: Socket %d accepted connection\n", socknum);
            }
        }
    }
}

static byte handler(word loc, int val, int ploc, int psw)
{
    // We only handle soft switches ($C0nX)
    if (psw == -1) {
        // ROM area - return ID bytes
        if (ploc >= 0) {
            // ID bytes for Uthernet II detection
            // These are checked by IP65 and other software
            if (ploc == 0x00) return 0x00;
            if (ploc == 0x01) return 0x00;
            if (ploc == 0x05) return 0x38;  // ID byte
            if (ploc == 0x07) return 0x18;  // ID byte
            if (ploc == 0xFF) return 0x00;  // Entry point (none)
            return 0x00;
        }
        return 0;
    }

    // Handle soft switch access
    byte result = 0;

    switch (psw) {
        case SW_MODE_REG:
            if (val == -1) {
                // Read mode register
                result = u2.mode;
            } else {
                // Write mode register
                // Bit 7 (0x80) triggers W5100 reset
                if (val & 0x80) {
                    INFO("Uthernet II: Reset via mode register\n");
                    w5100_reset();
                    u2.mode = val & 0x7F;  // Clear reset bit, keep other bits
                } else {
                    u2.mode = val;
                }
                DEBUG("Uthernet II: Mode set to 0x%02X\n", u2.mode);
            }
            break;

        case SW_ADDR_HI:
            if (val == -1) {
                result = HI(u2.addr_ptr);
            } else {
                u2.addr_ptr = WORD(LO(u2.addr_ptr), val);
                INFO("Uthernet II: Addr ptr hi = 0x%02X (ptr=0x%04X)\n",
                     val, u2.addr_ptr);
            }
            break;

        case SW_ADDR_LO:
            if (val == -1) {
                result = LO(u2.addr_ptr);
            } else {
                u2.addr_ptr = WORD(val, HI(u2.addr_ptr));
                INFO("Uthernet II: Addr ptr lo = 0x%02X (ptr=0x%04X)\n",
                     val, u2.addr_ptr);
            }
            break;

        case SW_DATA_REG:
            if (val == -1) {
                // Read from W5100 memory
                result = w5100_read(u2.addr_ptr);
                INFO("Uthernet II: Data read [0x%04X] = 0x%02X\n",
                     u2.addr_ptr, result);
            } else {
                // Write to W5100 memory
                INFO("Uthernet II: Data write [0x%04X] = 0x%02X\n",
                     u2.addr_ptr, val);
                w5100_write(u2.addr_ptr, val);
            }

            // Auto-increment if enabled
            if (u2.mode & MR_AI) {
                u2.addr_ptr++;
            }
            break;

        default:
            // Other switches not implemented
            break;
    }

    return result;
}

static void init(void)
{
    DEBUG("Uthernet II: Initializing in slot %d\n", slot_num);
    w5100_reset();
}

// Public configuration function
void uthernet2_set_slot(unsigned int slot)
{
    if (slot >= 1 && slot <= 7) {
        slot_num = slot;
    }
}

PeriphDesc uthernet2 = {
    init,
    handler,
};
