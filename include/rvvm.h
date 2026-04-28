/* rvvm.h — RVVM machine topology as seen from a bare-metal guest.
 *
 * Single source of truth for every magic address, IRQ number, PCI device
 * ID, and register offset our firmware pokes at. Each entry cross-refs
 * the RVVM source path it was lifted from so it's easy to re-verify
 * when RVVM changes.
 *
 * Scope: the default machine layout produced by `rvvm <firmware>` with
 * no architecture flags. AIA mode (`-riscv_aia`) and other variations
 * shift addresses around — not modelled here yet. */

#pragma once
#include <stdint.h>

/* ======================================================================
 *  Memory map  (RVVM defaults — see src/rvvm.c, src/devices/...)
 * ====================================================================== */

#define RVVM_RAM_BASE          0x80000000UL  /* RAM origin = reset PC.
                                                src/rvvm.c:32 RVVM_DEFAULT_MEMBASE,
                                                src/rvvm.c:568 RVVM_OPT_RESET_PC. */

#define RVVM_PCI_IO_BASE       0x03000000UL  /* 64 KiB PCI IO window.
                                                src/devices/pci-bus.h:30. */
#define RVVM_PCI_IO_SIZE       0x00010000UL  /* src/devices/pci-bus.h:31. */

#define RVVM_PCI_ECAM_BASE     0x30000000UL  /* PCI Enhanced Config window.
                                                src/devices/pci-bus.h:29. */
#define RVVM_PCI_ECAM_SIZE     0x10000000UL  /* 256 buses × 1 MiB each. */

#define RVVM_PCI_MEM_BASE      0x40000000UL  /* 1 GiB PCI MMIO window.
                                                src/devices/pci-bus.h:32. */
#define RVVM_PCI_MEM_SIZE      0x40000000UL  /* src/devices/pci-bus.h:33. */

/* ======================================================================
 *  Native MMIO devices  (always present in the default machine)
 * ====================================================================== */

/* NS16550A UART, 4 KiB region, byte-aligned regs.
 * src/devices/ns16550a.h:16 NS16550A_ADDR_DEFAULT.
 * src/devices/ns16550a.c lines 35-65 for register offsets. */
#define RVVM_UART_BASE         0x10000000UL
#define RVVM_UART_SIZE         0x00001000UL

/* OpenCores I²C master, 4 KiB region, byte regs at offsets {00,04,08,0C,10}.
 * src/devices/i2c-oc.h:15 I2C_OC_ADDR_DEFAULT.
 * src/devices/i2c-oc.c lines 36-58 for registers and CMD/STATUS bits. */
#define RVVM_I2C_OC_BASE       0x10030000UL
#define RVVM_I2C_OC_SIZE       0x00001000UL

/* RISC-V CLINT — `compatible = "sifive,clint0"`.
 * src/devices/riscv-aclint.h:15 CLINT_ADDR_DEFAULT.
 *
 * Layout (SiFive convention, NOT in FDT — implied by the compat string):
 *   0x0000 + hartid*4   MSWI   msip[hartid]      (4-byte ops only)
 *   0x4000 + hartid*8   MTIMER mtimecmp[hartid]  (8-byte ops only)
 *   0xBFF8              MTIMER mtime
 *
 * Default address — firmware should discover via FDT and pass to
 * time_init(); this is the fallback. */
#define RVVM_CLINT_BASE        0x02000000UL
#define RVVM_CLINT_SIZE        0x00010000UL

/* SiFive PLIC — `compatible = "sifive,plic-1.0.0"`.
 * src/devices/riscv-plic.h:16 PLIC_ADDR_DEFAULT.
 * src/devices/riscv-plic.c:17 PLIC_MMIO_SIZE.
 *
 * Layout (MMIO offsets):
 *   0x000000 + irq*4         : interrupt priority (1..7; 0 disables)
 *   0x001000 + reg*4         : pending bits, 32 IRQs per reg
 *   0x002000 + ctx*0x80
 *           + reg*4          : enable bits per (ctx, irq word)
 *   0x200000 + ctx*0x1000    : threshold (only IRQ prio > threshold is delivered)
 *   0x200004 + ctx*0x1000    : claim/complete (read = claim, write = complete)
 *
 * Context ↔ hart layout (riscv-plic.c:43-54):
 *   ctx = (hartid << 1) | (mode==S ? 1 : 0)
 * So for hart 0 in M-mode (our firmware), ctx = 0.
 *
 * IRQ source numbers are allocated dynamically by the host devices in
 * the order they're attached (`rvvm_alloc_irq`), so we discover them
 * via FDT (`interrupts` property of each device's node) or via PCI
 * config offset 0x3C (Interrupt Line — RVVM auto-fills with the PLIC
 * source #). */
#define RVVM_PLIC_BASE         0x0C000000UL
#define RVVM_PLIC_SIZE         0x04000000UL
#define RVVM_PLIC_SRC_LIMIT    64           /* PLIC_SRC_LIMIT in riscv-plic.c */
#define RVVM_PLIC_PRIO_OFF     0x000000U
#define RVVM_PLIC_PENDING_OFF  0x001000U
#define RVVM_PLIC_ENABLE_OFF   0x002000U    /* + ctx*0x80 + word*4 */
#define RVVM_PLIC_ENABLE_STRIDE 0x80U
#define RVVM_PLIC_CTX_OFF      0x200000U    /* + ctx*0x1000 */
#define RVVM_PLIC_CTX_STRIDE   0x1000U
#define RVVM_PLIC_CTX_THRESHOLD 0x000U
#define RVVM_PLIC_CTX_CLAIM     0x004U      /* read = claim, write = complete */

/* ======================================================================
 *  Timer / clocksource  (src/rvvm.c:569 RVVM_OPT_TIME_FREQ)
 * ====================================================================== */

/* RVVM's stock tick rate. Used as the time.c default if time_init()
 * isn't called — but firmware should always read time_hz() instead,
 * after discovering /cpus/timebase-frequency from FDT, so non-default
 * RVVM machines work too. */
#define RVVM_TIME_HZ           10000000ULL    /* mtime ticks at 10 MHz */

/* ======================================================================
 *  PCI device IDs that RVVM emulates
 *  (vendor << 16 | device — match the layout returned by config-space
 *  reads at offset 0x00, where vendor is in the low 16 bits.)
 * ====================================================================== */

#define RVVM_PCI_VENDOR(devid_be) ((uint16_t)((devid_be) & 0xFFFF))
#define RVVM_PCI_DEVICE(devid_be) ((uint16_t)(((devid_be) >> 16) & 0xFFFF))

/* Bochs Display (no VGA) — vendor 0x1234, device 0x1111.
 * src/devices/bochs-display.c lines 254-256. */
#define RVVM_PCI_ID_BOCHS_DISPLAY 0x11111234U

/* C-Media CM8888 HDA controller — vendor 0x13F6, device 0x5011.
 * src/devices/sound-hda.c:21-23. */
#define RVVM_PCI_ID_HDA           0x501113F6U

/* Intel HDA controller register offsets (within BAR0).
 * src/devices/sound-hda.c lines 27-56. */
#define HDA_REG_GCAP              0x00
#define HDA_REG_GCTL              0x08   /* CRST in bit 0 */
#define HDA_REG_STATESTS          0x0E   /* codec discovery bitmap */
#define HDA_REG_CORBLBASE         0x40
#define HDA_REG_CORBUBASE         0x44
#define HDA_REG_CORBWP            0x48
#define HDA_REG_CORBRP            0x4A
#define HDA_REG_CORBCTL           0x4C   /* DMA run = bit 1 */
#define HDA_REG_CORBSIZE          0x4E
#define HDA_REG_RIRBLBASE         0x50
#define HDA_REG_RIRBUBASE         0x54
#define HDA_REG_RIRBWP            0x58   /* write 1<<15 to reset */
#define HDA_REG_RINTCNT           0x5A
#define HDA_REG_RIRBCTL           0x5C
#define HDA_REG_RIRBSIZE          0x5E

/* Codec verbs we use. The Beep Generator widget was added to RVVM in
 * commit a2a4255 on 2026-04-27 (sound-hda.c §7.2.3.8 / §7.3.3.31). */
#define HDA_VERB_GET_PARAMETER       0xF00U
#define HDA_VERB_GET_AMP_GAIN_MUTE   0x00BU
#define HDA_VERB_SET_AMP_GAIN_MUTE   0x003U
#define HDA_VERB_GET_CONV_FMT        0x00AU
#define HDA_VERB_SET_CONV_FMT        0x002U
#define HDA_VERB_GET_CONV_STREAM     0xF06U
#define HDA_VERB_SET_CONV_STREAM     0x706U
#define HDA_VERB_GET_BEEP_GENERATION 0xF0AU
#define HDA_VERB_SET_BEEP_GENERATION 0x70AU

/* HDA output stream descriptor — single output stream at index 1 (the
 * input slot at index 0 is reserved per HDA_PARAM_NO_IN=1). MMIO
 * offset = 0x80 + STREAM_INDEX * 0x20. sound-hda.c §3.3.34, §3.3.35. */
#define RVVM_HDA_OUT_SD_BASE      0xA0U
#define HDA_SD_CTL                0x00   /* 24-bit; bit 1 RUN, bits 23:20 STRM */
#define HDA_SD_STS                0x03
#define HDA_SD_LPIB               0x04   /* RO; bytes consumed within CBL */
#define HDA_SD_CBL                0x08   /* total cyclic buffer length, bytes */
#define HDA_SD_LVI                0x0C   /* last valid BDL index */
#define HDA_SD_FIFOS              0x10
#define HDA_SD_FMT                0x12
#define HDA_SD_BDPL               0x18   /* BDL pointer low; 128-B aligned */
#define HDA_SD_BDPU               0x1C

/* SDnFMT bit packing (§7.3.3.10):
 *   bits 3:0    channels - 1
 *   bits 6:4    BITS code (1 = 16-bit; 0 = 8-bit; 3 = 24-bit; 4 = 32-bit)
 *   bits 10:8   divisor - 1
 *   bits 13:11  multiplier - 1
 *   bit  14     base rate (0 = 48 kHz, 1 = 44.1 kHz)
 *   bit  15     0 = PCM, 1 = non-PCM. */
#define HDA_FMT_16BIT_MONO_48K    0x0010U   /* base=48, mult/div=1, bits=16, ch=1 */
#define HDA_FMT_16BIT_MONO_44K1   0x4010U
#define HDA_FMT_16BIT_MONO_96K    0x0810U   /* base=48, mult=2, div=1 */
#define HDA_FMT_16BIT_MONO_88K2   0x4810U   /* base=44.1, mult=2, div=1 */

/* Stereo variants — channels-1 = 1 in bits 3:0. RVVM's HDA stream
 * worker honours the format register's channel count regardless of
 * what the codec widget caps advertise (see sound-hda.c around the
 * `channels = (fmt & 0xF) + 1` parse), so a stereo stream works even
 * though the C-Media-9880 codec NID 2 currently lacks the STEREO bit
 * — RVVM averages L+R into mono before handing to ALSA. When RVVM
 * eventually exposes a stereo widget, this same fmt will deliver
 * true stereo to the host with no firmware changes. */
#define HDA_FMT_16BIT_STEREO_48K  0x0011U
#define HDA_FMT_16BIT_STEREO_44K1 0x4011U
#define HDA_FMT_16BIT_STEREO_96K  0x0811U
#define HDA_FMT_16BIT_STEREO_88K2 0x4811U

/* AMP_GAIN_MUTE payload bits. */
#define HDA_AMP_OUTPUT            0x8000U
#define HDA_AMP_LEFT              0x2000U
#define HDA_AMP_RIGHT             0x1000U
#define HDA_AMP_MUTE              0x0080U

/* Beep widget on NID 4. Frequency = 48000 / (4 * divider) Hz; range
 * 1..255 covers ~47 Hz to 12 kHz. divider=27 ≈ 444 Hz (close to A4). */
#define RVVM_HDA_BEEP_NID         4
#define RVVM_HDA_BEEP_DIV_440HZ   27

/* Realtek RTL8168 — vendor 0x10EC, device 0x8168.
 * src/devices/rtl8169.c:714-715. */
#define RVVM_PCI_ID_RTL8168       0x816810ECU

/* Toshiba ATA/IDE controller — vendor 0x1179, device 0x0102.
 * src/devices/ata.c:675-676. */
#define RVVM_PCI_ID_ATA           0x01021179U

/* ATA register offsets (within the data BAR — BAR0).
 * src/devices/ata.c lines 33-46. */
#define ATA_REG_DATA              0x00   /* 16-bit DATA, R/W */
#define ATA_REG_ERROR             0x01   /* RO */
#define ATA_REG_NSECT             0x02   /* sector count, 0 = 256 */
#define ATA_REG_LBAL              0x03
#define ATA_REG_LBAM              0x04
#define ATA_REG_LBAH              0x05
#define ATA_REG_DEVICE            0x06   /* 0xE0 | (slave<<4) | LBA[27:24] */
#define ATA_REG_STATUS            0x07   /* RO */
#define ATA_REG_COMMAND           0x07   /* WO */

/* ATA STATUS bits (src/devices/ata.c lines 56-59). */
#define ATA_STATUS_ERR            0x01
#define ATA_STATUS_DRQ            0x08
#define ATA_STATUS_SRV            0x10
#define ATA_STATUS_RDY            0x40
#define ATA_STATUS_BSY            0x80   /* not asserted by RVVM but spec'd */

/* ATA commands we care about. */
#define ATA_CMD_READ_SECTORS      0x20
#define ATA_CMD_IDENTIFY          0xEC

/* SiFive FU740 host bridge — vendor 0xF15E, used as device 0 of bus 0.
 * src/devices/pci-bus.c:878. Always at 00:00.0. */
#define RVVM_PCI_ID_HOST_BRIDGE   0x0000F15EU

/* ======================================================================
 *  Bochs Display registers  (src/devices/bochs-display.c lines 33-67)
 *  All 16-bit. Live in BAR2; VRAM is BAR0 (16 MiB).
 * ====================================================================== */

#define BOCHS_REG_ID            0x0500
#define BOCHS_REG_XRES          0x0502
#define BOCHS_REG_YRES          0x0504
#define BOCHS_REG_BPP           0x0506
#define BOCHS_REG_ENABLE        0x0508
#define BOCHS_REG_BANK          0x050A   /* x86-VGA only, ignored on RVVM */
#define BOCHS_REG_VIRT_WIDTH    0x050C
#define BOCHS_REG_VIRT_HEIGHT   0x050E
#define BOCHS_REG_X_OFFSET      0x0510
#define BOCHS_REG_Y_OFFSET      0x0512
#define BOCHS_REG_VRAM_64K      0x0514   /* read-only, VRAM size in 64K units */

#define BOCHS_VER_ID5           0xB0C5

#define BOCHS_ENABLE            0x01     /* engine on */
#define BOCHS_ENABLE_CAPS       0x02     /* read XRES/YRES = max supported */
#define BOCHS_ENABLE_NOCLR      0x80     /* don't zero VRAM on enable */

#define RVVM_BOCHS_VRAM_SIZE    0x01000000U   /* 16 MiB. src/devices/bochs-display.h:18 */

/* ======================================================================
 *  OpenCores I²C controller  (src/devices/i2c-oc.c lines 36-58)
 * ====================================================================== */

#define I2C_OC_REG_CLKLO        0x00
#define I2C_OC_REG_CLKHI        0x04
#define I2C_OC_REG_CTR          0x08     /* control */
#define I2C_OC_REG_TXRXR        0x0C     /* TX byte (write), RX byte (read) */
#define I2C_OC_REG_CRSR         0x10     /* command (write), status (read) */

#define I2C_OC_CTR_EN           0x80     /* core enable */
#define I2C_OC_CTR_IEN          0x40     /* interrupt enable */

#define I2C_OC_CMD_STA          0x80     /* generate (repeated) start */
#define I2C_OC_CMD_STO          0x40     /* generate stop */
#define I2C_OC_CMD_RD           0x20     /* read from slave */
#define I2C_OC_CMD_WR           0x10     /* write to slave */
#define I2C_OC_CMD_NACK         0x08     /* NACK on read (1=NACK) */
#define I2C_OC_CMD_IACK         0x01     /* clear pending IRQ */

#define I2C_OC_STA_NACK         0x80     /* slave returned NACK */
#define I2C_OC_STA_BSY          0x40     /* bus busy */
#define I2C_OC_STA_AL           0x20     /* arbitration lost */
#define I2C_OC_STA_TIP          0x02     /* transfer in progress */
#define I2C_OC_STA_IF           0x01     /* IRQ pending */

/* ======================================================================
 *  HID-over-I²C  (src/devices/i2c-hid.c lines 20-34)
 *  Register addresses are 16-bit values that the host writes to the
 *  device to select a register. RVVM hardcodes these constants. */

#define I2C_HID_REG_DESC        0x0001   /* HID descriptor (30 bytes) */
#define I2C_HID_REG_REPORT      0x0002   /* report descriptor (per cap) */
#define I2C_HID_REG_INPUT       0x0003   /* current input report */
#define I2C_HID_REG_OUTPUT      0x0004   /* output report */
#define I2C_HID_REG_COMMAND     0x0005   /* command register */
#define I2C_HID_REG_DATA        0x0006   /* data register */

/* Command verbs (low nibble of byte 1 of the command word). */
#define I2C_HID_CMD_RESET       0x01
#define I2C_HID_CMD_GET_REPORT  0x02
#define I2C_HID_CMD_SET_REPORT  0x03
#define I2C_HID_CMD_SET_POWER   0x08

/* I²C address autoallocation: i2c-hid devices auto-attach at 0x08, 0x09…
 * (src/devices/i2c-oc.c:264). RVVM creates HID keyboard then HID mouse,
 * then mouse-tablet variant — three devices total. So:
 *   0x08  HID keyboard
 *   0x09  HID mouse (relative)
 *   0x0A  HID mouse (tablet/absolute)  */
#define RVVM_I2C_HID_KEYBOARD   0x08
#define RVVM_I2C_HID_MOUSE      0x09
#define RVVM_I2C_HID_TABLET     0x0A

/* Keyboard report layout (src/devices/hid-keyboard.c):
 *   [0..1]  total report length, little-endian = 10
 *   [2]     modifier byte (LCtrl/LShift/LAlt/LMeta/RCtrl/RShift/RAlt/RMeta)
 *   [3]     reserved (0)
 *   [4..9]  up to 6 USB-HID usage codes for currently-pressed keys */
#define RVVM_HID_KB_REPORT_LEN  10

/* USB HID modifier-byte bit positions (matches HID_KEY_LEFTCTRL..RIGHTMETA
 * encoded into the status byte of the boot keyboard descriptor). */
#define HID_MOD_LCTRL           0x01
#define HID_MOD_LSHIFT          0x02
#define HID_MOD_LALT            0x04
#define HID_MOD_LMETA           0x08
#define HID_MOD_RCTRL           0x10
#define HID_MOD_RSHIFT          0x20
#define HID_MOD_RALT            0x40
#define HID_MOD_RMETA           0x80
