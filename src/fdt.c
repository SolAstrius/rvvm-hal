#include "fdt.h"
#include "uart.h"
#include <stddef.h>   /* NULL */

/* DTB header — first 40 bytes of every blob, all fields big-endian. */
struct fdt_header {
    uint32_t magic;
    uint32_t totalsize;
    uint32_t off_dt_struct;
    uint32_t off_dt_strings;
    uint32_t off_mem_rsvmap;
    uint32_t version;
    uint32_t last_comp_version;
    uint32_t boot_cpuid_phys;
    uint32_t size_dt_strings;
    uint32_t size_dt_struct;
};

uint32_t fdt_read_be32(const void *data, uint32_t off) {
    const uint8_t *p = (const uint8_t *)data + off;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

uint64_t fdt_read_be64(const void *data, uint32_t off) {
    return ((uint64_t)fdt_read_be32(data, off) << 32)
         | fdt_read_be32(data, off + 4);
}

static bool str_eq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == 0 && *b == 0;
}

bool fdt_init(fdt_t *fdt, const void *blob) {
    const struct fdt_header *h = (const struct fdt_header *)blob;
    if (fdt_read_be32(h, 0) != FDT_MAGIC) return false;
    if (fdt_read_be32(h, 20) < 17)        return false;     /* last_comp_version */

    uint32_t off_struct  = fdt_read_be32(h, 8);
    uint32_t off_strings = fdt_read_be32(h, 12);
    fdt->struct_blob = (const uint8_t *)blob + off_struct;
    fdt->strings     = (const char    *)blob + off_strings;
    fdt->struct_size = fdt_read_be32(h, 36);                /* size_dt_struct */
    return true;
}

const char *fdt_node_name(const fdt_t *fdt, uint32_t node_off) {
    /* Node name lives right after the FDT_BEGIN_NODE token. */
    return (const char *)(fdt->struct_blob + node_off + 4);
}

/* Skip over a node's name string + trailing NUL, padded to 4 bytes.
 * Returns the offset of the first token after the name. */
static uint32_t skip_name(const fdt_t *fdt, uint32_t off) {
    /* off points at FDT_BEGIN_NODE; name starts at off+4. */
    uint32_t p = off + 4;
    while (fdt->struct_blob[p]) p++;             /* find NUL */
    p++;                                          /* include NUL */
    return (p + 3) & ~3U;                         /* 4-byte align */
}

/* Skip over a FDT_PROP token's payload. `off` points at the token itself.
 * Returns the offset of the next token. */
static uint32_t skip_prop(const fdt_t *fdt, uint32_t off) {
    uint32_t len = fdt_read_be32(fdt->struct_blob, off + 4);
    /* token + len + nameoff + payload, payload padded to 4 bytes. */
    return off + 12 + ((len + 3) & ~3U);
}

uint32_t fdt_next_node(const fdt_t *fdt, uint32_t off) {
    /* Walk forward looking for FDT_BEGIN_NODE. Caller passes the offset
     * to start scanning; to iterate, pass `prev_offset + 4` so we step
     * past the BEGIN_NODE token already consumed. The loop handles
     * intermediate FDT_PROP / FDT_NOP / FDT_END_NODE tokens — the only
     * thing it WON'T survive is being called with `off` mid-payload. */
    while (off < fdt->struct_size) {
        uint32_t tok = fdt_read_be32(fdt->struct_blob, off);
        switch (tok) {
        case FDT_BEGIN_NODE: return off;
        case FDT_END_NODE:
        case FDT_NOP:        off += 4; break;
        case FDT_PROP:       off = skip_prop(fdt, off); break;
        case FDT_END:
        default:             return UINT32_MAX;
        }
    }
    return UINT32_MAX;
}

const void *fdt_node_prop(const fdt_t *fdt, uint32_t node_off,
                          const char *name, uint32_t *out_len) {
    /* Walk just this node's own properties — stop on entering a child
     * (FDT_BEGIN_NODE) or leaving this node (FDT_END_NODE). */
    uint32_t off = skip_name(fdt, node_off);
    while (off < fdt->struct_size) {
        uint32_t tok = fdt_read_be32(fdt->struct_blob, off);
        if (tok == FDT_BEGIN_NODE || tok == FDT_END_NODE || tok == FDT_END) break;
        if (tok == FDT_NOP) { off += 4; continue; }
        if (tok == FDT_PROP) {
            uint32_t len     = fdt_read_be32(fdt->struct_blob, off + 4);
            uint32_t nameoff = fdt_read_be32(fdt->struct_blob, off + 8);
            const char *pname = fdt->strings + nameoff;
            if (str_eq(pname, name)) {
                if (out_len) *out_len = len;
                return fdt->struct_blob + off + 12;
            }
            off += 12 + ((len + 3) & ~3U);
            continue;
        }
        break;   /* malformed */
    }
    return NULL;
}

uint32_t fdt_find_compatible(const fdt_t *fdt, const char *compat) {
    /* Walk every token in the structure block. On every BEGIN_NODE,
     * check the node's `compatible` property; on hit, return that
     * node's offset. Otherwise advance past the BEGIN_NODE+name to
     * the first token of the body (so child PROPs are next), letting
     * the loop's natural FDT_PROP / FDT_BEGIN_NODE handling recurse
     * implicitly into children. */
    uint32_t off = 0;
    while (off < fdt->struct_size) {
        uint32_t tok = fdt_read_be32(fdt->struct_blob, off);
        switch (tok) {
        case FDT_BEGIN_NODE: {
            uint32_t len = 0;
            const char *list = fdt_node_prop(fdt, off, "compatible", &len);
            if (list) {
                uint32_t pos = 0;
                while (pos < len) {
                    if (str_eq(list + pos, compat)) return off;
                    while (pos < len && list[pos]) pos++;
                    pos++;   /* skip NUL */
                }
            }
            off = skip_name(fdt, off);   /* descend into node body */
            break;
        }
        case FDT_END_NODE:
        case FDT_NOP:        off += 4; break;
        case FDT_PROP:       off = skip_prop(fdt, off); break;
        case FDT_END:
        default:             return UINT32_MAX;
        }
    }
    return UINT32_MAX;
}

bool fdt_node_reg64(const fdt_t *fdt, uint32_t node_off, uint32_t idx,
                    uint64_t *out_addr, uint64_t *out_size) {
    uint32_t len = 0;
    const void *reg = fdt_node_prop(fdt, node_off, "reg", &len);
    if (!reg) return false;

    /* Hardcoded #address-cells=2, #size-cells=2 — the /soc default. */
    const uint32_t entry_bytes = 16;
    if ((idx + 1) * entry_bytes > len) return false;

    uint32_t off = idx * entry_bytes;
    if (out_addr) *out_addr = fdt_read_be64(reg, off);
    if (out_size) *out_size = fdt_read_be64(reg, off + 8);
    return true;
}
