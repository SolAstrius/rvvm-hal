/* GCC/Clang lower zero-initialised arrays, struct copies, and various
 * other patterns into implicit memset/memcpy/memmove calls — even with
 * -nostdlib, the compiler still emits them and expects them at link
 * time. We supply word-aligned implementations: head/tail byte loops to
 * align, then 8-byte stores in the body. ~5× over a naive byte loop on
 * the 49 KB Speccy snapshot path.
 *
 * The word path is gated on src/dst sharing 8-byte alignment — RVVM
 * permits unaligned access but real RISC-V hardware (where this also
 * compiles) often traps. In practice the firmware's hot buffers
 * (vm.mem in BSS, NVMe disk_buf with aligned attribute) all share
 * page alignment, so the word path is what runs. */

#include <stdint.h>
#include <stddef.h>

/* Block the compiler from pattern-matching these bodies into self-calls.
 * Without it, -O2+ recognises the byte/word-copy idiom and emits
 * `call memcpy` inside memcpy itself; the firmware then trampolines
 * forever. Belt-and-braces: the Makefile also passes -fno-builtin and
 * -fno-tree-loop-distribute-patterns for this TU. */
#if defined(__clang__)
#  define NO_BUILTIN __attribute__((no_builtin("memcpy", "memmove", "memset")))
#else
#  define NO_BUILTIN
#endif

NO_BUILTIN
void *memset(void *dst, int c, size_t n) {
    uint8_t *d = dst;
    uint8_t  b = (uint8_t)c;
    while (n && ((uintptr_t)d & 7)) { *d++ = b; n--; }
    uint64_t  w  = (uint64_t)b * 0x0101010101010101ULL;
    uint64_t *dw = (uint64_t *)d;
    while (n >= 8) { *dw++ = w; n -= 8; }
    d = (uint8_t *)dw;
    while (n--) *d++ = b;
    return dst;
}

NO_BUILTIN
void *memcpy(void *dst, const void *src, size_t n) {
    uint8_t       *d = dst;
    const uint8_t *s = src;
    /* Word path requires src/dst to share alignment — otherwise the
     * 8-byte loads/stores would be unaligned, which traps on most
     * real RISC-V parts. */
    if ((((uintptr_t)d ^ (uintptr_t)s) & 7) == 0) {
        while (n && ((uintptr_t)d & 7)) { *d++ = *s++; n--; }
        uint64_t       *dw = (uint64_t *)d;
        const uint64_t *sw = (const uint64_t *)s;
        while (n >= 8) { *dw++ = *sw++; n -= 8; }
        d = (uint8_t *)dw;
        s = (const uint8_t *)sw;
    }
    while (n--) *d++ = *s++;
    return dst;
}

NO_BUILTIN
void *memmove(void *dst, const void *src, size_t n) {
    uint8_t       *d = dst;
    const uint8_t *s = src;
    if (d == s || n == 0) return dst;
    /* Non-overlapping forward case is just memcpy. The dangerous case
     * is dst > src with overlap — copy backward to avoid clobbering
     * source bytes before they're read. */
    if (d < s) return memcpy(dst, src, n);
    d += n; s += n;
    if ((((uintptr_t)d ^ (uintptr_t)s) & 7) == 0) {
        while (n && ((uintptr_t)d & 7)) { *--d = *--s; n--; }
        uint64_t       *dw = (uint64_t *)d;
        const uint64_t *sw = (const uint64_t *)s;
        while (n >= 8) { *--dw = *--sw; n -= 8; }
        d = (uint8_t *)dw;
        s = (const uint8_t *)sw;
    }
    while (n--) *--d = *--s;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *x = a, *y = b;
    while (n--) {
        if (*x != *y) return (int)*x - (int)*y;
        x++; y++;
    }
    return 0;
}
