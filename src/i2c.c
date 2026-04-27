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

/* STOP is fire-and-forget — there's no slave ACK for it. RVVM (per
 * the OpenCores semantics) sets the ACK status bit (= NACK indicator)
 * at the top of every CRSR write and clears it only on successful WR
 * or RD. STO doesn't touch ACK, so the bit stays set after STOP and
 * a naive `if (!i2c_cmd(STO))` reads as NACK and drops the valid data
 * the read phase already collected. Just issue STO and ignore. */
static void i2c_stop(void) { i2c_cmd(I2C_OC_CMD_STO); }

bool i2c_write(uint8_t addr, const uint8_t *data, size_t len) {
    bool ok = false;
    w(I2C_OC_REG_TXRXR, (uint8_t)((addr << 1) | 0));
    if (!i2c_cmd(I2C_OC_CMD_STA | I2C_OC_CMD_WR)) goto out;
    for (size_t i = 0; i < len; i++) {
        w(I2C_OC_REG_TXRXR, data[i]);
        if (!i2c_cmd(I2C_OC_CMD_WR)) goto out;
    }
    ok = true;
out:
    i2c_stop();
    return ok;
}

bool i2c_write_then_read(uint8_t addr,
                         const uint8_t *wdata, size_t wlen,
                         uint8_t *rdata, size_t rlen) {
    bool ok = false;

    /* Write phase. */
    w(I2C_OC_REG_TXRXR, (uint8_t)((addr << 1) | 0));
    if (!i2c_cmd(I2C_OC_CMD_STA | I2C_OC_CMD_WR)) goto out;
    for (size_t i = 0; i < wlen; i++) {
        w(I2C_OC_REG_TXRXR, wdata[i]);
        if (!i2c_cmd(I2C_OC_CMD_WR)) goto out;
    }

    /* Repeated start with read direction. */
    w(I2C_OC_REG_TXRXR, (uint8_t)((addr << 1) | 1));
    if (!i2c_cmd(I2C_OC_CMD_STA | I2C_OC_CMD_WR)) goto out;

    /* Read phase. NACK on the final byte signals end-of-read to slave. */
    for (size_t i = 0; i < rlen; i++) {
        uint8_t cmd = I2C_OC_CMD_RD;
        if (i + 1 == rlen) cmd |= I2C_OC_CMD_NACK;
        if (!i2c_cmd(cmd)) goto out;
        rdata[i] = r(I2C_OC_REG_TXRXR);
    }
    ok = true;

out:
    i2c_stop();
    return ok;
}
