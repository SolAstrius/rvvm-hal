#include "nvme.h"
#include "pci.h"
#include "rvvm.h"
#include "mmio.h"
#include "uart.h"

/* RVVM emulates a Nextorage NE1N as its NVMe target. */
#define NVME_PCI_ID      0x45121F31U   /* device:vendor packed (high:low) */

/* Controller registers (BAR0 offsets). NVMe spec §3.1. */
#define NVME_REG_CAP1    0x00
#define NVME_REG_CAP2    0x04
#define NVME_REG_VS      0x08
#define NVME_REG_INTMS   0x0C
#define NVME_REG_INTMC   0x10
#define NVME_REG_CC      0x14
#define NVME_REG_CSTS    0x1C
#define NVME_REG_AQA     0x24
#define NVME_REG_ASQ_LO  0x28
#define NVME_REG_ASQ_HI  0x2C
#define NVME_REG_ACQ_LO  0x30
#define NVME_REG_ACQ_HI  0x34
#define NVME_REG_DOORBELL_BASE 0x1000   /* SQ0 tail, CQ0 head, SQ1 tail, ... */

#define NVME_CC_EN       0x00000001U
#define NVME_CC_IOQES    0x00460000U   /* IOSQES=6 (64 B), IOCQES=4 (16 B) */
#define NVME_CSTS_RDY    0x00000001U

/* SQE field offsets (NVMe §4.2). 64-byte entry. */
#define SQE_CDW0         0x00          /* opcode in low byte */
#define SQE_CID          0x02
#define SQE_NSID         0x04
#define SQE_PRP1         0x18
#define SQE_PRP2         0x20
#define SQE_CDW10        0x28
#define SQE_CDW11        0x2C
#define SQE_CDW12        0x30

/* CQE field offsets (§4.6). 16-byte entry. */
#define CQE_SQHD_SQID    0x08
#define CQE_CID_PB_SF    0x0C          /* CID[15:0], PB[16], SF[31:17] */
#define CQE_PB_MASK      0x00010000U

/* Admin opcodes (§5). */
#define ADM_CREATE_IO_SQ 0x01
#define ADM_CREATE_IO_CQ 0x05
#define ADM_IDENTIFY     0x06

/* I/O opcodes (§6). */
#define IO_FLUSH         0x00
#define IO_WRITE         0x01
#define IO_READ          0x02

/* Identify CNS values. CNS=0 → namespace, CNS=1 → controller. */
#define CNS_NAMESPACE    0x00

/* Create-CQ / Create-SQ flags (§5.3.2 / §5.4.2). */
#define CQ_FLAGS_PC      0x0001U       /* physically contiguous */
#define SQ_FLAGS_PC      0x0001U

/* Hardcoded sole namespace. RVVM exposes one per controller. */
#define NVME_NSID        1

#define ADMIN_QID        0
#define IO_QID           1

/* Queue depth chosen so SQ entries (64 B) fit one page: 4096/64 = 64. */
#define QUEUE_DEPTH      64

static inline uint32_t r32(nvme_t *n, uint32_t off) {
    return mmio_r32(n->bar0 + off);
}
static inline void w32(nvme_t *n, uint32_t off, uint32_t v) {
    mmio_w32(n->bar0 + off, v);
}

/* Doorbell offsets. With CAP.DSTRD = 0 (RVVM hardcodes this), each
 * doorbell is 4 bytes wide and queues are interleaved SQ-tail then
 * CQ-head. So qid N's SQ tail is at +N*8, CQ head at +N*8+4. */
static inline uint32_t db_sq_tail(uint32_t qid) {
    return NVME_REG_DOORBELL_BASE + qid * 8;
}
static inline uint32_t db_cq_head(uint32_t qid) {
    return NVME_REG_DOORBELL_BASE + qid * 8 + 4;
}

/* Wait for CSTS.RDY to match `want` (true = wait for ready, false =
 * wait for not-ready). RVVM toggles the bit synchronously inside the
 * CC write callback, so this rarely spins beyond the first read. */
static bool wait_csts_rdy(nvme_t *n, bool want, int spin) {
    for (int i = 0; i < spin; i++) {
        bool rdy = (r32(n, NVME_REG_CSTS) & NVME_CSTS_RDY) != 0;
        if (rdy == want) return true;
    }
    return false;
}

/* Write a single SQE field (little-endian). Each SQE occupies 64 bytes
 * starting at `sqe_base + slot*64`. The buffer is plain RAM — DMA from
 * the controller fetches it via PCI when we ring the doorbell. */
static void sqe_w8 (uint8_t *sqe, uint32_t off, uint8_t  v) { sqe[off] = v; }
static void sqe_w16(uint8_t *sqe, uint32_t off, uint16_t v) {
    sqe[off+0] = v & 0xFF; sqe[off+1] = (v >> 8) & 0xFF;
}
static void sqe_w32(uint8_t *sqe, uint32_t off, uint32_t v) {
    sqe[off+0] = v & 0xFF;       sqe[off+1] = (v >> 8) & 0xFF;
    sqe[off+2] = (v >> 16) & 0xFF; sqe[off+3] = (v >> 24) & 0xFF;
}
static void sqe_w64(uint8_t *sqe, uint32_t off, uint64_t v) {
    sqe_w32(sqe, off,   (uint32_t)v);
    sqe_w32(sqe, off+4, (uint32_t)(v >> 32));
}
/* CQE reads MUST be volatile. RVVM dispatches I/O commands on worker
 * threads, so the CQE updates asynchronously after our doorbell write.
 * Without volatile the compiler CSE-s the read in submit_and_wait's
 * spin loop, latches the initial phase=0, and we poll forever. */
static uint64_t cqe_r64(const uint8_t *cqe, uint32_t off) {
    const volatile uint8_t *p = cqe + off;
    uint64_t lo = (uint32_t)p[0] | ((uint32_t)p[1] << 8)
                | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    uint64_t hi = (uint32_t)p[4] | ((uint32_t)p[5] << 8)
                | ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24);
    return lo | (hi << 32);
}
static uint32_t cqe_r32(const uint8_t *cqe, uint32_t off) {
    const volatile uint8_t *p = cqe + off;
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Write a 64-bit little-endian value into the PRP list at byte
 * offset `off`. Defined as bytewise so we don't assume CPU endian
 * matches the wire format (which is always LE for NVMe). */
static inline void prp_list_w64(nvme_t *n, uint32_t off, uint64_t v) {
    for (int b = 0; b < 8; b++) {
        n->prp_list[off + b] = (uint8_t)((v >> (b * 8)) & 0xFF);
    }
}

/* Fill in PRP1/PRP2 (and a chained PRP list if needed) for a transfer
 * of `size` bytes starting at `buf`. NVMe rules (§4.1.2):
 *   - PRP1 may be misaligned within a page; subsequent PRPs are page-aligned
 *   - For data covering ≤ 1 page after PRP1's offset, only PRP1 is used
 *   - For data covering ≤ 2 pages, PRP1 + PRP2 (PRP2 = page address)
 *   - Otherwise PRP2 = phys address of a PRP list. A PRP list is one
 *     page (512 × 8-byte entries). For lists larger than one page,
 *     §4.1.2.2 requires the LAST entry of each non-final list page
 *     to be a CHAIN POINTER to the next list page — NOT a data
 *     entry. So chained pages hold 511 data entries each; only the
 *     final page holds up to 512.
 *
 * Caller must guarantee `size` ≤ NVME_MAX_SINGLE_TRANSFER (32 MiB
 * with NVME_PRP_LIST_PAGES=16). nvme_read/nvme_write split larger
 * transfers across multiple commands.
 *
 * Since we emulate on RVVM with identity-mapped DMA, "phys address" of
 * a buffer is just its pointer cast to uint64. */
#define ENTRIES_PER_LIST_PAGE   (NVME_PAGE_SIZE / 8)   /* 512 */

static void setup_prp(nvme_t *n, uint8_t *sqe, const void *buf, uint32_t size) {
    uint64_t addr = (uint64_t)(uintptr_t)buf;
    uint32_t page_off = addr & (NVME_PAGE_SIZE - 1);
    uint32_t first_chunk = NVME_PAGE_SIZE - page_off;

    sqe_w64(sqe, SQE_PRP1, addr);

    if (size <= first_chunk) {
        sqe_w64(sqe, SQE_PRP2, 0);
        return;
    }
    if (size <= first_chunk + NVME_PAGE_SIZE) {
        /* Exactly one extra page: PRP2 = address of that page. */
        sqe_w64(sqe, SQE_PRP2, (addr + first_chunk) & ~(uint64_t)(NVME_PAGE_SIZE - 1));
        return;
    }

    /* PRP list path. */
    uint64_t list_base = (addr + first_chunk) & ~(uint64_t)(NVME_PAGE_SIZE - 1);
    uint32_t remaining = size - first_chunk;
    uint32_t entries_needed = (remaining + NVME_PAGE_SIZE - 1) / NVME_PAGE_SIZE;

    /* Walk entries; emit chain pointer at the last slot of each
     * non-final list page. `cur_page` and `slot` track our write
     * position in n->prp_list. `i` is the data-entry index — does
     * NOT advance when we emit a chain pointer. */
    uint32_t cur_page = 0;
    uint32_t slot     = 0;
    uint32_t i        = 0;
    while (i < entries_needed) {
        uint32_t off = cur_page * NVME_PAGE_SIZE + slot * 8;

        bool last_slot       = (slot == ENTRIES_PER_LIST_PAGE - 1);
        bool more_after_this = (i + 1 < entries_needed);
        if (last_slot && more_after_this) {
            /* Chain to next list page; do NOT consume `i`. */
            uint64_t next = (uint64_t)(uintptr_t)
                            &n->prp_list[(cur_page + 1) * NVME_PAGE_SIZE];
            prp_list_w64(n, off, next);
            cur_page++;
            slot = 0;
            continue;
        }

        prp_list_w64(n, off, list_base + (uint64_t)i * NVME_PAGE_SIZE);
        slot++;
        i++;
    }

    sqe_w64(sqe, SQE_PRP2, (uint64_t)(uintptr_t)n->prp_list);
}

/* Submit one prepared SQE on a queue and wait for its CQE. Returns the
 * status field (0 = success, non-zero = error). */
static uint32_t submit_and_wait(nvme_t *n, uint32_t qid,
                                uint8_t *sq_buf, uint8_t *cq_buf,
                                uint16_t *sq_tail, uint16_t *cq_head,
                                bool *expected_phase, uint16_t depth)
{
    /* Ring the SQ doorbell. The SQE's CID is at SQE_CID — caller must
     * have already filled the SQE in slot *sq_tail. */
    *sq_tail = (*sq_tail + 1) % depth;
    mmio_barrier();   /* SQE writes complete before doorbell */
    w32(n, db_sq_tail(qid), *sq_tail);

    /* Poll the CQE phase bit at slot *cq_head. The 1M cap was
     * tight for large I/O reads (a 32 MiB read with PRP chaining
     * makes RVVM's NVMe model walk thousands of PRP entries DMA-
     * ing each page). Bump to 16M — still bounded for stuck-host
     * cases, and ~16× slack for big transfers. */
    for (int spin = 0; spin < 16 * 1000 * 1000; spin++) {
        const uint8_t *cqe = cq_buf + (*cq_head) * 16;
        uint32_t cid_pb_sf = cqe_r32(cqe, CQE_CID_PB_SF);
        bool phase = (cid_pb_sf & CQE_PB_MASK) != 0;
        if (phase == *expected_phase) {
            uint32_t status = cid_pb_sf >> 17;
            *cq_head = (*cq_head + 1) % depth;
            if (*cq_head == 0) *expected_phase = !*expected_phase;
            mmio_barrier();
            w32(n, db_cq_head(qid), *cq_head);
            return status;
        }
    }
    return 0xFFFF;   /* timeout */
}

static uint32_t admin_cmd(nvme_t *n, uint8_t opcode, uint32_t nsid,
                          const void *prp_buf, uint32_t prp_size,
                          uint32_t cdw10, uint32_t cdw11)
{
    uint8_t *sqe = n->asq_buf + n->asq_tail * 64;
    /* Zero the slot — leftover bytes from a previous command in the
     * same slot would otherwise be interpreted as command parameters. */
    for (int i = 0; i < 64; i++) sqe[i] = 0;
    sqe_w8 (sqe, SQE_CDW0,  opcode);
    sqe_w16(sqe, SQE_CID,   n->cmd_id++);
    sqe_w32(sqe, SQE_NSID,  nsid);
    /* PRP1 always carries `prp_buf` for admin commands — for Identify
     * et al. it's the data-buffer address; for Create-IO-{C,S}Q it's
     * the queue-memory address (size=0 since no per-byte transfer). */
    if (prp_buf) setup_prp(n, sqe, prp_buf, prp_size);
    sqe_w32(sqe, SQE_CDW10, cdw10);
    sqe_w32(sqe, SQE_CDW11, cdw11);
    return submit_and_wait(n, ADMIN_QID, n->asq_buf, n->acq_buf,
                           &n->asq_tail, &n->acq_head, &n->acq_phase,
                           n->aq_entries);
}

static uint32_t io_cmd(nvme_t *n, uint8_t opcode, uint64_t lba,
                       const void *buf, uint32_t size, uint32_t nlb)
{
    uint8_t *sqe = n->iosq_buf + n->iosq_tail * 64;
    for (int i = 0; i < 64; i++) sqe[i] = 0;
    sqe_w8 (sqe, SQE_CDW0,  opcode);
    sqe_w16(sqe, SQE_CID,   n->cmd_id++);
    sqe_w32(sqe, SQE_NSID,  NVME_NSID);
    if (size) setup_prp(n, sqe, buf, size);
    sqe_w32(sqe, SQE_CDW10, (uint32_t)lba);
    sqe_w32(sqe, SQE_CDW11, (uint32_t)(lba >> 32));
    /* CDW12: low 16 bits = NLB - 1 (0-based). */
    sqe_w32(sqe, SQE_CDW12, nlb ? (nlb - 1) : 0);
    return submit_and_wait(n, IO_QID, n->iosq_buf, n->iocq_buf,
                           &n->iosq_tail, &n->iocq_head, &n->iocq_phase,
                           n->ioq_entries);
}

bool nvme_init_nth(nvme_t *n, uint32_t idx) {
    /* Zero the whole struct (queue pages included) so we start with a
     * known-blank phase-0 CQE memory. */
    uint8_t *p = (uint8_t *)n;
    for (uint32_t i = 0; i < sizeof(*n); i++) p[i] = 0;

    pci_func_t pf;
    if (!pci_find_device_nth(NVME_PCI_ID, idx, &pf)) return false;
    pci_setup_bars(&pf);
    if (pf.bar[0] == 0) {
        uart_printf("nvme: BAR0 unmapped on %u:%u.%u\n",
                    (uint64_t)pf.bus, (uint64_t)pf.dev, (uint64_t)pf.func);
        return false;
    }
    n->bar0 = pf.bar[0];
    n->aq_entries  = QUEUE_DEPTH;
    n->ioq_entries = QUEUE_DEPTH;
    n->cmd_id      = 1;
    /* The NVMe spec says CQEs are zero-initialised before first use,
     * so the initial phase bit on every slot is 0. The first lap of
     * completions therefore arrives as phase=1 — that's our starting
     * "expected" value. */
    n->acq_phase   = true;
    n->iocq_phase  = true;

    /* 1. Disable controller. */
    w32(n, NVME_REG_CC, 0);
    if (!wait_csts_rdy(n, false, 100000)) {
        uart_puts("nvme: controller stuck ready during reset\n");
        return false;
    }

    /* 2. Program admin queue attributes + base addresses. AQA encodes
     *    the queue depth as N-1 (so 0 means 1 entry, 0xFFF means 4096). */
    uint32_t aqa = (n->aq_entries - 1) | ((uint32_t)(n->aq_entries - 1) << 16);
    w32(n, NVME_REG_AQA,    aqa);
    uint64_t asq = (uint64_t)(uintptr_t)n->asq_buf;
    uint64_t acq = (uint64_t)(uintptr_t)n->acq_buf;
    w32(n, NVME_REG_ASQ_LO, (uint32_t)asq);
    w32(n, NVME_REG_ASQ_HI, (uint32_t)(asq >> 32));
    w32(n, NVME_REG_ACQ_LO, (uint32_t)acq);
    w32(n, NVME_REG_ACQ_HI, (uint32_t)(acq >> 32));

    /* 3. Enable. CC.IOQES bakes in our SQE/CQE entry sizes (64/16). */
    w32(n, NVME_REG_CC, NVME_CC_EN | NVME_CC_IOQES);
    if (!wait_csts_rdy(n, true, 100000)) {
        uart_puts("nvme: controller failed to ready\n");
        return false;
    }

    /* 4. Identify Namespace 1. We only need byte 0..7 (NSZE = total size
     *    in LBAs). The rest of the 4 KiB result page lands in our admin
     *    SQ buffer — fine to overwrite, we'll re-zero each command. */
    static uint8_t ns_id_buf[NVME_PAGE_SIZE] __attribute__((aligned(NVME_PAGE_SIZE)));
    uint32_t st = admin_cmd(n, ADM_IDENTIFY, NVME_NSID,
                            ns_id_buf, NVME_PAGE_SIZE, CNS_NAMESPACE, 0);
    if (st != 0) {
        uart_printf("nvme: Identify Namespace failed, status=%x\n", (uint64_t)st);
        return false;
    }
    n->num_lbas = cqe_r64(ns_id_buf, 0);

    /* 5. Create I/O CQ first (Create-SQ references it). CDW10:
     *      [15:0]  CQID
     *      [31:16] queue size - 1 (0-based)
     *    CDW11: PC=1 (physically contiguous), IEN left 0 (no IRQs).  */
    uint32_t cdw10 = IO_QID | ((uint32_t)(n->ioq_entries - 1) << 16);
    st = admin_cmd(n, ADM_CREATE_IO_CQ, 0, n->iocq_buf, 0, cdw10, CQ_FLAGS_PC);
    if (st != 0) {
        uart_printf("nvme: Create I/O CQ failed, status=%x\n", (uint64_t)st);
        return false;
    }

    /* 6. Create I/O SQ. CDW11: [15:0]=PC, [31:16]=CQID. */
    uint32_t cdw11 = SQ_FLAGS_PC | ((uint32_t)IO_QID << 16);
    st = admin_cmd(n, ADM_CREATE_IO_SQ, 0, n->iosq_buf, 0, cdw10, cdw11);
    if (st != 0) {
        uart_printf("nvme: Create I/O SQ failed, status=%x\n", (uint64_t)st);
        return false;
    }

    n->present = true;
    uart_printf("nvme: %u:%u.%u  bar0=%p  capacity=%u sectors (%u MiB)\n",
                (uint64_t)pf.bus, (uint64_t)pf.dev, (uint64_t)pf.func,
                (void *)n->bar0, n->num_lbas,
                (n->num_lbas * NVME_LBA_SIZE) >> 20);
    return true;
}

bool nvme_init(nvme_t *n) {
    return nvme_init_nth(n, 0);
}

/* Max LBAs we can describe in a single NVMe command using our PRP
 * list capacity. Beyond this, nvme_read / nvme_write loop. */
#define NVME_MAX_LBAS_PER_CMD   (NVME_MAX_SINGLE_TRANSFER / NVME_LBA_SIZE)

uint32_t nvme_read(nvme_t *n, uint64_t lba, void *buf, uint32_t nlb) {
    if (!n->present || nlb == 0) return 0;
    uint32_t done = 0;
    while (done < nlb) {
        uint32_t batch = nlb - done;
        if (batch > NVME_MAX_LBAS_PER_CMD) batch = NVME_MAX_LBAS_PER_CMD;
        uint32_t st = io_cmd(n, IO_READ, lba + done,
                             (uint8_t *)buf + (uint64_t)done * NVME_LBA_SIZE,
                             batch * NVME_LBA_SIZE, batch);
        if (st != 0) {
            uart_printf("nvme: read failed at lba=%u nlb=%u status=%x "
                        "(%u of %u LBAs done)\n",
                        (uint64_t)(lba + done), (uint64_t)batch,
                        (uint64_t)st, (uint64_t)done, (uint64_t)nlb);
            return done;
        }
        done += batch;
    }
    return done;
}

uint32_t nvme_write(nvme_t *n, uint64_t lba, const void *buf, uint32_t nlb) {
    if (!n->present || nlb == 0) return 0;
    uint32_t done = 0;
    while (done < nlb) {
        uint32_t batch = nlb - done;
        if (batch > NVME_MAX_LBAS_PER_CMD) batch = NVME_MAX_LBAS_PER_CMD;
        uint32_t st = io_cmd(n, IO_WRITE, lba + done,
                             (const uint8_t *)buf + (uint64_t)done * NVME_LBA_SIZE,
                             batch * NVME_LBA_SIZE, batch);
        if (st != 0) return done;
        done += batch;
    }
    return done;
}

bool nvme_flush(nvme_t *n) {
    if (!n->present) return false;
    return io_cmd(n, IO_FLUSH, 0, (void *)0, 0, 0) == 0;
}
