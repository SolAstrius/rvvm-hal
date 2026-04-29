/* RTL8169 driver — implements include/eth.h.
 *
 * Targets RVVM's RTL8169 emulation (src/devices/rtl8169.c, vendor
 * 0x10EC device 0x8168). RVVM auto-attaches a user-mode tap_user
 * (slirp-style NAT) when started without -nonet, so packets reach
 * the host network without privileges.
 *
 * Spec note: this is NOT a full Linux r8169 driver. We skip the
 * EEPROM dance, the 9346 lock, the ERI registers, the rtl8168b
 * special cases, the offloads, and most of the PHY work — RVVM
 * doesn't model any of that, and we don't need it for raw frame
 * TX/RX. Real hardware would need the full sequence.
 *
 * Layout:
 *   §1  Register definitions and descriptor format
 *   §2  Probe + reset + MAC read + ring setup (eth_init)
 *   §3  Frame send (eth_send)
 *   §4  Frame receive (eth_recv)
 *   §5  Link status (eth_link_up) */

#include "eth.h"
#include "pci.h"
#include "rvvm.h"
#include "mmio.h"
#include "uart.h"

/* Forward-decl mem* — HAL's src/string.c provides them. Avoiding
 * <string.h> means this driver builds without HAL_PICOLIBC. */
extern void *memcpy(void *, const void *, unsigned long);
extern void *memset(void *, int, unsigned long);

/* ====================================================================
 * §1. Register and descriptor definitions
 *
 * Subset of what RVVM implements; offsets from the RTL8169 spec
 * confirmed against RVVM src/devices/rtl8169.c lines 28-64.
 * ==================================================================== */

#define REG_IDR0      0x00   /* MAC address bytes 0-3 */
#define REG_IDR4      0x04   /* MAC address bytes 4-5 (low 16 bits) */
#define REG_TXDA1     0x20   /* TX normal-priority descriptor base, low 32 */
#define REG_TXDA2     0x24   /* high 32 */
#define REG_CR        0x37   /* Command Register: RST | RE | TE */
#define REG_TPOLL     0x38   /* TX Priority Polling */
#define REG_IMR       0x3C   /* Interrupt Mask */
#define REG_ISR       0x3E   /* Interrupt Status (W1C) */
#define REG_TCR       0x40   /* TX Configuration */
#define REG_RCR       0x44   /* RX Configuration */
#define REG_9346      0x50   /* config-write lock */
#define REG_PHYS      0x6C   /* PHY status (link/speed) */
#define REG_RMS       0xDA   /* RX Max Size */
#define REG_CPCR      0xE0   /* C+ Command */
#define REG_RXDA1     0xE4   /* RX descriptor base, low 32 */
#define REG_RXDA2     0xE8

/* CR bits */
#define CR_TE         0x04
#define CR_RE         0x08
#define CR_RST        0x10

/* 9346 lock — write 0xC0 to unlock config writes, 0x00 to lock. */
#define LOCK_UNLOCK   0xC0
#define LOCK_LOCK     0x00

/* TPPoll: kicking TX queues. Set bit 6 (NPQ) for normal-priority. */
#define TPOLL_NPQ     0x40

/* TCR — defaults RVVM accepts. */
#define TCR_IFG       0x03000000   /* 96 ns inter-frame gap */
#define TCR_MXDMA     0x00000700   /* unlimited DMA burst */
#define TCR_DEFAULT   (TCR_IFG | TCR_MXDMA)

/* RCR bits */
#define RCR_AAP       0x0001   /* Accept All Packets */
#define RCR_APM       0x0002   /* Accept Physical Match (our MAC) */
#define RCR_AM        0x0004   /* Accept Multicast */
#define RCR_AB        0x0008   /* Accept Broadcast */
#define RCR_DEFAULT   (RCR_APM | RCR_AB | RCR_AM)
#define RCR_MXDMA     0x00000700
#define RCR_RXFTH     0x00007000   /* RX FIFO threshold = no threshold */

/* C+ Command — must enable for descriptor-mode RX/TX. */
#define CPCR_RXCHKSUM 0x0020
#define CPCR_TXENB    0x0001       /* TX C+ command enable */
#define CPCR_RXENB    0x0002       /* RX C+ command enable */

/* PHY Status bits */
#define PHYS_LINKSTS  0x02

/* Descriptor flags (16 bytes per descriptor: u32 flags+len, u32
 * vlan, u64 buffer-phys-addr). Common to TX and RX. */
#define DESC_OWN      0x80000000U   /* device owns this descriptor */
#define DESC_EOR      0x40000000U   /* end of ring (wrap) */
#define DESC_FS       0x20000000U   /* first segment */
#define DESC_LS       0x10000000U   /* last segment */
#define DESC_LEN_MASK 0x00003FFFU   /* low 14 bits = length */

/* In-memory descriptor layout. Little-endian on the wire (RV is
 * also LE so direct mapping works). */
typedef struct __attribute__((packed,aligned(16))) {
    uint32_t flags_len;
    uint32_t vlan;          /* unused — leave zero */
    uint64_t buf_addr;
} desc_t;

/* ====================================================================
 * §2. Probe + reset + ring setup
 * ==================================================================== */

static inline uint8_t  R8 (eth_t *e, uint32_t off)            { return mmio_r8 (e->bar + off); }
static inline uint16_t R16(eth_t *e, uint32_t off)            { return mmio_r16(e->bar + off); }
static inline uint32_t R32(eth_t *e, uint32_t off)            { return mmio_r32(e->bar + off); }
static inline void     W8 (eth_t *e, uint32_t off, uint8_t  v){ mmio_w8 (e->bar + off, v); }
static inline void     W16(eth_t *e, uint32_t off, uint16_t v){ mmio_w16(e->bar + off, v); }
static inline void     W32(eth_t *e, uint32_t off, uint32_t v){ mmio_w32(e->bar + off, v); }

bool eth_init(eth_t *e) {
    /* Zero the whole struct so descriptor rings start clean (their
     * OWN bits will be set explicitly below). */
    memset(e, 0, sizeof(*e));

    if (!pci_find_device(RVVM_PCI_ID_RTL8168, &e->pf)) {
        uart_puts("eth: no RTL8169 found — RVVM started with -nonet?\n");
        return false;
    }
    pci_setup_bars(&e->pf);

    /* RVVM places the RTL8169's MMIO in BAR2 (the IO BAR) and BAR0
     * is the I/O space window. Some real RTL8169 silicon flips this.
     * We pick the first non-zero memory BAR. */
    for (int i = 0; i < 6; i++) {
        if (e->pf.bar[i] != 0) {
            e->bar = e->pf.bar[i];
            break;
        }
    }
    if (!e->bar) {
        uart_puts("eth: no usable BAR on RTL8169\n");
        return false;
    }

    /* Reset. Set RST bit, wait for it to self-clear (synchronous in
     * RVVM but spin a finite count for safety). */
    W8(e, REG_CR, CR_RST);
    for (int i = 0; i < 1000; i++) {
        if (!(R8(e, REG_CR) & CR_RST)) break;
    }
    if (R8(e, REG_CR) & CR_RST) {
        uart_puts("eth: RTL8169 reset did not complete\n");
        return false;
    }

    /* Read MAC from IDR0..IDR5. The first four bytes live at IDR0
     * (32-bit reg), bytes 4-5 at IDR4 (16-bit accessible). */
    uint32_t idr0 = R32(e, REG_IDR0);
    uint32_t idr4 = R32(e, REG_IDR4);
    e->mac[0] = (uint8_t)(idr0 >>  0);
    e->mac[1] = (uint8_t)(idr0 >>  8);
    e->mac[2] = (uint8_t)(idr0 >> 16);
    e->mac[3] = (uint8_t)(idr0 >> 24);
    e->mac[4] = (uint8_t)(idr4 >>  0);
    e->mac[5] = (uint8_t)(idr4 >>  8);

    /* Build RX ring: every descriptor points at its buffer, OWN=1
     * (device may write into it). The last descriptor sets EOR so
     * the device wraps back to entry 0 instead of running off the
     * end. */
    desc_t *rx = (desc_t *)e->rx_ring;
    for (int i = 0; i < ETH_RING_DEPTH; i++) {
        uint32_t flags = DESC_OWN | (uint32_t)ETH_BUFFER_SIZE;
        if (i == ETH_RING_DEPTH - 1) flags |= DESC_EOR;
        rx[i].flags_len = flags;
        rx[i].vlan      = 0;
        rx[i].buf_addr  = (uint64_t)(uintptr_t)&e->rx_bufs[i][0];
    }

    /* Build TX ring: all descriptors zero (owned by software).
     * EOR on the last so when we wrap, the device knows. */
    desc_t *tx = (desc_t *)e->tx_ring;
    for (int i = 0; i < ETH_RING_DEPTH; i++) {
        tx[i].flags_len = (i == ETH_RING_DEPTH - 1) ? DESC_EOR : 0;
        tx[i].vlan      = 0;
        tx[i].buf_addr  = (uint64_t)(uintptr_t)&e->tx_bufs[i][0];
    }

    mmio_barrier();   /* descriptors flushed before device sees them */

    /* Unlock config registers. */
    W8(e, REG_9346, LOCK_UNLOCK);

    /* Program ring base addresses. Both must be 256-byte aligned —
     * the rx_ring/tx_ring fields use __attribute__((aligned(256))). */
    uint64_t rx_phys = (uint64_t)(uintptr_t)e->rx_ring;
    uint64_t tx_phys = (uint64_t)(uintptr_t)e->tx_ring;
    W32(e, REG_RXDA1, (uint32_t)(rx_phys & 0xFFFFFFFF));
    W32(e, REG_RXDA2, (uint32_t)(rx_phys >> 32));
    W32(e, REG_TXDA1, (uint32_t)(tx_phys & 0xFFFFFFFF));
    W32(e, REG_TXDA2, (uint32_t)(tx_phys >> 32));

    /* RX max size = our buffer (RVVM caps to 0x3FFF anyway). */
    W16(e, REG_RMS, (uint16_t)ETH_BUFFER_SIZE);

    /* Configure TCR/RCR. */
    W32(e, REG_TCR, TCR_DEFAULT);
    W32(e, REG_RCR, RCR_DEFAULT | RCR_MXDMA | RCR_RXFTH);

    /* C+ Command — descriptor-mode RX/TX enable. RVVM's emulation
     * keys off this. */
    W16(e, REG_CPCR, CPCR_TXENB | CPCR_RXENB);

    /* Mask all interrupts (we poll). The W1C ISR is left alone —
     * future IRQ-driven path will swap CPCR + IMR config. */
    W16(e, REG_IMR, 0);
    W16(e, REG_ISR, 0xFFFF);   /* clear any pending */

    /* Lock config back. */
    W8(e, REG_9346, LOCK_LOCK);

    /* Final: enable RX/TX engines. */
    W8(e, REG_CR, CR_RE | CR_TE);

    e->up = true;
    return true;
}

void eth_mac(const eth_t *e, uint8_t out[6]) {
    for (int i = 0; i < 6; i++) out[i] = e->mac[i];
}

bool eth_link_up(const eth_t *e) {
    if (!e->up) return false;
    return (mmio_r8(e->bar + REG_PHYS) & PHYS_LINKSTS) != 0;
}

/* ====================================================================
 * §3. Frame send
 *
 * Single-segment frames only — every frame fits in one descriptor.
 * (Hardware can do multi-segment scatter-gather; we don't need it
 * for sub-2KB ethernet.)
 * ==================================================================== */

bool eth_send(eth_t *e, const void *frame, uint32_t len) {
    if (!e->up || len == 0 || len > ETH_BUFFER_SIZE) return false;

    desc_t *tx = (desc_t *)e->tx_ring;
    uint32_t idx = e->tx_head;
    uint32_t flags = __atomic_load_n(&tx[idx].flags_len, __ATOMIC_ACQUIRE);

    /* If the device still owns this slot, the ring is full. */
    if (flags & DESC_OWN) {
        return false;
    }

    /* Copy payload, set descriptor (preserving EOR if it's the last
     * slot), hand to device with OWN+FS+LS for a single-segment
     * frame, kick TPPoll. */
    memcpy(&e->tx_bufs[idx][0], frame, len);

    uint32_t new_flags = DESC_OWN | DESC_FS | DESC_LS | (len & DESC_LEN_MASK);
    if (idx == ETH_RING_DEPTH - 1) new_flags |= DESC_EOR;

    tx[idx].flags_len = new_flags;
    mmio_barrier();

    /* Kick. RVVM samples TPOLL.NPQ on every write and immediately
     * dispatches all OWN-bit-set TX descriptors. */
    W8(e, REG_TPOLL, TPOLL_NPQ);

    e->tx_head = (idx + 1) % ETH_RING_DEPTH;
    return true;
}

/* ====================================================================
 * §4. Frame receive
 *
 * Walk the RX ring forward looking for a descriptor whose OWN bit
 * has been cleared (device has written into the buffer). Copy the
 * payload out, re-arm the descriptor, advance.
 * ==================================================================== */

int eth_recv(eth_t *e, void *out, uint32_t maxlen) {
    if (!e->up) return 0;

    desc_t *rx = (desc_t *)e->rx_ring;
    uint32_t idx = e->rx_head;
    uint32_t flags = __atomic_load_n(&rx[idx].flags_len, __ATOMIC_ACQUIRE);

    if (flags & DESC_OWN) {
        /* Still owned by device — no frame ready. */
        return 0;
    }

    /* Got one. Length is in the low 14 bits of flags_len, including
     * the trailing 4-byte FCS the device appends. We drop the FCS
     * (lwIP and friends don't expect it). */
    uint32_t len = flags & DESC_LEN_MASK;
    if (len < 4) len = 0; else len -= 4;

    if (len > maxlen) len = maxlen;
    if (len > 0) {
        memcpy(out, &e->rx_bufs[idx][0], len);
    }

    /* Re-arm: hand the descriptor back to the device. Preserve EOR
     * for the last slot. */
    uint32_t new_flags = DESC_OWN | (uint32_t)ETH_BUFFER_SIZE;
    if (idx == ETH_RING_DEPTH - 1) new_flags |= DESC_EOR;
    rx[idx].flags_len = new_flags;
    mmio_barrier();

    e->rx_head = (idx + 1) % ETH_RING_DEPTH;
    return (int)len;
}
