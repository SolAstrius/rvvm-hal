/* probe-s — minimal first-light test for the S-mode + SBI HAL build.
 *
 * Booted by OpenSBI as the next-stage S-mode payload. We print a
 * banner via UART (NS16550A is mapped to S-mode by RVVM, same as in
 * M-mode), walk the FDT to confirm the same machine layout we know
 * from the M-mode probe, exercise plat_timer (one wfi-paced sleep),
 * and shut down via the SBI SRST extension.
 *
 * RAM start is 0x80200000 (link_s.ld). OpenSBI lives at 0x80000000.
 *
 * Run: rvvm fw_dynamic.bin -k firmware.bin -nogui -nonet -nosound
 *      RVVM passes `firmware.bin` to OpenSBI as the M-mode firmware
 *      and `-k firmware.bin` is loaded at RAM+0x200000 = 0x80200000,
 *      which OpenSBI fw_jump targets by default. */

#include "uart.h"
#include "time.h"
#include "fdt.h"
#include "plat.h"
#include "rvvm.h"
#include "fw_cfg.h"
#include "cfi.h"
#include "hid.h"
#include "gfx.h"
#include <stdint.h>
#include <stddef.h>

extern char __bss_start[], __bss_end[];

void probe_on_key(uint8_t u, bool pressed, void *ctx) {
    (void)ctx;
    uart_printf("  hid: usage=0x%x %s\n",
                (uint64_t)u, pressed ? "DOWN" : "up");
}

/* SBI System Reset extension — used to power off cleanly so the host
 * RVVM exits 0. EID 0x53525354 ("SRST"), FID 0 = sbi_system_reset. */
static void sbi_shutdown(void) {
    register unsigned long a0 __asm__("a0") = 0;   /* type=shutdown */
    register unsigned long a1 __asm__("a1") = 0;   /* reason=none   */
    register unsigned long a6 __asm__("a6") = 0;
    register unsigned long a7 __asm__("a7") = 0x53525354UL;
    __asm__ volatile ("ecall"
                      : "+r"(a0), "+r"(a1)
                      : "r"(a6), "r"(a7)
                      : "memory");
}

void kmain(uint64_t hartid, uint64_t fdt_addr) {
    uart_init(0);
    uart_puts("\n=== rvvm-hal probe-s (S-mode under SBI) ===\n");
    uart_printf("hart=%u  fdt=%p  bss=%u bytes\n",
                hartid, (void *)(uintptr_t)fdt_addr,
                (uint64_t)(__bss_end - __bss_start));

    /* Read sstatus to prove we're in S-mode (SPP bit reflects the
     * privilege we trapped *from*; checking it isn't useful here, but
     * reading sstatus at all would have trapped in M-mode if we were
     * confused about the build). */
    uint64_t sstatus;
    __asm__ volatile ("csrr %0, sstatus" : "=r"(sstatus));
    uart_printf("sstatus = %x  (csrr sstatus succeeded → we are in S-mode)\n",
                sstatus);

    fdt_t fdt;
    if (!fdt_init(&fdt, (const void *)(uintptr_t)fdt_addr)) {
        uart_puts("FDT invalid; aborting.\n");
        sbi_shutdown();
        for (;;) __asm__ volatile ("wfi");
    }

    /* Hand the FDT to the platform layer for runtime feature probes
     * (Sstc → direct stimecmp path, skipping sbi_set_timer ecalls). */
    plat_probe_features(&fdt);

    /* List the same handful of devices the M-mode probe lists; we
     * expect the FDT layout to be identical. */
    static const char *want[] = {
        "ns16550a", "sifive,plic-1.0.0", "sifive,clint0",
        "google,goldfish-rtc", "syscon", NULL,
    };
    uart_puts("Devices visible via FDT:\n");
    for (const char **p = want; *p; p++) {
        uint32_t off = fdt_find_compatible(&fdt, *p);
        if (off == UINT32_MAX) {
            uart_printf("  (none)  %s\n", *p);
            continue;
        }
        uint64_t at = 0, sz = 0;
        fdt_node_reg64(&fdt, off, 0, &at, &sz);
        uart_printf("  %s @ %p  size=%x\n",
                    *p, (void *)(uintptr_t)at, sz);
    }

    /* Bring up time subsystem from FDT. SBI sets the timer; we just
     * need timebase-frequency from /cpus to scale frame deadlines. */
    uint32_t cpus_off = fdt_find_node_named(&fdt, "cpus");
    uint32_t hz = 0;
    if (cpus_off != UINT32_MAX)
        fdt_node_prop_u32(&fdt, cpus_off, "timebase-frequency", &hz);
    time_init(0, hz);
    uart_printf("\ntime: %u Hz; time_now() = %x\n",
                (uint64_t)time_hz(), time_now());

    /* Sleep one frame using plat_timer + wfi. Confirms the timer path
     * works and that sip.STIP wakes wfi. With Sstc, the deadline is
     * armed via direct csrw stimecmp; otherwise via sbi_set_timer
     * ecall. The probe-s output overhead per frame should drop
     * noticeably with Sstc on. */
    uart_puts("time: sleeping one frame...\n");
    uint64_t before = time_now();
    time_busy_until(before + time_ticks_per_frame());
    uint64_t after = time_now();
    uart_printf("time: woke after %u ticks (target %u, overhead %u)\n",
                after - before, time_ticks_per_frame(),
                (after - before) - time_ticks_per_frame());

    /* Repeat 50× to amortise out the print latency in the first
     * sample, giving a steadier average. */
    uint64_t total_overhead = 0;
    const uint32_t reps = 50;
    for (uint32_t i = 0; i < reps; i++) {
        uint64_t t0 = time_now();
        time_busy_until(t0 + time_ticks_per_frame());
        uint64_t t1 = time_now();
        total_overhead += (t1 - t0) - time_ticks_per_frame();
    }
    uart_printf("time: avg overhead over %u frames = %u ticks (%u us)\n",
                (uint64_t)reps,
                total_overhead / reps,
                (total_overhead / reps) / 10);   /* 10 MHz → 1 tick = 100 ns */

    /* Full FDT enumeration — every node, with first compat string
     * and root `reg` if present.  Useful as a side-by-side snapshot
     * of the host machine: RVVM's default vs QEMU virt's default
     * differ noticeably in non-essential devices (virtio, fw-cfg,
     * cfi-flash on QEMU; bochs/HDA/i2c-oc/ATA on RVVM as PCI), but
     * agree on the core set the HAL drives (UART, CLINT, PLIC, RTC,
     * syscon, ECAM). The reg-decoder assumes the SoC default of
     * #address-cells=2 / #size-cells=2; nodes under busses with
     * other cells (QEMU's platform-bus is 1/1) will print `reg=?`. */
    /* QEMU-only: fw-cfg. Pulls a few well-known entries to prove the
     * byte-stream + selector handshake works, then dumps the file
     * directory so we can see what the host published. */
    if (fw_cfg_init_fdt(&fdt)) {
        uart_puts("\nfw-cfg: ");
        char sig[5] = {0};
        fw_cfg_select(FW_CFG_SIGNATURE);
        fw_cfg_read(sig, 4);
        uart_printf("signature='%s' ", sig);

        uint32_t id_le = 0;
        fw_cfg_select(FW_CFG_ID);
        fw_cfg_read(&id_le, 4);   /* this field is little-endian */
        uart_printf("id=%x ", (uint64_t)id_le);

        uint16_t ncpus = 0;
        fw_cfg_select(FW_CFG_NB_CPUS);
        fw_cfg_read(&ncpus, 2);   /* little-endian */
        uart_printf("nb_cpus=%u\n", (uint64_t)ncpus);

        /* Walk the file directory. The file dir starts with a u32 BE
         * count; we re-do the read here rather than calling
         * fw_cfg_find_file because we want to print every entry. */
        fw_cfg_select(FW_CFG_FILE_DIR);
        uint8_t cb[4];
        fw_cfg_read(cb, 4);
        uint32_t count = ((uint32_t)cb[0] << 24) | ((uint32_t)cb[1] << 16)
                       | ((uint32_t)cb[2] << 8)  |  (uint32_t)cb[3];
        uart_printf("fw-cfg: %u file(s):\n", (uint64_t)count);
        for (uint32_t i = 0; i < count; i++) {
            fw_cfg_file_t e;
            fw_cfg_read(&e, sizeof(e));
            uint32_t sz   = ((uint32_t)((uint8_t *)&e.size)[0] << 24)
                          | ((uint32_t)((uint8_t *)&e.size)[1] << 16)
                          | ((uint32_t)((uint8_t *)&e.size)[2] << 8)
                          |  (uint32_t)((uint8_t *)&e.size)[3];
            uint16_t sel  = ((uint16_t)((uint8_t *)&e.select)[0] << 8)
                          |  (uint16_t)((uint8_t *)&e.select)[1];
            uart_printf("  [sel=%x size=%u]  %s\n",
                        (uint64_t)sel, (uint64_t)sz, e.name);
        }
    } else {
        uart_puts("\nfw-cfg: not present (RVVM or non-QEMU host)\n");
    }

    /* QEMU-only: CFI parallel-NOR flash. With no `-drive if=pflash`
     * the region is unbacked and the QRY signature won't show; we
     * just report that and move on. */
    cfi_t flash;
    if (cfi_init_fdt(&flash, &fdt) && flash.ok) {
        uart_printf("cfi-flash: %u MiB, cmdset=%x at %p (bank-width=%u)\n",
                    (uint64_t)(flash.size_bytes >> 20),
                    (uint64_t)flash.cmdset,
                    (void *)flash.base,
                    (uint64_t)flash.bank_width);
        uint8_t head[16];
        cfi_read(&flash, 0, head, sizeof(head));
        uart_puts("cfi-flash: first 16 bytes: ");
        for (uint32_t i = 0; i < sizeof(head); i++)
            uart_printf("%x ", (uint64_t)head[i]);
        uart_putc('\n');
    } else {
        uart_puts("cfi-flash: no QRY signature (unbacked or absent)\n");
    }

    /* QEMU virt has a second pflash bank at 0x22000000 (FDT exposes
     * it as reg[1] of the same node). Try reading that one too —
     * useful when the host launches with `-drive if=pflash,unit=1`
     * which leaves bank 0 unbacked but stages content in bank 1. */
    cfi_t flash1;
    if (cfi_init(&flash1, 0x22000000UL, 4) && flash1.ok) {
        uint8_t head[16];
        cfi_read(&flash1, 0, head, sizeof(head));
        uart_printf("cfi-flash bank1: %u MiB, first 16: ",
                    (uint64_t)(flash1.size_bytes >> 20));
        for (uint32_t i = 0; i < sizeof(head); i++)
            uart_printf("%x ", (uint64_t)head[i]);
        uart_putc('\n');
    }

    /* Cross-host graphics. RVVM picks Bochs (or simplefb), QEMU virt
     * with `-device virtio-gpu-device` picks the virtio backend.
     * Either way the same gfx_rect calls land on screen. */
    static gfx_t g;
    if (gfx_init_fdt(&g, &fdt, 640, 400)) {
        const char *which = (g.backend == GFX_BACKEND_BOCHS)      ? "bochs"
                          : (g.backend == GFX_BACKEND_SIMPLEFB)   ? "simplefb"
                          : (g.backend == GFX_BACKEND_VIRTIO_GPU) ? "virtio-gpu"
                                                                  : "?";
        uart_printf("\ngfx: %s backend, %ux%u\n", which,
                    (uint64_t)g.width, (uint64_t)g.height);
        /* RGBW four-bar test pattern. gfx_rect auto-presents on the
         * virtio-gpu backend, no-ops the present on Bochs/simplefb. */
        uint32_t bw = g.width / 4;
        uint32_t colours[4] = {0x00FF0000, 0x0000FF00, 0x000000FF, 0x00FFFFFF};
        for (int i = 0; i < 4; i++)
            gfx_rect(&g, i * bw, 0, bw, g.height, colours[i]);
        uart_puts("gfx: 4-bar test pattern drawn\n");
    } else {
        uart_puts("\ngfx: no display backend\n");
    }

    /* Cross-host keyboard discovery. On QEMU `-device
     * virtio-keyboard-device` this picks the virtio-input backend;
     * on RVVM it picks i2c-HID. Same hid_kb_poll() loop drains
     * either, and both deliver USB HID usage codes to the callback.
     * Poll for ~0.5 s so a human can press a few keys. */
    static hid_keyboard_t kb;
    if (hid_kb_init_fdt(&kb, &fdt)) {
        extern void probe_on_key(uint8_t, bool, void *);
        const char *which = (kb.backend == HID_BACKEND_VIRTIO) ? "virtio-input"
                          : (kb.backend == HID_BACKEND_I2C)    ? "i2c-hid"
                                                               : "?";
        uart_printf("\nhid: %s backend up; type for ~0.5 s ...\n", which);
        uint32_t total = 0;
        for (uint32_t f = 0; f < 30; f++) {
            total += (uint32_t)hid_kb_poll(&kb, probe_on_key, NULL);
            time_busy_until(time_now() + time_ticks_per_frame());
        }
        uart_printf("hid: %u event(s) captured\n", (uint64_t)total);
    } else {
        uart_puts("\nhid: no keyboard backend available\n");
    }

    uart_puts("\nFull FDT walk:\n");
    /* Token-level walk. fdt_next_node()'s API can only find one node
     * forward and can't iterate siblings, so we drive the state
     * machine ourselves: every FDT_BEGIN_NODE is a node we want to
     * print (with its compat + reg), and we then descend by stepping
     * past just the BEGIN_NODE + name; FDT_PROP advances by prop size.
     * Depth-first order = DTB file order. Indent by tracking depth. */
    {
        uint32_t walk = 0;
        int depth = 0;
        while (walk < fdt.struct_size) {
            uint32_t tok = fdt_read_be32(fdt.struct_blob, walk);
            if (tok == FDT_BEGIN_NODE) {
                const char *name = fdt_node_name(&fdt, walk);
                uint32_t clen = 0;
                const char *compat = (const char *)fdt_node_prop(&fdt, walk,
                                                                 "compatible", &clen);
                uint64_t at = 0, sz = 0;
                bool has_reg = fdt_node_reg64(&fdt, walk, 0, &at, &sz);

                for (int d = 0; d < depth; d++) uart_puts("  ");
                uart_puts(*name ? name : "/");
                if (has_reg) uart_printf(" @%p (size=%x)",
                                         (void *)(uintptr_t)at, sz);
                if (compat && clen) {
                    uart_puts("  compat=");
                    for (uint32_t i = 0; i < clen && compat[i]; i++)
                        uart_putc(compat[i]);
                }
                uart_putc('\n');

                /* Step past BEGIN_NODE + NUL-terminated name (4-byte aligned). */
                walk += 4;
                while (walk < fdt.struct_size && fdt.struct_blob[walk]) walk++;
                walk = (walk + 4) & ~3U;
                depth++;
            } else if (tok == FDT_END_NODE) {
                walk += 4;
                if (depth) depth--;
            } else if (tok == FDT_PROP) {
                /* PROP layout: token | len(BE32) | nameoff(BE32) | data[len]. */
                uint32_t plen = fdt_read_be32(fdt.struct_blob, walk + 4);
                walk += 12 + ((plen + 3) & ~3U);
            } else if (tok == FDT_NOP) {
                walk += 4;
            } else {
                break;   /* FDT_END or garbage */
            }
        }
    }

    uart_puts("\nprobe-s: clean shutdown via SBI SRST.\n");
    sbi_shutdown();
    for (;;) __asm__ volatile ("wfi");
}
