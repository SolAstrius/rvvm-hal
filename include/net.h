/* Network — lwIP TCP/IP stack on top of the RTL8169 driver.
 *
 * Bridges include/eth.h (raw L2 frames) into lwIP. Exposes lwIP's
 * raw API: callers that want sockets-style code use the lwip/api.h
 * header from the vendored tree; callers that want the leanest path
 * use the raw tcp.h / udp.h / icmp.h on the same netif we set up.
 *
 * Compiled only when HAL_LWIP=1 (Makefile gate). The .o is otherwise
 * excluded from libhal.a so default builds stay lwIP-free.
 *
 * Typical workflow:
 *
 *     #include "net.h"
 *     #include "lwip/dhcp.h"
 *     #include "lwip/udp.h"
 *
 *     net_init(&nic);                  // bring up netif on existing eth_t
 *     dhcp_start(net_default_netif()); // ask RVVM tap_user for an IP
 *
 *     for (;;) {
 *         net_poll();                   // drives lwIP timers + RX
 *         // ... your loop work ...
 *     }
 *
 * The single netif gets a stable name "en0" — lwIP allows up to
 * 256 simultaneously but one is plenty for our use case. */

#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef HAL_LWIP

#include "eth.h"

/* Forward-decl from lwIP — opaque pointer here to avoid forcing
 * every consumer to pull lwIP's headers. */
struct netif;

/* Initialise lwIP and register a netif backed by `nic`. The eth_t
 * must have been initialised via eth_init() and remain valid for
 * the lifetime of network access. Returns false on failure (lwIP
 * netif_add rejected, or eth not up). */
bool net_init(eth_t *nic);

/* Drive lwIP: process every queued RX frame from the NIC, run any
 * lwIP timers that have elapsed. Call this in the firmware's main
 * loop. With NO_SYS=1 lwIP doesn't run a thread of its own — the
 * firmware is responsible for the timing pulse. */
void net_poll(void);

/* Get the netif we registered (so consumers can do
 * `dhcp_start(net_default_netif())` etc). */
struct netif *net_default_netif(void);

#endif /* HAL_LWIP */
