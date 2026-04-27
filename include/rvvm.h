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

/* ======================================================================
 *  Timer / clocksource  (src/rvvm.c:569 RVVM_OPT_TIME_FREQ)
 * ====================================================================== */

#define RVVM_TIME_HZ           10000000ULL    /* mtime ticks at 10 MHz */
#define RVVM_TICKS_PER_FRAME   (RVVM_TIME_HZ / 60)   /* ~166 666 / frame */

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
