/* MMIO accessors. RISC-V is strongly-ordered for normal memory but
 * MMIO regions need volatile to keep the compiler from reordering or
 * elimintating accesses. For ordering between MMIO and DRAM, callers
 * should use `fence` explicitly via mmio_barrier(). */

#pragma once
#include <stdint.h>

static inline uint8_t  mmio_r8 (uintptr_t a) { return *(volatile uint8_t  *)a; }
static inline uint16_t mmio_r16(uintptr_t a) { return *(volatile uint16_t *)a; }
static inline uint32_t mmio_r32(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline uint64_t mmio_r64(uintptr_t a) { return *(volatile uint64_t *)a; }

static inline void mmio_w8 (uintptr_t a, uint8_t  v) { *(volatile uint8_t  *)a = v; }
static inline void mmio_w16(uintptr_t a, uint16_t v) { *(volatile uint16_t *)a = v; }
static inline void mmio_w32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static inline void mmio_w64(uintptr_t a, uint64_t v) { *(volatile uint64_t *)a = v; }

static inline void mmio_barrier(void) {
    __asm__ volatile ("fence iorw, iorw" ::: "memory");
}
