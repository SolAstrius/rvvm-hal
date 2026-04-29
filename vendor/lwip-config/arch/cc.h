/* lwIP compiler / platform abstractions for rvvm-hal.
 *
 * lwIP includes this from sys.h; we satisfy the few bits it needs
 * (byte-order, packed-struct, diag/assert hooks). Everything else
 * comes from picolibc via -isystem on the firmware build. */

#pragma once
#include <stdint.h>
#include <stdio.h>     /* picolibc's printf for LWIP_PLATFORM_DIAG */
#include <sys/types.h> /* ssize_t — picolibc defines as long, lwIP would otherwise redefine as int */
#include <limits.h>
#ifndef SSIZE_MAX
#define SSIZE_MAX  LONG_MAX  /* tells lwIP arch.h not to redefine ssize_t */
#endif

/* lwIP packs its protocol structs with these macros. zig cc + rv64
 * accept GCC-style attributes. */
#define PACK_STRUCT_FIELD(x)    x
#define PACK_STRUCT_STRUCT      __attribute__((packed))
#define PACK_STRUCT_BEGIN
#define PACK_STRUCT_END

/* Diag goes to UART via picolibc's printf. */
#define LWIP_PLATFORM_DIAG(x)   do { printf x; } while (0)

/* Assert hook — with LWIP_NOASSERT=1 in lwipopts.h this is unused,
 * but lwIP still references the symbol. Print + halt. */
#define LWIP_PLATFORM_ASSERT(x) \
    do { printf("lwIP ASSERT: %s\n", x); for (;;) __asm__ volatile ("wfi"); } while (0)

/* Random number for protocol-level randomness (TCP ISN, etc).
 * Picolibc's rand returns 0..RAND_MAX; squeeze into 32 bits. */
#include <stdlib.h>
#define LWIP_RAND() ((uint32_t)rand())
