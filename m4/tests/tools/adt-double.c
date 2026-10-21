/* Host double of the loader ADT reader (the loader links the Rust one): the real ADT binary, m1n1 adt.c semantics. */
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include "adt.h"
void *adt;
static uint32_t adt_total;
void adt_double_init(void *p, uint32_t n) { adt = p; adt_total = n; }
u32 adt_get_size(void) { return adt_total; }
int adt_check_header(const void *a) { return a == adt && adt_total >= 8 ? 0 : -1; }
static uint32_t pad4(uint32_t n) { return (n + 3) & ~3u; }
static const struct adt_node_hdr *hdr(const void *a, int off) { return (const struct adt_node_hdr *)((const uint8_t *)a + off); }
int adt_get_property_count(const void *a, int off) { return off < 0 ? -1 : (int)hdr(a, off)->property_count; }
int adt_first_property_offset(const void *a, int off) { return off < 0 ? -1 : off + 8; }
int adt_next_property_offset(const void *a, int off)
{
    const struct adt_property *p = ADT_PROP(a, off);
    return off + 36 + (int)pad4(p->size & 0x7fffffff);
}
const struct adt_property *adt_get_property_by_offset(const void *a, int off) { return ADT_PROP(a, off); }
static int after_props(const void *a, int off)
{
    int o = adt_first_property_offset(a, off);
    for (uint32_t i = hdr(a, off)->property_count; i; i--) o = adt_next_property_offset(a, o);
    return o;
}
static int skip_node(const void *a, int off)
{
    int o = after_props(a, off);
    for (uint32_t i = hdr(a, off)->child_count; i; i--) o = skip_node(a, o);
    return o;
}
int adt_get_child_count(const void *a, int off) { return off < 0 ? -1 : (int)hdr(a, off)->child_count; }
int adt_first_child_offset(const void *a, int off) { return after_props(a, off); }
int adt_next_sibling_offset(const void *a, int off) { return skip_node(a, off); }
const void *adt_getprop_by_offset(const void *a, int off, const char **namep, u32 *lenp)
{
    const struct adt_property *p = ADT_PROP(a, off);
    if (namep) *namep = p->name;
    if (lenp) *lenp = p->size & 0x7fffffff;
    return p->value;
}
const struct adt_property *adt_get_property(const void *a, int node, const char *name)
{
    int o = adt_first_property_offset(a, node);
    for (uint32_t i = hdr(a, node)->property_count; i; i--, o = adt_next_property_offset(a, o))
        if (!strncmp(ADT_PROP(a, o)->name, name, 32)) return ADT_PROP(a, o);
    return NULL;
}
const void *adt_getprop(const void *a, int node, const char *name, u32 *lenp)
{
    const struct adt_property *p = adt_get_property(a, node, name);
    if (!p) return NULL;
    if (lenp) *lenp = p->size & 0x7fffffff;
    return p->value;
}
int adt_getprop_copy(const void *a, int node, const char *name, void *out, size_t len)
{
    u32 l; const void *p = adt_getprop(a, node, name, &l);
    if (!p || l != len) return -1;
    memcpy(out, p, len); return (int)l;
}
const char *adt_get_name(const void *a, int node) { const void *p = adt_getprop(a, node, "name", NULL); return p; }
int adt_subnode_offset(const void *a, int parent, const char *name)
{
    int o = after_props(a, parent);
    for (uint32_t i = hdr(a, parent)->child_count; i; i--, o = skip_node(a, o)) {
        const char *n = adt_get_name(a, o);
        if (n && !strcmp(n, name)) return o;
    }
    return -1;
}
int adt_path_offset_trace(const void *a, const char *path, int *offsets)
{
    int node = 0;
    if (offsets) *offsets++ = 0;
    while (*path == 0x2f) path++;
    while (*path) {
        const char *e = strchr(path, 0x2f); size_t n = e ? (size_t)(e - path) : strlen(path);
        char name[64];
        if (n >= sizeof(name)) return -1;
        memcpy(name, path, n); name[n] = 0;
        node = adt_subnode_offset(a, node, name);
        if (node < 0) return -1;
        if (offsets) *offsets++ = node;
        path += n; while (*path == 0x2f) path++;
    }
    if (offsets) *offsets = 0;
    return node;
}
int adt_path_offset(const void *a, const char *path) { return adt_path_offset_trace(a, path, NULL); }
bool adt_is_compatible_at(const void *a, int node, const char *compat, size_t index)
{
    u32 l; const char *c = adt_getprop(a, node, "compatible", &l);
    size_t i = 0;
    for (u32 p = 0; c && p < l; p += strlen(c + p) + 1, i++)
        if (!strcmp(c + p, compat) && (index == (size_t)-1 || i == index)) return true;
    return false;
}
bool adt_is_compatible(const void *a, int node, const char *compat) { return adt_is_compatible_at(a, node, compat, (size_t)-1); }
int adt_get_reg(const void *a, int *path, const char *prop, int idx, u64 *addr, u64 *size)
{
    int n = 0; while (path[n + 1]) n++;
    int node = path[n];
    u32 l; const u64 *reg = adt_getprop(a, node, prop, &l);
    if (!reg || (unsigned)(idx + 1) * 16 > l) return -1;
    u64 ad = reg[2 * idx], sz = reg[2 * idx + 1];
    /* translate through every ancestor  (24-byte entries: child, parent, size) */
    for (int i = n - 1; i >= 1; i--) {
        u32 rl; const u64 *r = adt_getprop(a, path[i], "ranges", &rl);
        if (!r || !rl) continue;
        bool ok = false;
        for (u32 j = 0; j + 24 <= rl; j += 24, r += 3)
            if (ad >= r[0] && ad - r[0] < r[2]) { ad = ad - r[0] + r[1]; ok = true; break; }
        if (!ok) return -1;
    }
    if (addr) *addr = ad;
    if (size) *size = sz;
    return 0;
}
int adt_setprop(void *a, int node, const char *name, void *value, size_t len) { (void)a; (void)node; (void)name; (void)value; (void)len; return -1; }
