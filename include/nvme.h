/* Tiny NVMe over PCIe block driver. Single namespace per controller,
 * single I/O submission/completion queue pair, polling-based completion.
 *
 * Replaces ata.h since RVVM caps ATA at one disk total but lets you
 * attach as many NVMe controllers as you want (`-nvme file1 -nvme
 * file2 ...`). Use nvme_init_nth() to bind to controller 0, 1, 2, etc.
 *
 * Targets RVVM's NVMe emulation (src/devices/nvme.c, vendor 0x1F31
 * device 0x4512, NVMe v1.4, 4 KiB pages, 512 B LBAs). 64-entry admin
 * and I/O queues, 4 KiB PRP list. Largest single transfer is ~2 MiB
 * (one PRP-list page × 512 entries × 4 KiB), which covers everything
 * realistic for our firmware.
 *
 * Storage layout: each nvme_t embeds five page-aligned 4 KiB buffers
 * (admin SQ/CQ, I/O SQ/CQ, PRP list) plus state. Total ~20 KiB; declare
 * as `static nvme_t disk;` not on the stack. */

#pragma once
#include <stdint.h>
#include <stdbool.h>

#define NVME_LBA_SIZE       512U
#define NVME_PAGE_SIZE      4096U

/* PRP list capacity (§4.1.2). One PRP list page holds 512 8-byte
 * entries; in a multi-page list the last entry of each non-final
 * page is a chain pointer to the next page (NVMe spec §4.1.2.2),
 * so chained pages give 511 data entries each. With 16 pages:
 *
 *   max data entries = 15 chained × 511 + 1 final × 512 = 8177
 *   max single transfer = (1 PRP1 + 8177 list entries) × 4 KiB ≈ 32 MiB
 *
 * Larger transfers are split internally by nvme_read / nvme_write.
 * 16 pages costs 64 KiB per nvme_t — acceptable for our cores
 * (typical use is 1-3 nvme_t instances per firmware). */
#define NVME_PRP_LIST_PAGES   16

/* Max single NVMe command transfer in BYTES, given NVME_PRP_LIST_PAGES.
 *   = (1 PRP1 page) + (15 chained × 511 entries) + (1 final × 512)
 *   = 1 + 15*511 + 512 = 8178 pages
 *   = 8178 × 4 KiB = ~32 MiB
 * nvme_read / nvme_write split larger callers internally. Exposed
 * here in case a caller wants to pre-size buffers to avoid the split. */
#define NVME_MAX_SINGLE_TRANSFER  (8178u * NVME_PAGE_SIZE)

/* Static storage for the queues + PRP list. Page-aligned via the
 * struct's own aligned attribute (these fields are first). */
typedef struct __attribute__((aligned(4096))) {
    uint8_t asq_buf[NVME_PAGE_SIZE];
    uint8_t acq_buf[NVME_PAGE_SIZE];
    uint8_t iosq_buf[NVME_PAGE_SIZE];
    uint8_t iocq_buf[NVME_PAGE_SIZE];
    uint8_t prp_list[NVME_PRP_LIST_PAGES * NVME_PAGE_SIZE];

    uintptr_t bar0;
    uint64_t  num_lbas;       /* namespace size in 512 B blocks */

    uint16_t  aq_entries;     /* admin queue depth (in SQEs / CQEs) */
    uint16_t  ioq_entries;

    uint16_t  asq_tail;
    uint16_t  acq_head;
    uint16_t  iosq_tail;
    uint16_t  iocq_head;
    uint16_t  cmd_id;

    /* Expected phase bit for the next CQE to consume. NVMe completions
     * toggle their phase bit each time the consumer wraps the queue,
     * which is how software detects new entries without a doorbell. */
    bool      acq_phase;
    bool      iocq_phase;
    bool      present;
} nvme_t;

/* Bind to the first NVMe controller on the PCI bus. Returns false if
 * none found, BARs missing, or controller failed to ready. */
bool nvme_init(nvme_t *n);

/* Bind to the Nth NVMe controller (0 = first). Lets a firmware open
 * multiple `-nvme` images independently. */
bool nvme_init_nth(nvme_t *n, uint32_t idx);

/* Read/write `nlb` 512-B blocks starting at LBA `lba`. Returns the
 * number of blocks transferred (0 on failure, `nlb` on success — partial
 * results aren't possible in NVMe within a single command). */
uint32_t nvme_read (nvme_t *n, uint64_t lba,       void *buf, uint32_t nlb);
uint32_t nvme_write(nvme_t *n, uint64_t lba, const void *buf, uint32_t nlb);

/* Issue a Flush command (forces the device to commit pending writes
 * to non-volatile storage). Returns true on success. */
bool nvme_flush(nvme_t *n);
