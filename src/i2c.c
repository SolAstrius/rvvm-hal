#include "i2c.h"
#include "rvvm.h"
#include "mmio.h"
#include "uart.h"

static uintptr_t i2c_base = RVVM_I2C_OC_BASE;

static inline uint8_t r(uint32_t off)            { return mmio_r8(i2c_base + off); }
static inline void    w(uint32_t off, uint8_t v) { mmio_w8(i2c_base + off, v); }

void i2c_init(uintptr_t base) {
    if (base) i2c_base = base;
    /* Disable IRQs (we poll), enable the core. */
    w(I2C_OC_REG_CTR, I2C_OC_CTR_EN);
}

/* Issue a CMD byte, wait for the IF (transfer-complete) status bit.
 * Returns true if the slave ACKed (status NACK bit clear).
 *
 * RVVM processes the cmd synchronously and sets IF immediately, so the
 * spin loop is more for portability than RVVM-side need. The IACK bit
 * we OR into every cmd clears any prior IF flag before the new
 * transaction begins (i2c-oc.c handles IACK before STA/WR/RD/STO). */
static bool i2c_cmd(uint8_t cmd) {
    w(I2C_OC_REG_CRSR, cmd | I2C_OC_CMD_IACK);
    for (int i = 0; i < 1000; i++) {
        uint8_t s = r(I2C_OC_REG_CRSR);
        if (s & I2C_OC_STA_IF) return !(s & I2C_OC_STA_NACK);
    }
    uart_puts("i2c: timeout\n");
    return false;
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
