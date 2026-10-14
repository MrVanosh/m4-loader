/* SPDX-License-Identifier: MIT */
/* Level 1 policy, Boot ABI v1: GPT, ext4, boot-pending, loader.conf, entry.conf, load + verify.
 * Portable and hardware-free; the caller supplies the block device and the
 * destination memory. Nothing here writes to the device. */
#include "level1.h"

static void zero(void *p, size_t n) { uint8_t *b = p; while (n--) *b++ = 0; }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t le64(const uint8_t *p) { return le32(p) | (uint64_t)le32(p + 4) << 32; }
static void scopy(char *dst, size_t cap, const char *s)
{
    size_t i = 0;
    while (s[i] && i + 1 < cap) { dst[i] = s[i]; i++; }
    dst[i] = 0;
}
/* The blob is linked at address 0 and has no relocation step: a table of string pointers would hold
 * link-time addresses and dereference garbage when the blob runs elsewhere (R39's final line did exactly
 * that: the fault escaped every guard and stopped the boot). Switches over literals are PC-relative; the
 * Makefile builds the blob with -fno-tree-switch-conversion (no pointer lookup tables) and refuses any
 * object that still carries an absolute data relocation. */
const char *l1_status_name(enum l1_status s)
{
    switch (s) {
    case L1_OK: return "ok";
    case L1_DISABLED: return "disabled";
    case L1_NO_CONF: return "no-conf";
    case L1_CARRIER_MISMATCH: return "unused";
    case L1_BAD_CONF: return "bad-conf";
    case L1_GPT: return "gpt";
    case L1_EXT4: return "ext4";
    case L1_NOT_FOUND: return "not-found";
    case L1_TOO_LARGE: return "too-large";
    case L1_SHA_IMAGE: return "sha-image";
    case L1_SHA_INITRD: return "sha-initrd";
    case L1_IMAGE_HEADER: return "image-header";
    case L1_NVME_READ: return "nvme-read";
    case L1_TIMEOUT: return "timeout";
    case L1_ANS_POWER: return "ans-power";
    case L1_RTKIT_BOOT: return "rtkit-boot";
    case L1_NVME_ENABLE: return "nvme-enable";
    case L1_NVME_IDENTIFY: return "nvme-identify";
    case L1_NVME_QUEUES: return "nvme-queues";
    case L1_ABORT: return "abort";
    case L1_PENDING: return "pending";
    case L1_BAD_ABI: return "bad-abi";
    case L1_NO_ENTRY: return "no-entry";
    case L1_SIZE: return "size";
    case L1_DIGEST: return "digest";
    case L1_BAD_DTB: return "bad-dtb";
    default: return "unknown";
    }
}

/* GPT sectors and p4 only; everything else is refused before the device sees it. */
int l1_dev_read(struct l1_dev *d, uint64_t lba, uint32_t n, void *dst)
{
    if (!n || lba + n < lba) return L1_NVME_READ;
    int gpt = lba + n <= 8;
    int p4 = lba >= L1_P4_FIRST && lba + n <= L1_P4_END;
    if (!gpt && !p4) return L1_NVME_READ;
    d->reads++;
    d->blocks += n;
    return d->read(d->ctx, lba, n, dst);
}

/* PARTUUID of the pinned partition (level1-install.h), GPT mixed-endian bytes. */
static const uint8_t P4_GUID[16] = L1_P4_GUID_BYTES;
/* ebd0a0a2-b9e5-4433-87c0-68b6b72699c7 (basic data), 0fc63daf-8483-4772-8e79-3d69d8477de4 (Linux fs) */
static const uint8_t TYPE_BASIC[16] = {0xa2, 0xa0, 0xd0, 0xeb, 0xe5, 0xb9, 0x33, 0x44,
                                       0x87, 0xc0, 0x68, 0xb6, 0xb7, 0x26, 0x99, 0xc7};
static const uint8_t TYPE_LINUX[16] = {0xaf, 0x3d, 0xc6, 0x0f, 0x83, 0x84, 0x72, 0x47,
                                       0x8e, 0x79, 0x3d, 0x69, 0xd8, 0x47, 0x7d, 0xe4};
static int same16(const uint8_t *a, const uint8_t *b)
{
    for (unsigned i = 0; i < 16; i++) if (a[i] != b[i]) return 0;
    return 1;
}

static uint32_t crc_step(uint32_t c, const uint8_t *p, size_t n)
{
    while (n--) {
        c ^= *p++;
        for (unsigned k = 0; k < 8; k++) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1)));
    }
    return c;
}

/* why: 1 read, 2 signature, 3 header, 4 crc, 5 entries, 6 not found, 7 range.
 * buf: two 4 KiB pages (header, one block of entries at a time). */
int l1_gpt_find_p4(struct l1_dev *d, uint8_t *buf, int *why)
{
    uint8_t *hdr = buf;
    uint8_t *ent = buf + 4096;
    *why = 0;
    if (l1_dev_read(d, 1, 1, hdr)) { *why = 1; return L1_GPT; }
    static const char sig[8] = {'E', 'F', 'I', ' ', 'P', 'A', 'R', 'T'};
    for (unsigned i = 0; i < 8; i++) if (hdr[i] != (uint8_t)sig[i]) { *why = 2; return L1_GPT; }
    uint32_t hsize = le32(hdr + 12);
    if (hsize < 92 || hsize > 512 || le64(hdr + 24) != 1) { *why = 3; return L1_GPT; }
    uint32_t crc = le32(hdr + 16);
    uint8_t tmp[512];
    for (unsigned i = 0; i < hsize; i++) tmp[i] = hdr[i];
    tmp[16] = tmp[17] = tmp[18] = tmp[19] = 0;
    if (l1_crc32(tmp, hsize) != crc) { *why = 4; return L1_GPT; }
    uint64_t elba = le64(hdr + 72);
    uint32_t num = le32(hdr + 80), esz = le32(hdr + 84), ecrc = le32(hdr + 88);
    if (esz != 128 || !num || num > 128 || elba < 2) { *why = 5; return L1_GPT; }
    uint32_t bytes = num * esz, blocks = (bytes + 4095) / 4096;
    if (elba + blocks > 8) { *why = 5; return L1_GPT; }
    uint32_t c = 0xffffffffu;
    int found = 0, conflict = 0;
    for (uint32_t b = 0; b < blocks; b++) {
        if (l1_dev_read(d, elba + b, 1, ent)) { *why = 1; return L1_GPT; }
        uint32_t chunk = bytes - b * 4096 < 4096 ? bytes - b * 4096 : 4096;
        c = crc_step(c, ent, chunk);
        for (uint32_t o = 0; o + esz <= chunk; o += esz) {
            const uint8_t *e = ent + o;
            int guid = same16(e + 16, P4_GUID);
            int range = le64(e + 32) == L1_P4_FIRST && le64(e + 40) == L1_P4_END - 1;
            int type = same16(e, TYPE_BASIC) || same16(e, TYPE_LINUX);
            if (guid && !range) conflict = 1;
            else if (range && (guid || type)) found = 1;
        }
    }
    if (~c != ecrc) { *why = 4; return L1_GPT; }
    if (conflict) { *why = 7; return L1_GPT; }
    if (!found) { *why = 6; return L1_GPT; }
    return 0;
}

int l1_image_header_ok(const uint8_t *h, uint64_t file_bytes, uint64_t window, uint64_t *image_memory)
{
    if (file_bytes < 64) return 1;
    if (h[56] != 'A' || h[57] != 'R' || h[58] != 'M' || h[59] != 0x64) return 2;
    if (le64(h + 8)) return 3;                 /* text_offset must be 0 */
    uint64_t size = le64(h + 16), flags = le64(h + 24);
    if ((flags & 7) != 4) return 4;            /* little endian, 16 KiB pages */
    if (size < file_bytes || (size & 0xfff)) return 5;
    if (size > window) return 6;
    *image_memory = size;
    return 0;
}

static void step(struct l1_load *L, const char *n, uint64_t v)
{
    if (L->step) L->step(L->hook_ctx, n, v);
}

/* ---- key=value text ---------------------------------------------------- */
static int key_eq(const char *p, size_t n, const char *key)
{
    size_t i = 0;
    while (i < n && key[i] && p[i] == key[i]) i++;
    return i == n && !key[i];
}
int l1_kv_get(const char *text, size_t len, const char *key, const char **value, size_t *vlen)
{
    int found = 0;
    size_t pos = 0;
    while (pos < len) {
        size_t e = pos;
        while (e < len && text[e] != '\n') e++;
        size_t le = e;
        if (le > pos && text[le - 1] == '\r') le--;
        if (le > pos && text[pos] != '#') {
            size_t eq = pos;
            while (eq < le && text[eq] != '=') eq++;
            if (eq == le || eq == pos) return -1;
            for (size_t i = pos; i < eq; i++) {
                char c = text[i];
                if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.')) return -1;
            }
            for (size_t i = eq + 1; i < le; i++) if ((unsigned char)text[i] < 0x20 || (unsigned char)text[i] > 0x7e) return -1;
            if (key_eq(text + pos, eq - pos, key)) {
                if (found) return -1;
                found = 1;
                *value = text + eq + 1;
                *vlen = le - eq - 1;
            }
        }
        pos = e + 1;
    }
    return found ? 0 : 1;
}
int l1_token_ok(const char *t, size_t n)
{
    if (!n || n >= L1_NAME_MAX || t[0] == '.') return 0;
    for (size_t i = 0; i < n; i++) {
        char c = t[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-' || c == '+')) return 0;
        if (c == '.' && i && t[i - 1] == '.') return 0;
    }
    return 1;
}
static int parse_u64(const char *p, size_t n, uint64_t *out)
{
    uint64_t v = 0;
    if (!n || n > 12) return 0;
    for (size_t i = 0; i < n; i++) {
        if (p[i] < '0' || p[i] > '9') return 0;
        v = v * 10 + (uint64_t)(p[i] - '0');
    }
    *out = v;
    return 1;
}
static int parse_hex32(const char *p, size_t n, uint32_t *out)
{
    uint32_t v = 0;
    if (n != 8) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = p[i];
        uint32_t d = c >= '0' && c <= '9' ? (uint32_t)(c - '0') : c >= 'a' && c <= 'f' ? (uint32_t)(c - 'a' + 10) : 16;
        if (d > 15) return 0;
        v = v << 4 | d;
    }
    *out = v;
    return 1;
}

/* One small text file under /boot/m4: stat, regular, 1..L1_CONF_MAX bytes, read into buf (4 KiB aligned). */
static int read_small(struct l1_ext4 *fs, const char *path, uint8_t *buf, uint32_t *len, struct l1_result *R,
                      enum l1_status missing)
{
    uint32_t ino;
    struct l1_inode in;
    if (l1_ext4_lookup_path(fs, path, &ino)) {
        R->detail = fs->detail;
        return fs->detail == L1X_NOTFOUND || fs->detail == L1X_NOTDIR ? (int)missing : L1_EXT4;
    }
    if (l1_ext4_inode(fs, ino, &in)) { R->detail = fs->detail; return L1_EXT4; }
    if ((in.mode & 0xf000) != 0x8000 || !in.size || in.size > L1_CONF_MAX) { R->detail = 0; return L1_BAD_CONF; }
    if (l1_ext4_read(fs, &in, 0, in.size, buf)) { R->detail = fs->detail; return fs->detail == L1X_READ ? L1_NVME_READ : L1_EXT4; }
    *len = (uint32_t)in.size;
    return 0;
}

struct l1_file_spec { const char *name; uint64_t size; uint32_t crc; };

/* One entry file: stat, size check against entry.conf, read into dst, CRC-32. */
static int load_file(struct l1_load *L, struct l1_ext4 *fs, const char *path, const struct l1_file_spec *f,
                     uint8_t *dst, uint64_t window, struct l1_result *R)
{
    uint32_t ino;
    struct l1_inode in;
    scopy(R->file, sizeof(R->file), f->name);
    if (f->size > window) return L1_TOO_LARGE;
    if (l1_ext4_lookup_path(fs, path, &ino)) {
        R->detail = fs->detail;
        return fs->detail == L1X_NOTFOUND || fs->detail == L1X_NOTDIR ? L1_NOT_FOUND : L1_EXT4;
    }
    if (l1_ext4_inode(fs, ino, &in)) { R->detail = fs->detail; return L1_EXT4; }
    if ((in.mode & 0xf000) != 0x8000) { R->detail = L1X_NOTREG; return L1_NOT_FOUND; }
    if (in.size != f->size) { R->detail = L1X_SHORT; return L1_SIZE; }
    if (l1_ext4_read(fs, &in, 0, f->size, dst)) { R->detail = fs->detail; return fs->detail == L1X_READ ? L1_NVME_READ : L1_EXT4; }
    if (L->pre_verify) L->pre_verify(L->hook_ctx, dst, f->size);
    uint32_t crc = l1_crc32_fast(dst, (size_t)f->size);
    step(L, "hashed", f->size);
    if (crc != f->crc) return L1_DIGEST;
    return 0;
}

int l1_path_ok(const char *p)
{
    size_t n = 0;
    while (p[n]) n++;
    if (n < 2 || n > 255 || p[0] != '/') return 0;
    unsigned comps = 0, len = 0;
    for (size_t i = 1; i <= n; i++) {
        char c = p[i];
        if (c == '/' || c == 0) {
            if (!len) return 0; /* empty component, double or trailing slash */
            if (len > 64) return 0;
            if ((len == 1 && p[i - 1] == '.') || (len == 2 && p[i - 1] == '.' && p[i - 2] == '.')) return 0;
            if (++comps > 6) return 0;
            len = 0;
            continue;
        }
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '.' || c == '+' || c == '-')) return 0;
        len++;
    }
    return comps >= 1;
}

static size_t sl(const char *s) { size_t n = 0; while (s[n]) n++; return n; }

/* Reason words (/chosen m4,boot-reason values of Boot ABI v1) for a refused policy run. A literal per case: no pointer table. */
const char *l1_reason(enum l1_status s, int detail)
{
    switch (s) {
    case L1_OK: return "default";
    case L1_PENDING: return "previous-boot-incomplete";
    case L1_NO_CONF: return "no-loader-conf";
    case L1_BAD_CONF: return "bad-conf";
    case L1_BAD_ABI: return "bad-abi";
    case L1_NO_ENTRY: return "no-entry";
    case L1_NOT_FOUND: return "missing-file";
    case L1_SIZE: return "size-mismatch";
    case L1_DIGEST: return "digest-mismatch";
    case L1_IMAGE_HEADER: return "bad-image";
    case L1_TOO_LARGE: return "too-large";
    case L1_BAD_DTB: return "bad-dtb";
    case L1_EXT4: return detail == L1X_DIRTY ? "ext4-dirty" : "ssd-unreadable";
    case L1_GPT: case L1_NVME_READ: case L1_TIMEOUT: return "ssd-unreadable";
    default: return "ssd-unavailable";
    }
}

int l1_run(struct l1_load *L, struct l1_result *R)
{
    zero(R, sizeof(*R));
    struct l1_ext4 fs;
    int why;
    step(L, "gpt", 0);
    if (l1_gpt_find_p4(L->dev, L->meta, &why)) { R->status = L1_GPT; R->detail = why; return R->status; }
    step(L, "ext4", 0);
    if (l1_ext4_mount(&fs, L->dev, L1_P4_FIRST, L1_P4_END - L1_P4_FIRST, L->meta)) {   /* a dirty file system is refused here */
        R->status = L1_EXT4; R->detail = fs.detail; return R->status;
    }
    return l1_run_fs(L, &fs, R);
}

/* Everything after the mount: boot-pending, loader.conf, entry.conf, the three files. Shared with the host
 * decision tool (tools/loader-decision.c), which mounts an image of the file system instead of the SSD. */
int l1_run_fs(struct l1_load *L, struct l1_ext4 *fsp, struct l1_result *R)
{
    struct l1_ext4 fs = *fsp;
    int rc;
    /* the previous boot never reached display-manager + 20 s: existence is the whole signal, any content */
    {
        uint32_t ino;
        if (!l1_ext4_lookup_path(&fs, L1_PENDING_PATH, &ino)) { R->status = L1_PENDING; return R->status; }
        if (fs.detail != L1X_NOTFOUND && fs.detail != L1X_NOTDIR) { R->detail = fs.detail; R->status = L1_EXT4; return R->status; }
    }
    uint8_t *conf = L->meta + 3 * 4096; /* the 4th scratch page */
    uint32_t n = 0;
    const char *v;
    size_t vl;
    uint64_t abi;
    rc = read_small(&fs, L1_LOADER_CONF, conf, &n, R, L1_NO_CONF);
    if (rc) { R->status = (enum l1_status)rc; return R->status; }
    step(L, "loader.conf", n);
    if (l1_kv_get((const char *)conf, n, "abi", &v, &vl) || !parse_u64(v, vl, &abi)) { R->status = L1_BAD_CONF; R->detail = 1; return R->status; }
    if (abi != L1_ABI) { R->status = L1_BAD_ABI; R->detail = 1; return R->status; }
    if (l1_kv_get((const char *)conf, n, "default", &v, &vl) || !l1_token_ok(v, vl)) { R->status = L1_BAD_CONF; R->detail = 2; return R->status; }
    for (size_t i = 0; i < vl; i++) R->entry[i] = v[i];
    R->entry[vl] = 0;

    char path[160];
    size_t base = sl(L1_ENTRY_DIR), elen = vl;
    for (size_t i = 0; i < base; i++) path[i] = L1_ENTRY_DIR[i];
    for (size_t i = 0; i < elen; i++) path[base + i] = R->entry[i];
    path[base + elen] = '/';
    size_t dir = base + elen + 1;
    static const char ec[] = "entry.conf";
    for (size_t i = 0; i <= sizeof(ec) - 1; i++) path[dir + i] = ec[i];
    rc = read_small(&fs, path, conf, &n, R, L1_NO_ENTRY);
    if (rc) { R->status = (enum l1_status)rc; return R->status; }
    step(L, "entry.conf", n);
    if (l1_kv_get((const char *)conf, n, "abi", &v, &vl) || !parse_u64(v, vl, &abi)) { R->status = L1_BAD_CONF; R->detail = 3; return R->status; }
    if (abi != L1_ABI) { R->status = L1_BAD_ABI; R->detail = 2; return R->status; }
    if (l1_kv_get((const char *)conf, n, "kver", &v, &vl) || !l1_token_ok(v, vl)) { R->status = L1_BAD_CONF; R->detail = 4; return R->status; }
    for (size_t i = 0; i < vl; i++) R->kver[i] = v[i];
    R->kver[vl] = 0;
    if (l1_kv_get((const char *)conf, n, "cmdline", &v, &vl) || !vl || vl > L1_CMDLINE_MAX) { R->status = L1_BAD_CONF; R->detail = 5; return R->status; }
    for (size_t i = 0; i < vl; i++) R->cmdline[i] = v[i];
    R->cmdline[vl] = 0;

    struct l1_file_spec spec[3];
    zero(spec, sizeof(spec));
    spec[0].name = "board.dtb"; spec[1].name = "Image"; spec[2].name = "initramfs.img"; /* PC-relative, no data relocation */
    for (unsigned k = 0; k < 3; k++) {
        char key[32];
        size_t kl = sl(spec[k].name);
        for (size_t i = 0; i < kl; i++) key[i] = spec[k].name[i];
        static const char sz[] = "_size", cr[] = "_crc32";
        for (size_t i = 0; i <= sizeof(sz) - 1; i++) key[kl + i] = sz[i];
        if (l1_kv_get((const char *)conf, n, key, &v, &vl) || !parse_u64(v, vl, &spec[k].size) || !spec[k].size) {
            scopy(R->file, sizeof(R->file), spec[k].name); R->status = L1_BAD_CONF; R->detail = 6; return R->status;
        }
        for (size_t i = 0; i <= sizeof(cr) - 1; i++) key[kl + i] = cr[i];
        if (l1_kv_get((const char *)conf, n, key, &v, &vl) || !parse_hex32(v, vl, &spec[k].crc)) {
            scopy(R->file, sizeof(R->file), spec[k].name); R->status = L1_BAD_CONF; R->detail = 7; return R->status;
        }
    }
    if (spec[1].size < L->image_floor) { scopy(R->file, sizeof(R->file), "Image"); R->status = L1_TOO_LARGE; R->detail = 0; return R->status; }
    /* board.dtb first (small), then Image, then initramfs.img */
    for (unsigned k = 0; k < 3; k++) {
        size_t nl = sl(spec[k].name);
        for (size_t i = 0; i <= nl; i++) path[dir + i] = spec[k].name[i];
        uint8_t *dst = k == 0 ? L->dtb_dst : k == 1 ? L->image_dst : L->initrd_dst;
        uint64_t window = k == 0 ? L->dtb_window : k == 1 ? L->image_window : L->initrd_window;
        step(L, spec[k].name, spec[k].size);
        rc = load_file(L, &fs, path, &spec[k], dst, window, R);
        if (rc) { R->status = (enum l1_status)rc; return R->status; }
        if (k == 1) {
            uint64_t memory = 0;
            int h = l1_image_header_ok(L->image_dst, spec[1].size, L->image_window, &memory);
            if (h) { R->detail = h; R->status = h == 6 ? L1_TOO_LARGE : L1_IMAGE_HEADER; return R->status; }
            R->image_memory = memory;
        }
    }
    step(L, "digest_ok", 0);
    R->image_bytes = spec[1].size;
    R->initrd_bytes = spec[2].size;
    R->dtb_bytes = spec[0].size;
    R->status = L1_OK;
    return L1_OK;
}
