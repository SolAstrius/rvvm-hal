/* net-hello — lwIP smoke test: DHCP client + UDP echo server.
 *
 *   make
 *   make run
 *
 * RVVM's user-mode tap_user runs a slirp-style DHCP server on
 * 10.0.2.2 that hands out 10.0.2.15 to the first DHCP client. We
 * watch the netif state until it gets an IP, print a small status,
 * then start a UDP echo server on port 7 and loop forever.
 *
 * Talking to it from the host requires RVVM's portfwd flag — pass
 * `-portfwd udp/host_port=guest_port` to map a local UDP port to
 * port 7 inside the guest:
 *
 *     rvvm firmware.bin -portfwd udp/2007=7 -nogui -nosound
 *     # then on the host:
 *     echo "hello" | nc -u -w1 127.0.0.1 2007
 *     # → guest UART logs the received bytes and echoes back
 *
 * Without portfwd, you'll still see the DHCP exchange complete
 * and the printed IP — that alone proves the L3+ stack works. */

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
#include "net.h"

#include "lwip/netif.h"
#include "lwip/dhcp.h"
#include "lwip/udp.h"
#include "lwip/ip4_addr.h"

static eth_t   nic;

static void udp_echo_recv(void *arg, struct udp_pcb *pcb,
                          struct pbuf *p,
                          const ip_addr_t *addr, u16_t port) {
    (void)arg;
    if (!p) return;
    printf("udp/7 ← %u.%u.%u.%u:%u (%u bytes)\n",
           ip4_addr1(ip_2_ip4(addr)), ip4_addr2(ip_2_ip4(addr)),
           ip4_addr3(ip_2_ip4(addr)), ip4_addr4(ip_2_ip4(addr)),
           port, p->tot_len);
    /* Echo the same payload back to the sender. */
    udp_sendto(pcb, p, addr, port);
    pbuf_free(p);
}

void kmain(uint64_t hartid, uint64_t fdt_addr) {
    uart_init(0);
    uart_puts("\n=== net-hello (lwIP DHCP + UDP echo) ===\n");

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
        printf("eth_init failed — is RVVM started without -nonet?\n");
        for (;;) __asm__ volatile ("wfi");
    }
    uint8_t mac[6]; eth_mac(&nic, mac);
    printf("eth: link=%s MAC=%02x:%02x:%02x:%02x:%02x:%02x\n",
           eth_link_up(&nic) ? "UP" : "down",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    if (!net_init(&nic)) {
        printf("net_init failed\n");
        for (;;) __asm__ volatile ("wfi");
    }
    printf("lwip: initialised, starting DHCP...\n");
    dhcp_start(net_default_netif());

    /* Poll until DHCP completes (or 10 seconds elapse). With
     * LWIP_IPV6=0, netif->ip_addr/netmask/gw are plain ip4_addr_t. */
    uint64_t deadline = time_now() + 10 * RVVM_TIME_HZ;
    bool got_ip = false;
    while (time_now() < deadline) {
        net_poll();
        if (!ip4_addr_isany_val(net_default_netif()->ip_addr)) {
            got_ip = true;
            break;
        }
    }
    if (!got_ip) {
        printf("DHCP timed out — check tap is enabled\n");
        for (;;) __asm__ volatile ("wfi");
    }

    const ip4_addr_t *ip = &net_default_netif()->ip_addr;
    const ip4_addr_t *nm = &net_default_netif()->netmask;
    const ip4_addr_t *gw = &net_default_netif()->gw;
    printf("dhcp: ip=%u.%u.%u.%u/%u.%u.%u.%u gw=%u.%u.%u.%u\n",
           ip4_addr1(ip), ip4_addr2(ip), ip4_addr3(ip), ip4_addr4(ip),
           ip4_addr1(nm), ip4_addr2(nm), ip4_addr3(nm), ip4_addr4(nm),
           ip4_addr1(gw), ip4_addr2(gw), ip4_addr3(gw), ip4_addr4(gw));

    /* UDP echo on port 7. */
    struct udp_pcb *echo = udp_new();
    if (!echo) { printf("udp_new failed\n"); for (;;) __asm__ volatile ("wfi"); }
    if (udp_bind(echo, IP_ANY_TYPE, 7) != ERR_OK) {
        printf("udp_bind(7) failed\n");
        for (;;) __asm__ volatile ("wfi");
    }
    udp_recv(echo, udp_echo_recv, NULL);
    printf("udp: echo server bound on 0.0.0.0:7\n");
    printf("(host-side test: rvvm ... -portfwd udp/2007=7  then  echo hi | nc -u -w1 127.0.0.1 2007)\n\n");

    /* Service forever. */
    for (;;) {
        net_poll();
    }
}
