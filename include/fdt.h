/* Tiny Flattened Device Tree walker. Pure index-into-blob; no malloc.
 *
 * Spec: devicetree.org Specification Release v0.4 (DTSpec). DTB layout
 * is well-defined and stable since 2003-ish.
 *
 * We parse what RVVM emits — version 17 DTBs with the standard
 * 4-token vocabulary (BEGIN_NODE/END_NODE/PROP/NOP/END), 8-byte
 * header alignment, big-endian everything. */

#pragma once
#include <stdint.h>
#include <stdbool.h>

/* DTB tokens (devicetree spec §5.4). */
#define FDT_BEGIN_NODE 0x00000001U
#define FDT_END_NODE   0x00000002U
#define FDT_PROP       0x00000003U
#define FDT_NOP        0x00000004U
#define FDT_END        0x00000009U
#define FDT_MAGIC      0xD00DFEEDU

typedef struct {
    const uint8_t *struct_blob;   /* points at start of structure block */
    const char    *strings;       /* points at start of strings block */
    uint32_t       struct_size;   /* bytes in structure block */
} fdt_t;

/* Initialise from a raw DTB pointer. Validates magic + version, fills
 * the struct/strings pointers. Returns false if the blob isn't a DTB. */
bool fdt_init(fdt_t *fdt, const void *blob);

/* Walk one BEGIN_NODE forward from `off`. Pass `off=0` to start at the
 * first node (root). Returns the byte offset (within the struct blob)
 * of the FDT_BEGIN_NODE token of the next node found, or UINT32_MAX
 * if no more nodes. The order is depth-first (parent → children → siblings),
 * which is exactly the DTB file order. */
uint32_t fdt_next_node(const fdt_t *fdt, uint32_t off);

/* Get the name of a node (the bytes after its FDT_BEGIN_NODE token).
 * The pointer is into the struct blob; valid as long as `fdt` is. */
const char *fdt_node_name(const fdt_t *fdt, uint32_t node_off);

/* Look up a property on a single node (does NOT recurse into children).
 * Returns the property's data pointer and writes its length to *out_len.
 * Returns NULL if the property is absent. */
const void *fdt_node_prop(const fdt_t *fdt, uint32_t node_off,
                          const char *name, uint32_t *out_len);

/* Find the first node anywhere in the tree whose `compatible` property
 * contains the target string (compatible is a list of null-terminated
 * strings; we match if any of them equals `compat`). Returns the node
 * offset, or UINT32_MAX if no such node exists. */
uint32_t fdt_find_compatible(const fdt_t *fdt, const char *compat);

/* Read a big-endian uint32 / uint64 from an arbitrary aligned offset
 * within a property's data. */
uint32_t fdt_read_be32(const void *data, uint32_t off);
uint64_t fdt_read_be64(const void *data, uint32_t off);

/* Read the i-th (address, size) entry from a node's `reg` property,
 * assuming #address-cells=2 and #size-cells=2 (RVVM's /soc default).
 * Returns false if the property is missing or the index is out of range. */
bool fdt_node_reg64(const fdt_t *fdt, uint32_t node_off, uint32_t idx,
                    uint64_t *out_addr, uint64_t *out_size);
