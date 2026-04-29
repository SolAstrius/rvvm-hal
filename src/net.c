/* net.c — lwIP integration glue.
 *
 * Pieces:
 *   §1  sys_now()            ms-since-boot for lwIP's timeout module
 *   §2  netif glue           lwIP→eth: linkoutput; eth→lwIP: net_poll
 *                            shovels RX frames into netif->input
 *   §3  net_init/net_poll    public API in include/net.h
 *
 * NO_SYS=1 mode: no scheduler, no callbacks-from-IRQ. The firmware
 * calls net_poll() in its main loop. lwIP's timer wheel runs from
 * sys_check_timeouts() which net_poll invokes. */

#ifdef HAL_LWIP

#include <string.h>
#include "net.h"
#include "eth.h"
#include "uart.h"
#include "rvvm.h"

static inline uint64_t mtime_now(void) {
    uint64_t v;
    __asm__ volatile ("rdtime %0" : "=r"(v));
    return v;
}

#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/etharp.h"
#include "lwip/timeouts.h"
#include "lwip/pbuf.h"
#include "netif/ethernet.h"

static eth_t        *bound_nic;
static struct netif  net_if;
static bool          inited;

/* ====================================================================
 * §1. sys_now
 *
 * lwIP needs a millisecond clock that monotonically advances.
 * RVVM's mtime is 10 MHz, so divide by 10 000 to get ms.
 * ==================================================================== */
uint32_t sys_now(void) {
    return (uint32_t)(mtime_now() / (RVVM_TIME_HZ / 1000ULL));
}

/* ====================================================================
 * §2. netif glue
 *
 * linkoutput: lwIP gives us a chain of pbufs (frame split across
 * one or more buffers); we coalesce into a flat staging buffer and
 * call eth_send. RTL8169 supports scatter-gather but it's a small
 * win at the cost of more complex driver code; not worth it yet.
 *
 * netif_init: called once during netif_add. We set hwaddr from the
 * NIC's MAC, set the output handlers, mark up + broadcast-capable.
 *
 * net_poll: drains every available RX frame, hands each to lwIP via
 * netif->input. Then runs lwIP's timer wheel.
 * ==================================================================== */

static err_t linkoutput(struct netif *netif, struct pbuf *p) {
    static uint8_t staging[ETH_BUFFER_SIZE];
    if (p->tot_len > sizeof(staging)) {
        return ERR_IF;
    }
    /* pbuf_copy_partial walks the pbuf chain and copies up to N
     * bytes into a flat buffer; returns the bytes actually copied. */
    u16_t copied = pbuf_copy_partial(p, staging, p->tot_len, 0);
    if (copied != p->tot_len) {
        return ERR_BUF;
    }
    if (!eth_send(bound_nic, staging, copied)) {
        return ERR_IF;
    }
    (void)netif;
    return ERR_OK;
}

static err_t netif_init_cb(struct netif *netif) {
    netif->name[0] = 'e';
    netif->name[1] = 'n';
    netif->mtu     = 1500;
    netif->flags   = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP;
    netif->hwaddr_len = 6;
    eth_mac(bound_nic, netif->hwaddr);
    netif->output     = etharp_output;     /* IP → ARP → linkoutput */
    netif->linkoutput = linkoutput;
    return ERR_OK;
}

void net_poll(void) {
    if (!inited) return;

    /* Drain every available RX frame. Each becomes a pbuf and is
     * handed to lwIP via netif->input (which routes Ethernet → IP
     * → TCP/UDP/etc by examining the ethertype). */
    for (;;) {
        uint8_t buf[ETH_BUFFER_SIZE];
        int n = eth_recv(bound_nic, buf, sizeof(buf));
        if (n <= 0) break;

        struct pbuf *p = pbuf_alloc(PBUF_RAW, (u16_t)n, PBUF_POOL);
        if (!p) {
            /* Out of pbufs — drop. lwIP will resend / re-ARP later. */
            continue;
        }
        if (pbuf_take(p, buf, (u16_t)n) != ERR_OK) {
            pbuf_free(p);
            continue;
        }
        if (net_if.input(p, &net_if) != ERR_OK) {
            pbuf_free(p);
        }
    }

    /* Run lwIP's timer wheel — DHCP retries, TCP retransmits, ARP
     * cache aging, etc. Cheap when no timer has expired. */
    sys_check_timeouts();
}

/* ====================================================================
 * §3. Public API
 * ==================================================================== */

bool net_init(eth_t *nic) {
    if (!nic || !nic->up) {
        uart_puts("net_init: eth_t isn't initialised or link is down\n");
        return false;
    }
    if (inited) return true;

    bound_nic = nic;
    lwip_init();

    /* Add netif with no IP — DHCP will fill in once the firmware
     * calls dhcp_start(net_default_netif()) explicitly. */
    ip4_addr_t any = { 0 };
    if (!netif_add(&net_if, &any, &any, &any, NULL,
                   netif_init_cb, ethernet_input)) {
        uart_puts("net_init: netif_add failed\n");
        return false;
    }
    netif_set_default(&net_if);
    netif_set_up(&net_if);
    netif_set_link_up(&net_if);

    inited = true;
    return true;
}

struct netif *net_default_netif(void) {
    return inited ? &net_if : NULL;
}

#endif /* HAL_LWIP */
