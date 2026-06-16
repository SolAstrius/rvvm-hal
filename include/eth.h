/* Ethernet — Realtek RTL8169 driver for RVVM's emulated NIC.
 *
 * RVVM ships an RTL8169 (vendor 10EC, device 8168) by default; pass
 * -nonet to disable. The host side runs RVVM's user-mode TAP (slirp-
 * style: NAT, DHCP server, DNS forwarder), so guest packets reach the
 * host network without root or any host configuration.
 *
 * This driver gives raw L2 (ethernet frame) send/receive. lwIP or any
 * other TCP/IP stack sits on top — eth_send takes a fully-formed
 * ethernet frame (dst MAC + src MAC + ethertype + payload) and
 * eth_recv returns the same shape.
 *
 * Two RX models: poll eth_recv() in a loop, or call eth_irq_attach()
 * to have frames delivered through the PLIC (the NIC's PCI INTx line,
 * source from config 0x3C). The descriptor format is identical either
 * way; the IRQ handler just calls eth_recv() internally.
 *
 * Typical usage:
 *
 *     static eth_t nic;
 *     eth_init(&nic);
 *     uint8_t mac[6]; eth_mac(&nic, mac);
 *     printf("MAC: %02x:%02x:%02x:%02x:%02x:%02x\n", ...);
 *
 *     uint8_t frame[1518];
 *     int n = eth_recv(&nic, frame, sizeof(frame));
 *     if (n > 0) { ... process ... }
 *
 *     eth_send(&nic, frame, len);  */

#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "pci.h"

/* RX/TX ring depth. 16 each gives a comfortable 32 KiB total for
 * both rings + per-descriptor 2 KiB buffers. RVVM's tap_user can
 * burst ~10 frames at a time during DHCP / ARP exchanges, so 16
 * leaves slack. Bump if higher throughput needed. */
#define ETH_RING_DEPTH      16
#define ETH_BUFFER_SIZE     2048   /* >= 1518 (max ethernet) + slack */

/* Driver state. Caller declares as `static eth_t e;` — embeds the
 * rings and buffers directly so there's no heap dependency. */
typedef struct eth_s {
    pci_func_t pf;
    uintptr_t  bar;           /* BAR0 or BAR2 — RVVM places at 0x40010000-ish */
    uint8_t    mac[6];

    /* RX ring + buffers, page-aligned. Allocated as static arrays
     * inside the struct so a freestanding firmware doesn't need
     * malloc just for ethernet. */
    uint8_t    rx_ring[ETH_RING_DEPTH * 16] __attribute__((aligned(256)));
    uint8_t    rx_bufs[ETH_RING_DEPTH][ETH_BUFFER_SIZE]
                   __attribute__((aligned(8)));
    uint32_t   rx_head;       /* next descriptor to consume */

    uint8_t    tx_ring[ETH_RING_DEPTH * 16] __attribute__((aligned(256)));
    uint8_t    tx_bufs[ETH_RING_DEPTH][ETH_BUFFER_SIZE]
                   __attribute__((aligned(8)));
    uint32_t   tx_head;       /* next descriptor to fill */
    uint32_t   tx_inflight;   /* descriptors with OWN bit set */

    bool       up;
} eth_t;

/* Probe + reset + ring setup + RX/TX enable. Returns false if no
 * RTL8169 found (RVVM started with -nonet) or if BAR mapping
 * failed. After this, `e->mac` is valid. */
bool eth_init(eth_t *e);

/* Copy the 6-byte MAC out. Same bytes RVVM auto-generated when it
 * created the tap, so two RVVM runs of the same firmware produce
 * different MACs (unless `-mac=...` is explicitly passed). */
void eth_mac(const eth_t *e, uint8_t out[6]);

/* Send one frame. `len` includes the ethernet header (14 bytes:
 * dst MAC + src MAC + ethertype) but NOT the FCS — the device
 * appends that automatically. Returns false if the TX ring is
 * full; caller may retry. */
bool eth_send(eth_t *e, const void *frame, uint32_t len);

/* Try to receive one frame. Returns the number of bytes copied
 * into `out` (excluding FCS), or 0 if no frame is ready.
 * Non-blocking. Up to `maxlen` bytes are written; longer frames
 * are silently truncated (extremely rare with maxlen >= 1518). */
int  eth_recv(eth_t *e, void *out, uint32_t maxlen);

/* PHY reports link status. RVVM's tap is always "up", so this is
 * mostly for shape — real hardware could lose link. */
bool eth_link_up(const eth_t *e);

/* Callback for IRQ-delivered frames. `frame`/`len` (FCS already
 * stripped, same as eth_recv) are valid only for the duration of the
 * call — the buffer is driver-owned and reused, so copy anything you
 * need to keep. Runs in interrupt context: keep it short. */
typedef void (*eth_rx_cb_t)(const void *frame, uint32_t len, void *ctx);

/* Switch RX to interrupt-driven delivery. Discovers the NIC's PLIC
 * source from its PCI Interrupt Line (config 0x3C, auto-filled by RVVM),
 * unmasks the RX interrupt bits, registers a PLIC handler that clears
 * the device status and drains every ready frame into `cb`, and enables
 * the source. The caller owns irq_init() (called first) and
 * irq_global_enable() (called once every source is wired). Returns the
 * PLIC source, or 0 if the NIC has no interrupt line assigned. Only one
 * NIC may be attached at a time; eth_send()/eth_recv() remain valid. */
uint32_t eth_irq_attach(eth_t *e, eth_rx_cb_t cb, void *ctx);
