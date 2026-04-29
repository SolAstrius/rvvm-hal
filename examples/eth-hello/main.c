/* eth-hello — RTL8169 driver smoke test.
 *
 * Boots, brings up the NIC, prints the MAC, broadcasts an ARP
 * request asking for the gateway's MAC, then prints every frame
 * that arrives for 5 seconds. Demonstrates raw L2 RX/TX without a
 * TCP/IP stack — that's what lwIP would slot in on top.
 *
 *   make
 *   make run
 *
 * RVVM's user-mode tap_user gives us a 10.0.2.0/24 network with the
 * gateway at 10.0.2.2 and DHCP server at 10.0.2.3 (slirp defaults).
 * We don't run DHCP yet — just ARP at the gateway and dump replies. */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "uart.h"
#include "fdt.h"
#include "pci.h"
#include "time.h"
#include "rvvm.h"
#include "eth.h"

extern char __bss_start[], __bss_end[];

static eth_t nic;

/* Build a minimum ARP-who-has packet:
 *   ethernet header (14 B): dst=ff:ff:ff:ff:ff:ff, src=our MAC, ethertype=0x0806
 *   ARP body (28 B): htype=1, ptype=0x0800, hlen=6, plen=4, op=1 (request),
 *                    sender MAC, sender IP, target MAC=00:00:00:00:00:00,
 *                    target IP. */
static int build_arp_request(uint8_t *frame, const uint8_t mac[6],
                             uint32_t my_ip, uint32_t target_ip) {
    /* dst MAC: broadcast */
    memset(&frame[0], 0xFF, 6);
    /* src MAC */
    memcpy(&frame[6], mac, 6);
    /* ethertype: 0x0806 (ARP), big-endian */
    frame[12] = 0x08; frame[13] = 0x06;

    /* ARP body */
    uint8_t *p = &frame[14];
    p[0] = 0x00; p[1] = 0x01;     /* htype = ethernet */
    p[2] = 0x08; p[3] = 0x00;     /* ptype = IPv4 */
    p[4] = 6;                     /* hlen */
    p[5] = 4;                     /* plen */
    p[6] = 0x00; p[7] = 0x01;     /* opcode = request */
    memcpy(&p[8], mac, 6);        /* sender MAC */
    p[14] = (my_ip >> 24) & 0xFF;
    p[15] = (my_ip >> 16) & 0xFF;
    p[16] = (my_ip >>  8) & 0xFF;
    p[17] = (my_ip >>  0) & 0xFF;
    memset(&p[18], 0, 6);          /* target MAC unknown */
    p[24] = (target_ip >> 24) & 0xFF;
    p[25] = (target_ip >> 16) & 0xFF;
    p[26] = (target_ip >>  8) & 0xFF;
    p[27] = (target_ip >>  0) & 0xFF;

    return 14 + 28;   /* 42 bytes total */
}

static void dump_frame(const uint8_t *f, int len) {
    printf("frame: dst=%02x:%02x:%02x:%02x:%02x:%02x src=%02x:%02x:%02x:%02x:%02x:%02x type=0x%02x%02x len=%d\n",
           f[0], f[1], f[2], f[3], f[4], f[5],
           f[6], f[7], f[8], f[9], f[10], f[11],
           f[12], f[13], len);
    if (f[12] == 0x08 && f[13] == 0x06 && len >= 42) {
        const uint8_t *p = f + 14;
        int op = (p[6] << 8) | p[7];
        printf("  ARP %s sender=%d.%d.%d.%d target=%d.%d.%d.%d\n",
               op == 1 ? "request" : "reply",
               p[14], p[15], p[16], p[17],
               p[24], p[25], p[26], p[27]);
    }
}

void kmain(uint64_t hartid, uint64_t fdt_addr) {
    uart_init(0);
    uart_puts("\n=== eth-hello (RTL8169 smoke test) ===\n");

    fdt_t fdt;
    if (fdt_init(&fdt, (const void *)(uintptr_t)fdt_addr)) {
        uint32_t off = fdt_find_compatible(&fdt, "ns16550a");
        uint64_t at = 0;
        fdt_node_reg64(&fdt, off, 0, &at, NULL);
        uart_init((uintptr_t)at);

        off = fdt_find_compatible(&fdt, "pci-host-ecam-generic");
        if (off != UINT32_MAX) {
            fdt_node_reg64(&fdt, off, 0, &at, NULL);
            pci_init((uintptr_t)at);
        }
    }

    if (!eth_init(&nic)) {
        printf("eth_init failed — was RVVM started without -nonet?\n");
        for (;;) __asm__ volatile ("wfi");
    }

    uint8_t mac[6];
    eth_mac(&nic, mac);
    printf("link: %s\n", eth_link_up(&nic) ? "UP" : "down");
    printf("MAC:  %02x:%02x:%02x:%02x:%02x:%02x\n",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    /* RVVM's user-mode tap defaults: 10.0.2.0/24, gateway 10.0.2.2,
     * guest 10.0.2.15. Send ARP for the gateway. */
    uint8_t arp[64];
    int n = build_arp_request(arp, mac,
                              (10u<<24) | (0u<<16) | (2u<<8) | 15u,
                              (10u<<24) | (0u<<16) | (2u<<8) |  2u);
    printf("\nsending ARP request for 10.0.2.2 (%d bytes)...\n", n);
    if (!eth_send(&nic, arp, (uint32_t)n)) {
        printf("eth_send failed\n");
    }

    /* Drain RX for 5 seconds, print every frame. */
    uint64_t deadline = time_now() + 5 * RVVM_TIME_HZ;
    int seen = 0;
    while (time_now() < deadline) {
        uint8_t buf[1600];
        int got = eth_recv(&nic, buf, sizeof(buf));
        if (got > 0) {
            seen++;
            dump_frame(buf, got);
        }
    }
    printf("\nseen %d frames total. done.\n", seen);
    for (;;) __asm__ volatile ("wfi");
}
