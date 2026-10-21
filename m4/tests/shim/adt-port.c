/* The loader's ADT reader lives in the Rust library; this is the same minimal C port the host e2e test uses
 * (struct layout + /arm-io ranges translation), used here so the real shim runs against the real ADT of this Mac. */
#include <stdint.h>
#include <stddef.h>
extern void *adt;
static size_t strnlen_(const char *s, size_t n) { size_t k = 0; while (k < n && s[k]) k++; return k; }
static int strncmp_(const char *a, const char *b, size_t n) { for (size_t i = 0; i < n; i++) { if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i]; if (!a[i]) return 0; } return 0; }
static int memcmp_(const void *a, const void *b, size_t n) { const uint8_t *x = a, *y = b; for (size_t i = 0; i < n; i++) if (x[i] != y[i]) return x[i] - y[i]; return 0; }
#define strnlen strnlen_
#define strncmp strncmp_
#define memcmp memcmp_
/* ---- ADT reader (the loader's Rust one, minimal port incl. reg translation) ---- */
#define ADT ((const uint8_t *)adt)
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

struct prop { const uint8_t *val; uint32_t size; };
static size_t node_end(size_t off)
{
    uint32_t np = rd32(ADT + off), nc = rd32(ADT + off + 4);
    size_t p = off + 8;
    for (uint32_t i = 0; i < np; i++) { uint32_t sz = rd32(ADT + p + 32) & 0x7fffffff; p += 36 + ((sz + 3) & ~3u); }
    for (uint32_t i = 0; i < nc; i++) p = node_end(p);
    return p;
}
static int node_prop(size_t off, const char *name, struct prop *out)
{
    uint32_t np = rd32(ADT + off);
    size_t p = off + 8;
    for (uint32_t i = 0; i < np; i++) {
        uint32_t sz = rd32(ADT + p + 32) & 0x7fffffff;
        if (!strncmp((const char *)ADT + p, name, 32)) { out->val = ADT + p + 36; out->size = sz; return 0; }
        p += 36 + ((sz + 3) & ~3u);
    }
    return -1;
}
static int name_eq(size_t node, const char *comp, size_t n)
{
    struct prop pr;
    if (node_prop(node, "name", &pr)) return 0;
    size_t len = strnlen((const char *)pr.val, pr.size);
    if (len == n && !memcmp(pr.val, comp, n)) return 1;
    return 0;
}
int adt_path_offset_trace(const void *adt_, const char *path, int *offsets)
{
    (void)adt_;
    size_t cur = 0;
    int depth = 0;
    const char *p = path;
    while (*p == '/') p++;
    while (*p) {
        const char *e = p;
        while (*e && *e != '/') e++;
        uint32_t np = rd32(ADT + cur), nc = rd32(ADT + cur + 4);
        size_t q = cur + 8;
        for (uint32_t i = 0; i < np; i++) { uint32_t sz = rd32(ADT + q + 32) & 0x7fffffff; q += 36 + ((sz + 3) & ~3u); }
        int found = 0;
        for (uint32_t i = 0; i < nc; i++) {
            if (name_eq(q, p, (size_t)(e - p))) { cur = q; found = 1; break; }
            q = node_end(q);
        }
        if (!found) return -1;
        if (depth < 7) offsets[depth] = (int)cur;
        depth++;
        p = e;
        while (*p == '/') p++;
    }
    if (depth < 8) offsets[depth] = 0;
    return (int)cur;
}
const void *adt_getprop(const void *adt_, int node, const char *name, uint32_t *len)
{
    (void)adt_;
    struct prop pr;
    if (node_prop((size_t)node, name, &pr)) return NULL;
    if (len) *len = pr.size;
    return pr.val;
}
static uint64_t cells(const uint8_t *p, unsigned n)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < n; i++) v |= (uint64_t)rd32(p + 4 * i) << (32 * i);
    return v;
}
static uint64_t cells_be_chunks(const uint8_t *p, unsigned n) /* get_cells_u8: reverse chunks of 4 */
{
    uint64_t v = 0;
    for (int i = (int)n - 1; i >= 0; i--) v = (v << 32) | rd32(p + 4 * i);
    return v;
}
int adt_get_reg(const void *adt_, int *path, const char *prop, int idx, uint64_t *addr, uint64_t *size)
{
    (void)adt_;
    int nodes[9], n = 0;
    while (n < 8 && path[n]) { nodes[n] = path[n]; n++; }
    if (!n) return -1;
    int cursor = n - 1;
    size_t node = (size_t)nodes[cursor];
    size_t parent = cursor > 0 ? (size_t)nodes[cursor - 1] : 0;
    struct prop pr, ac, sc;
    if (node_prop(parent, "#address-cells", &ac) || node_prop(parent, "#size-cells", &sc)) return -2;
    unsigned addr_cells = rd32(ac.val), size_cells = rd32(sc.val);
    if (addr_cells < 1 || addr_cells > 2 || size_cells > 2) return -3;
    if (node_prop(node, prop, &pr) || !pr.size) return -4;
    if (pr.size < (uint32_t)(idx + 1) * (addr_cells + size_cells) * 4) return -5;
    const uint8_t *a = pr.val + (size_t)idx * (addr_cells + size_cells) * 4;
    uint64_t ad = cells(a, addr_cells), sz = cells(a + 4 * addr_cells, size_cells);
    while (parent != 0) {
        cursor--;
        node = parent;
        parent = cursor > 0 ? (size_t)nodes[cursor - 1] : 0;
        struct prop rg;
        if (node_prop(node, "ranges", &rg)) break;
        struct prop pc;
        if (node_prop(parent, "#address-cells", &pc)) return -6;
        unsigned paddr = rd32(pc.val);
        unsigned entry = 4 * (paddr + addr_cells + size_cells);
        for (uint32_t o = 0; o + entry <= rg.size; o += entry) {
            const uint8_t *r = rg.val + o;
            uint64_t ca = cells_be_chunks(r, addr_cells), pa = cells_be_chunks(r + 4 * addr_cells, paddr),
                     cs = cells_be_chunks(r + 4 * (addr_cells + paddr), size_cells);
            if (ad >= ca && ad + sz <= ca + cs) { ad = ad - ca + pa; break; }
        }
        if (node_prop(parent, "#size-cells", &sc)) return -7;
        size_cells = rd32(sc.val);
        addr_cells = paddr;
    }
    *addr = ad;
    if (size) *size = sz;
    return 0;
}

