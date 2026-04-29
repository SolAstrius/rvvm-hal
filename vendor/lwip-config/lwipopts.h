/* lwIP options — rvvm-hal configuration.
 *
 * Single-threaded raw-API mode (NO_SYS=1): no scheduler, no socket
 * API, no os layer. The firmware is responsible for calling
 * net_poll() at intervals to drive lwIP timers and process the
 * RX queue. This is the standard "embedded baremetal" lwIP shape.
 *
 * IPv4 only — IPv6 omitted to keep firmware sub-30 KiB. Easy to
 * enable later (set LWIP_IPV6=1 and add the ipv6 dir's .c files
 * to the build).
 *
 * Memory: ~32 KiB peak for default pool sizing. Goes into BSS.
 *
 * Documentation: lwIP options are well-described in
 * vendor/lwip/src/include/lwip/opt.h. We override only what differs
 * from the default. */

#pragma once

/* ---------- platform integration ---------- */

#define NO_SYS                          1     /* raw API only, no threads */
#define SYS_LIGHTWEIGHT_PROT            0     /* no critical-section macros needed */

/* Endianness — RV is little-endian. */
#define BYTE_ORDER                      LITTLE_ENDIAN

/* ---------- protocols ---------- */

#define LWIP_IPV4                       1
#define LWIP_IPV6                       0
#define LWIP_ARP                        1
#define LWIP_ETHERNET                   1
#define LWIP_ICMP                       1
#define LWIP_RAW                        1
#define LWIP_DHCP                       1
#define LWIP_DNS                        1
#define LWIP_UDP                        1
#define LWIP_TCP                        1
#define LWIP_AUTOIP                     0
#define LWIP_IGMP                       0     /* multicast — enable if needed */
#define LWIP_NETCONN                    0     /* sequential API not in NO_SYS */
#define LWIP_SOCKET                     0     /* sockets need NETCONN */

/* ---------- memory ---------- */

#define MEM_LIBC_MALLOC                 0     /* use lwIP's mem.c, not picolibc malloc */
#define MEMP_MEM_MALLOC                 0
#define MEM_ALIGNMENT                   8
#define MEM_SIZE                        (16 * 1024)  /* heap for pbufs etc */
#define MEMP_NUM_PBUF                   16
#define MEMP_NUM_RAW_PCB                4
#define MEMP_NUM_UDP_PCB                4
#define MEMP_NUM_TCP_PCB                4
#define MEMP_NUM_TCP_PCB_LISTEN         2
#define MEMP_NUM_TCP_SEG                16
#define MEMP_NUM_REASSDATA              4
#define MEMP_NUM_FRAG_PBUF              4
#define MEMP_NUM_ARP_QUEUE              4
#define MEMP_NUM_SYS_TIMEOUT            10
#define PBUF_POOL_SIZE                  16
#define PBUF_POOL_BUFSIZE               1536  /* >= 1518 (max ethernet) */

/* ---------- TCP ---------- */

#define TCP_MSS                         1460  /* 1500 - 40 (IP+TCP headers) */
#define TCP_WND                         (4 * TCP_MSS)
#define TCP_SND_BUF                     (4 * TCP_MSS)
#define TCP_SND_QUEUELEN                ((4 * (TCP_SND_BUF) + (TCP_MSS - 1))/(TCP_MSS))

/* ---------- ARP ---------- */

#define ARP_TABLE_SIZE                  10
#define ARP_QUEUEING                    1     /* queue packets while ARP resolves */

/* ---------- IP ---------- */

#define IP_REASSEMBLY                   1
#define IP_FRAG                         1
#define IP_REASS_MAX_PBUFS              4
#define IP_FRAG_USES_STATIC_BUF         0
#define IP_DEFAULT_TTL                  64

/* ---------- DHCP ---------- */

#define DHCP_DOES_ARP_CHECK             0     /* skip the gratuitous ARP — faster boot on RVVM tap */

/* ---------- DNS ---------- */

#define DNS_TABLE_SIZE                  4
#define DNS_MAX_NAME_LENGTH             64
#define DNS_MAX_SERVERS                 2

/* ---------- statistics & debug ---------- */

#define LWIP_STATS                      0     /* off by default; flip on for tuning */
#define LWIP_NOASSERT                   1     /* drop assert bodies — picolibc assert
                                                 was pulling fprintf with %s, adding
                                                 ~3 KiB. Re-enable while debugging. */

/* Checksums: let lwIP do them in software. RVVM's RTL8169 emulation
 * doesn't offload, and our packets are small. */
#define CHECKSUM_GEN_IP                 1
#define CHECKSUM_GEN_TCP                1
#define CHECKSUM_GEN_UDP                1
#define CHECKSUM_GEN_ICMP               1
#define CHECKSUM_CHECK_IP               1
#define CHECKSUM_CHECK_TCP              1
#define CHECKSUM_CHECK_UDP              1
#define CHECKSUM_CHECK_ICMP             1

/* ---------- application layer protocols (off by default) ---------- */

#define LWIP_HTTPD                      0
#define LWIP_HTTPD_SSI                  0
#define LWIP_HTTPD_CGI                  0
