/* OpenCores I²C master driver. Polling-only — RVVM completes every
 * transaction synchronously inside the bus mmio_write callback, so the
 * IF status bit is set the moment we issue the command. */

#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Initialise. Pass MMIO base discovered via FDT (compatible
 * "opencores,i2c-ocores"); 0 falls back to RVVM's default. */
void i2c_init(uintptr_t base);

/* START + addr<<1|0 + write `len` bytes + STOP. Returns true if every
 * byte was ACKed by the slave. */
bool i2c_write(uint8_t addr, const uint8_t *data, size_t len);

/* START + addr<<1|0 + write `wlen` bytes + REPEATED-START + addr<<1|1 +
 * read `rlen` bytes (NACK on the last) + STOP. The HID-over-I²C "select
 * register, then drain its data" pattern uses this exclusively. */
bool i2c_write_then_read(uint8_t addr,
                         const uint8_t *wdata, size_t wlen,
                         uint8_t *rdata, size_t rlen);
