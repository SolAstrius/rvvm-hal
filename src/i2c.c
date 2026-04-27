#include "i2c.h"
#include "rvvm.h"
#include "mmio.h"
#include "uart.h"

static uintptr_t i2c_base = RVVM_I2C_OC_BASE;

/* Trace the next N i2c_cmd calls to UART. Useful for debugging. */
static int i2c_trace = 0;

static inline uint8_t r(uint32_t off)            { return mmio_r8(i2c_base + off); }
static inline void    w(uint32_t off, uint8_t v) { mmio_w8(i2c_base + off, v); }

void i2c_init(uintptr_t base) {
    if (base) i2c_base = base;
    /* Disable IRQs (we poll), enable the core. */
    w(I2C_OC_REG_CTR, I2C_OC_CTR_EN);

    /* Sanity probe: read CTR back and dump it, plus initial status,
     * so we can tell whether the controller is even listening. */
    uart_printf("i2c: ctr_w=%x ctr_r=%x crsr=%x  (expect ctr=0x80)\n",
                (uint64_t)I2C_OC_CTR_EN,
                (uint64_t)r(I2C_OC_REG_CTR),
                (uint64_t)r(I2C_OC_REG_CRSR));

    /* Trace the first 6 cmds so we can see exactly where address-write
     * goes wrong (if it does). 6 = STA+WR addr, WR reg.lo, WR reg.hi,
     * STA+WR addr_r, RD byte 0, STO. */
    i2c_trace = 6;
}

static bool i2c_cmd(uint8_t cmd) {
    w(I2C_OC_REG_CRSR, cmd | I2C_OC_CMD_IACK);
    uint8_t  s = 0;
    int      spun;
    for (spun = 0; spun < 1000; spun++) {
        s = r(I2C_OC_REG_CRSR);
        if (s & I2C_OC_STA_IF) break;
    }
    if (i2c_trace > 0) {
        uart_printf("i2c: cmd=%x status=%x spun=%u -> %s\n",
                    (uint64_t)cmd, (uint64_t)s, (uint64_t)spun,
                    spun >= 1000           ? "TIMEOUT"
                    : (s & I2C_OC_STA_NACK) ? "NACK"
                    :                          "ACK");
        i2c_trace--;
    }
    if (spun >= 1000) return false;
    return !(s & I2C_OC_STA_NACK);
}

bool i2c_write(uint8_t addr, const uint8_t *data, size_t len) {
    w(I2C_OC_REG_TXRXR, (uint8_t)((addr << 1) | 0));
    if (!i2c_cmd(I2C_OC_CMD_STA | I2C_OC_CMD_WR)) goto fail;
    for (size_t i = 0; i < len; i++) {
        w(I2C_OC_REG_TXRXR, data[i]);
        if (!i2c_cmd(I2C_OC_CMD_WR)) goto fail;
    }
    return i2c_cmd(I2C_OC_CMD_STO);
fail:
    /* Always drive STOP to release the bus, even on NACK. */
    i2c_cmd(I2C_OC_CMD_STO);
    return false;
}

bool i2c_write_then_read(uint8_t addr,
                         const uint8_t *wdata, size_t wlen,
                         uint8_t *rdata, size_t rlen) {
    /* Write phase. */
    w(I2C_OC_REG_TXRXR, (uint8_t)((addr << 1) | 0));
    if (!i2c_cmd(I2C_OC_CMD_STA | I2C_OC_CMD_WR)) goto fail;
    for (size_t i = 0; i < wlen; i++) {
        w(I2C_OC_REG_TXRXR, wdata[i]);
        if (!i2c_cmd(I2C_OC_CMD_WR)) goto fail;
    }

    /* Repeated start with read direction. */
    w(I2C_OC_REG_TXRXR, (uint8_t)((addr << 1) | 1));
    if (!i2c_cmd(I2C_OC_CMD_STA | I2C_OC_CMD_WR)) goto fail;

    /* Read phase. NACK on the final byte signals end-of-read to slave. */
    for (size_t i = 0; i < rlen; i++) {
        uint8_t cmd = I2C_OC_CMD_RD;
        if (i + 1 == rlen) cmd |= I2C_OC_CMD_NACK;
        if (!i2c_cmd(cmd)) goto fail;
        rdata[i] = r(I2C_OC_REG_TXRXR);
    }

    return i2c_cmd(I2C_OC_CMD_STO);
fail:
    i2c_cmd(I2C_OC_CMD_STO);
    return false;
}
