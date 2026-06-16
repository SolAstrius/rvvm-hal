/* virtio-gpu (2D) — modern virtio-mmio framebuffer.
 *
 * Plumbed under gfx.c as a third backend behind Bochs and
 * simple-framebuffer. The consumer API is unchanged: gfx_init_fdt()
 * picks whichever is present; the gfx_t exposes a writable vram
 * pointer; gfx_rect / gfx_fill / gfx_pixel work as before.
 *
 * Difference from Bochs and simplefb: writes to vram do NOT directly
 * appear on the host's display. The guest must explicitly issue a
 * TRANSFER_TO_HOST_2D + RESOURCE_FLUSH pair after each batch of
 * updates. To keep the existing gfx surface drop-in compatible,
 * gfx_rect / gfx_fill auto-flush the modified region. Consumers
 * that write through the raw vram pointer themselves call
 * gfx_present() or gfx_present_rect() to push their changes.
 *
 * Spec: virtio-v1.2 §5.7 (GPU Device); we implement the 2D subset
 * only — control queue, no cursor queue, no 3D / virgl, no EDID
 * negotiation, no multi-scanout. Plenty for "draw a UI on QEMU".
 */

#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "virtio.h"

/* Command opcodes (virtio-v1.2 §5.7.6.7). */
#define VIRTIO_GPU_CMD_GET_DISPLAY_INFO       0x0100
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D     0x0101
#define VIRTIO_GPU_CMD_RESOURCE_UNREF         0x0102
#define VIRTIO_GPU_CMD_SET_SCANOUT            0x0103
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH         0x0104
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D    0x0105
#define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING 0x0106
#define VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING 0x0107

/* Response codes. */
#define VIRTIO_GPU_RESP_OK_NODATA             0x1100
#define VIRTIO_GPU_RESP_OK_DISPLAY_INFO       0x1101

/* Pixel formats (subset). B8G8R8X8 = bytes in memory are B, G, R, X
 * — matches our XRGB8888 (uint32 0xXXRRGGBB) on little-endian. */
#define VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM      1

/* The host advertises up to 16 scanouts; we only use scanout 0. */
#define VIRTIO_GPU_MAX_SCANOUTS               16

/* Command framing structures. All little-endian on the wire and
 * packed to match host expectations exactly. */

struct virtio_gpu_ctrl_hdr {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint8_t  ring_idx;
    uint8_t  padding[3];
} __attribute__((packed));

struct virtio_gpu_rect {
    uint32_t x, y, width, height;
} __attribute__((packed));

struct virtio_gpu_resp_display_info {
    struct virtio_gpu_ctrl_hdr hdr;
    struct {
        struct virtio_gpu_rect r;
        uint32_t enabled;
        uint32_t flags;
    } pmodes[VIRTIO_GPU_MAX_SCANOUTS];
} __attribute__((packed));

struct virtio_gpu_resource_create_2d {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t format;
    uint32_t width;
    uint32_t height;
} __attribute__((packed));

struct virtio_gpu_resource_attach_backing {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t nr_entries;
    /* Followed by `nr_entries` of mem_entry. We always use 1. */
    uint64_t addr;
    uint32_t length;
    uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_set_scanout {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint32_t scanout_id;
    uint32_t resource_id;
} __attribute__((packed));

struct virtio_gpu_transfer_to_host_2d {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_resource_flush {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed));

/* Compile-time framebuffer ceiling. Bumping this only costs BSS, not
 * cycles — virtio-gpu commands address sub-rectangles, so a smaller
 * actual mode doesn't pay for the unused tail. 4 MiB = 1024×1024 in
 * 32-bit colour, plenty for menu UI.
 *
 * Override at HAL build time (e.g. -DVIRTIO_GPU_VRAM_BYTES=0x100000 for
 * 1 MiB / 640×400) to reclaim BSS — useful for small-RAM targets or
 * firmware that only ever uses the Bochs backend, where this buffer is
 * allocated but unused. Must still cover the largest mode you set. */
#ifndef VIRTIO_GPU_VRAM_BYTES
#define VIRTIO_GPU_VRAM_BYTES   (4 * 1024 * 1024)
#endif

#define VIRTIO_GPU_QSIZE        16

/* All state for one virtio-gpu device. The framebuffer storage and
 * queue rings live in the struct so a single static instance in
 * BSS is sufficient. */
typedef struct {
    virtio_dev_t dev;
    virtq_t      controlq;

    uint32_t     width;
    uint32_t     height;
    uint32_t     resource_id;

    struct virtq_desc desc[VIRTIO_GPU_QSIZE];
    uint8_t avail_buf[4 + VIRTIO_GPU_QSIZE * 2 + 8]
        __attribute__((aligned(16)));
    uint8_t used_buf[4 + VIRTIO_GPU_QSIZE * 8 + 8]
        __attribute__((aligned(16)));

    uint8_t vram[VIRTIO_GPU_VRAM_BYTES] __attribute__((aligned(16)));

    /* Pre-allocated request/response scratch — one in flight at a
     * time is fine for a synchronous API and avoids dynamic
     * allocation from BSS. Sized to the largest command we issue. */
    uint8_t req_buf[256] __attribute__((aligned(16)));
    uint8_t rsp_buf[1024] __attribute__((aligned(16)));
} virtio_gpu_t;

/* Discover the first virtio-gpu device on the bus, bring it up, and
 * mode-set to want_w×want_h. Returns true if the device was found
 * and the scanout configured. The caller-owned `vg` must outlive
 * use; we keep references into its embedded buffers. */
bool virtio_gpu_init_fdt(virtio_gpu_t *vg, const fdt_t *fdt,
                         uint32_t want_w, uint32_t want_h);

/* Push a sub-rectangle of vram to the host display. The pixels at
 * vg->vram + (y*stride_px + x)*4 ... are copied and presented. */
bool virtio_gpu_present(virtio_gpu_t *vg,
                        uint32_t x, uint32_t y,
                        uint32_t w, uint32_t h);
