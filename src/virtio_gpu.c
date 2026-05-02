#include "virtio_gpu.h"
#include "uart.h"
#include <stddef.h>

/* Synchronous "submit one command, wait for response" helper. The
 * controlq carries every command as a 2-descriptor chain:
 *   desc[head]      = request, READ-only,  NEXT → resp_head
 *   desc[resp_head] = response, WRITE-only, terminator
 *
 * We push the head, notify, and poll used.idx until the device
 * returns it. fits-in-cache simple — no IRQs, no fences beyond what
 * virtq_avail_push already emits. */
static bool gpu_cmd_sync(virtio_gpu_t *vg,
                         const void *req, uint32_t req_len,
                         void *rsp, uint32_t rsp_len) {
    uint16_t head = virtq_alloc_desc(&vg->controlq);
    uint16_t rid  = virtq_alloc_desc(&vg->controlq);
    if (head == 0xFFFF || rid == 0xFFFF) {
        if (rid != 0xFFFF) virtq_free_desc(&vg->controlq, rid);
        if (head != 0xFFFF) virtq_free_desc(&vg->controlq, head);
        return false;
    }

    /* Copy request into the device-visible req_buf so its physical
     * address is stable (the caller's storage might be on the stack,
     * which is fine here — same address space — but we standardise on
     * the scratch buffer to keep things uniform). */
    uint8_t *rb = vg->req_buf;
    for (uint32_t i = 0; i < req_len; i++) rb[i] = ((const uint8_t *)req)[i];

    vg->controlq.desc[head].addr  = (uint64_t)(uintptr_t)rb;
    vg->controlq.desc[head].len   = req_len;
    vg->controlq.desc[head].flags = VIRTQ_DESC_F_NEXT;
    vg->controlq.desc[head].next  = rid;

    vg->controlq.desc[rid].addr  = (uint64_t)(uintptr_t)vg->rsp_buf;
    vg->controlq.desc[rid].len   = rsp_len;
    vg->controlq.desc[rid].flags = VIRTQ_DESC_F_WRITE;
    vg->controlq.desc[rid].next  = 0;

    virtq_avail_push(&vg->controlq, head);
    virtio_notify(&vg->dev, /*qsel=*/0);

    /* Poll for completion. virtio-gpu is host-side software, so
     * commands return in microseconds; no need for wfi. */
    uint16_t cid;
    uint32_t clen;
    for (;;) {
        if (virtq_used_pop(&vg->controlq, &cid, &clen)) {
            /* Devices can return either descriptor head; with chains
             * they return `head`. cid should match. */
            (void)cid;
            if (rsp && rsp_len) {
                uint8_t *out = (uint8_t *)rsp;
                uint8_t *src = vg->rsp_buf;
                uint32_t n = (clen < rsp_len) ? clen : rsp_len;
                for (uint32_t i = 0; i < n; i++) out[i] = src[i];
            }
            break;
        }
    }

    virtq_free_desc(&vg->controlq, rid);
    virtq_free_desc(&vg->controlq, head);

    /* All our 2D commands return OK_NODATA (0x1100) or
     * OK_DISPLAY_INFO (0x1101) on success; anything else is a fault. */
    if (rsp && rsp_len >= sizeof(struct virtio_gpu_ctrl_hdr)) {
        uint32_t type = ((struct virtio_gpu_ctrl_hdr *)rsp)->type;
        if (type != VIRTIO_GPU_RESP_OK_NODATA &&
            type != VIRTIO_GPU_RESP_OK_DISPLAY_INFO) {
            return false;
        }
    }
    return true;
}

static bool first_gpu(uintptr_t base, uint32_t id, void *ctx) {
    (void)base; (void)ctx;
    return id == VIRTIO_ID_GPU;
}

bool virtio_gpu_init_fdt(virtio_gpu_t *vg, const fdt_t *fdt,
                         uint32_t want_w, uint32_t want_h) {
    uintptr_t base = virtio_find_fdt(fdt, first_gpu, NULL);
    if (!base) return false;

    if (!virtio_init(&vg->dev, base, 0, 0)) return false;

    if (!virtio_queue_setup(&vg->dev, /*qsel=*/0, &vg->controlq,
                            VIRTIO_GPU_QSIZE,
                            vg->desc, vg->avail_buf, vg->used_buf)) {
        return false;
    }
    virtq_free_init(&vg->controlq);
    virtio_finalize(&vg->dev);

    /* Step 1: GET_DISPLAY_INFO — host tells us the preferred mode of
     * scanout 0. We respect its width/height if non-zero, otherwise
     * use the caller's want_w/want_h. */
    struct virtio_gpu_ctrl_hdr di_req = { .type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO };
    struct virtio_gpu_resp_display_info di_rsp;
    for (uint32_t i = 0; i < sizeof(di_rsp); i++)
        ((uint8_t *)&di_rsp)[i] = 0;
    if (!gpu_cmd_sync(vg, &di_req, sizeof(di_req),
                      &di_rsp, sizeof(di_rsp))) {
        return false;
    }

    uint32_t w = want_w;
    uint32_t h = want_h;
    if (di_rsp.pmodes[0].enabled && di_rsp.pmodes[0].r.width != 0) {
        /* Host advertised a preferred mode; clamp our request to it. */
        if (w > di_rsp.pmodes[0].r.width)  w = di_rsp.pmodes[0].r.width;
        if (h > di_rsp.pmodes[0].r.height) h = di_rsp.pmodes[0].r.height;
    }
    if (w * h * 4 > sizeof(vg->vram)) return false;

    vg->width       = w;
    vg->height      = h;
    vg->resource_id = 1;

    /* Step 2: RESOURCE_CREATE_2D — allocate the host-side resource. */
    struct virtio_gpu_resource_create_2d c2d = {
        .hdr         = { .type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D },
        .resource_id = vg->resource_id,
        .format      = VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM,
        .width       = w,
        .height      = h,
    };
    struct virtio_gpu_ctrl_hdr ok;
    if (!gpu_cmd_sync(vg, &c2d, sizeof(c2d), &ok, sizeof(ok))) {
        return false;
    }

    /* Step 3: RESOURCE_ATTACH_BACKING — point the resource at our
     * vram[]. nr_entries=1, single contiguous entry. */
    struct virtio_gpu_resource_attach_backing ab = {
        .hdr         = { .type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING },
        .resource_id = vg->resource_id,
        .nr_entries  = 1,
        .addr        = (uint64_t)(uintptr_t)vg->vram,
        .length      = w * h * 4,
        .padding     = 0,
    };
    if (!gpu_cmd_sync(vg, &ab, sizeof(ab), &ok, sizeof(ok))) {
        return false;
    }

    /* Step 4: SET_SCANOUT — bind the resource to scanout 0. */
    struct virtio_gpu_set_scanout ss = {
        .hdr         = { .type = VIRTIO_GPU_CMD_SET_SCANOUT },
        .r           = { .x = 0, .y = 0, .width = w, .height = h },
        .scanout_id  = 0,
        .resource_id = vg->resource_id,
    };
    if (!gpu_cmd_sync(vg, &ss, sizeof(ss), &ok, sizeof(ok))) {
        return false;
    }

    return true;
}

bool virtio_gpu_present(virtio_gpu_t *vg,
                        uint32_t x, uint32_t y,
                        uint32_t w, uint32_t h) {
    if (!vg->resource_id) return false;
    /* Clamp to current mode. */
    if (x >= vg->width || y >= vg->height) return true;   /* nothing visible */
    if (x + w > vg->width)  w = vg->width  - x;
    if (y + h > vg->height) h = vg->height - y;
    if (w == 0 || h == 0)  return true;

    /* TRANSFER_TO_HOST_2D copies the dirty rect from our backing
     * buffer into the host-side resource. Offset is the byte offset
     * within the backing of the rect's top-left corner. */
    struct virtio_gpu_transfer_to_host_2d tx = {
        .hdr         = { .type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D },
        .r           = { .x = x, .y = y, .width = w, .height = h },
        .offset      = (uint64_t)(y * vg->width + x) * 4,
        .resource_id = vg->resource_id,
        .padding     = 0,
    };
    struct virtio_gpu_ctrl_hdr ok;
    if (!gpu_cmd_sync(vg, &tx, sizeof(tx), &ok, sizeof(ok))) {
        return false;
    }

    /* RESOURCE_FLUSH presents the dirty rect on the screen. */
    struct virtio_gpu_resource_flush fl = {
        .hdr         = { .type = VIRTIO_GPU_CMD_RESOURCE_FLUSH },
        .r           = { .x = x, .y = y, .width = w, .height = h },
        .resource_id = vg->resource_id,
        .padding     = 0,
    };
    return gpu_cmd_sync(vg, &fl, sizeof(fl), &ok, sizeof(ok));
}
