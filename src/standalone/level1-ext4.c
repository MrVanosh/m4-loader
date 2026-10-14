/* SPDX-License-Identifier: MIT */
/* Read-only ext4 reader for the R39 Level 1 loader. 4 KiB blocks, 64-bit group
 * descriptors, extent trees of any depth (<= 5), linear directory scan (works
 * on htree directories because the index blocks look like empty dirents),
 * metadata checksums ignored. Refuses: a filesystem that needs journal
 * recovery, inline data, encrypted and indirect-block files, symlinks.
 * Never writes. Every block number is bounded by blocks_count and then by the
 * device guard (p4 range). */
#include "level1.h"

#define EXT4_MAGIC 0xEF53u
#define INCOMPAT_FILETYPE 0x2u
#define INCOMPAT_RECOVER 0x4u
#define INCOMPAT_EXTENTS 0x40u
#define INCOMPAT_64BIT 0x80u
#define INCOMPAT_FLEX_BG 0x200u
#define INCOMPAT_CSUM_SEED 0x2000u
#define INCOMPAT_LARGEDIR 0x4000u
#define INCOMPAT_INLINE 0x8000u
#define INCOMPAT_ENCRYPT 0x10000u
#define INCOMPAT_OK (INCOMPAT_FILETYPE | INCOMPAT_EXTENTS | INCOMPAT_64BIT | INCOMPAT_FLEX_BG | \
                     INCOMPAT_CSUM_SEED | INCOMPAT_LARGEDIR | INCOMPAT_INLINE | INCOMPAT_ENCRYPT)
#define FL_ENCRYPT 0x800u
#define FL_EXTENTS 0x80000u
#define FL_INLINE 0x10000000u
#define EXT_MAGIC 0xF30Au
#define ROOT_INO 2u

static inline uint32_t le16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static inline uint32_t le32(const uint8_t *p) { return le16(p) | le16(p + 2) << 16; }

const char *l1_ext4_detail_name(int d)   /* switch, not a pointer table: see l1_status_name */
{
    switch (d) {
    case L1X_OK: return "ok";
    case L1X_READ: return "read";
    case L1X_MAGIC: return "magic";
    case L1X_BLOCKSIZE: return "blocksize";
    case L1X_FEATURES: return "features";
    case L1X_DIRTY: return "dirty";
    case L1X_SIZE: return "size";
    case L1X_GDT: return "gdt";
    case L1X_INODE: return "inode";
    case L1X_INLINE: return "inline";
    case L1X_NOEXTENTS: return "noextents";
    case L1X_ENCRYPTED: return "encrypted";
    case L1X_EXTENT: return "extent";
    case L1X_DIR: return "dir";
    case L1X_NOTFOUND: return "notfound";
    case L1X_NOTDIR: return "notdir";
    case L1X_NOTREG: return "notreg";
    case L1X_PATH: return "path";
    case L1X_RANGE: return "range";
    case L1X_SYMLINK: return "symlink";
    case L1X_SHORT: return "short";
    default: return "?";
    }
}

static int fail(struct l1_ext4 *fs, int d) { fs->detail = d; return d; }

static int rd_block(struct l1_ext4 *fs, uint64_t blk, void *dst)
{
    if (blk >= fs->blocks) return fail(fs, L1X_RANGE);
    if (l1_dev_read(fs->dev, fs->base_lba + blk, 1, dst)) return fail(fs, L1X_READ);
    return 0;
}

int l1_ext4_mount(struct l1_ext4 *fs, struct l1_dev *dev, uint64_t base_lba, uint64_t part_blocks, uint8_t *tmp)
{
    fs->dev = dev; fs->base_lba = base_lba; fs->tmp = tmp; fs->detail = 0;
    fs->blocks = part_blocks; /* until the superblock is read */
    if (rd_block(fs, 0, tmp)) return fs->detail;
    const uint8_t *sb = tmp + 1024;
    if (le16(sb + 0x38) != EXT4_MAGIC) return fail(fs, L1X_MAGIC);
    if (le32(sb + 0x18) != 2 || le32(sb + 0x14) != 0) return fail(fs, L1X_BLOCKSIZE);
    uint32_t state = le16(sb + 0x3a), incompat = le32(sb + 0x60), rocompat = le32(sb + 0x64);
    if (le32(sb + 0x4c) != 1) return fail(fs, L1X_FEATURES);
    if (incompat & INCOMPAT_RECOVER) return fail(fs, L1X_DIRTY);
    if (!(state & 1) || (state & 2)) return fail(fs, L1X_DIRTY);
    if ((incompat & ~INCOMPAT_OK) || !(incompat & INCOMPAT_EXTENTS)) return fail(fs, L1X_FEATURES);
    fs->incompat = incompat; fs->ro_compat = rocompat;
    uint64_t blocks = le32(sb + 4);
    fs->desc_size = 32;
    if (incompat & INCOMPAT_64BIT) {
        blocks |= (uint64_t)le32(sb + 0x150) << 32;
        fs->desc_size = le16(sb + 0xfe);
        if (fs->desc_size < 64 || fs->desc_size > 1024 || (fs->desc_size & (fs->desc_size - 1))) return fail(fs, L1X_GDT);
    }
    fs->inodes_per_group = le32(sb + 0x28);
    fs->blocks_per_group = le32(sb + 0x20);
    fs->inode_size = le16(sb + 0x58);
    fs->first_data_block = 0;
    if (!blocks || blocks > part_blocks) return fail(fs, L1X_SIZE);
    if (!fs->inodes_per_group || fs->inodes_per_group > 65536 || !fs->blocks_per_group ||
        fs->blocks_per_group > 32768 || fs->inode_size < 128 || fs->inode_size > 4096 ||
        (fs->inode_size & (fs->inode_size - 1))) return fail(fs, L1X_SIZE);
    fs->blocks = blocks;
    fs->groups = (uint32_t)((blocks + fs->blocks_per_group - 1) / fs->blocks_per_group);
    uint64_t gdt_blocks = ((uint64_t)fs->groups * fs->desc_size + 4095) / 4096;
    if (!fs->groups || 1 + gdt_blocks >= blocks) return fail(fs, L1X_GDT);
    return 0;
}

int l1_ext4_inode(struct l1_ext4 *fs, uint32_t ino, struct l1_inode *out)
{
    if (!ino || ino > (uint64_t)fs->inodes_per_group * fs->groups) return fail(fs, L1X_INODE);
    uint32_t idx = ino - 1, group = idx / fs->inodes_per_group, in_group = idx % fs->inodes_per_group;
    uint64_t gd = (uint64_t)group * fs->desc_size;
    if (rd_block(fs, 1 + gd / 4096, fs->tmp)) return fs->detail;
    const uint8_t *d = fs->tmp + gd % 4096;
    uint64_t table = le32(d + 8);
    if (fs->desc_size >= 64) table |= (uint64_t)le32(d + 0x28) << 32;
    uint64_t byte = (uint64_t)in_group * fs->inode_size;
    uint64_t blk = table + byte / 4096;
    if (!table || table >= fs->blocks) return fail(fs, L1X_GDT);
    if (rd_block(fs, blk, fs->tmp)) return fs->detail;
    const uint8_t *p = fs->tmp + byte % 4096;
    out->mode = (uint16_t)le16(p);
    out->flags = le32(p + 0x20);
    out->size = le32(p + 4);
    if ((out->mode & 0xf000) == 0x8000) out->size |= (uint64_t)le32(p + 0x6c) << 32;
    for (unsigned i = 0; i < 60; i++) out->iblock[i] = p[0x28 + i];
    if (!out->mode) return fail(fs, L1X_INODE);
    if (out->flags & FL_INLINE) return fail(fs, L1X_INLINE);
    if (out->flags & FL_ENCRYPT) return fail(fs, L1X_ENCRYPTED);
    if (!(out->flags & FL_EXTENTS)) return fail(fs, L1X_NOEXTENTS);
    return 0;
}

/* Maps logical block lblk. On success: *pblk (0 = hole/unwritten) and *run
 * (blocks, >= 1) that share the same mapping state. */
static int ext_map(struct l1_ext4 *fs, const struct l1_inode *in, uint32_t lblk, uint64_t *pblk, uint32_t *run)
{
    const uint8_t *n = in->iblock;
    uint8_t *node = fs->tmp + 2 * 4096;
    for (unsigned level = 0;; level++) {
        if (le16(n) != EXT_MAGIC) return fail(fs, L1X_EXTENT);
        uint32_t entries = le16(n + 2), max = le16(n + 4), depth = le16(n + 6);
        if (entries > max || depth > 5 || level > 5 || (level == 0 ? max > 4 : max > 340)) return fail(fs, L1X_EXTENT);
        if (depth == 0) {
            uint32_t best = entries, next = 0;
            for (uint32_t i = 0; i < entries; i++) {
                uint32_t eb = le32(n + 12 + 12 * i);
                if (eb <= lblk) best = i; else { next = eb; break; }
            }
            if (best != entries) {
                const uint8_t *e = n + 12 + 12 * best;
                uint32_t eb = le32(e), len = le16(e + 4);
                uint64_t start = le32(e + 8) | (uint64_t)le16(e + 6) << 32;
                int unwritten = 0;
                if (len > 32768) { len -= 32768; unwritten = 1; }
                if (!len) return fail(fs, L1X_EXTENT);
                if (lblk - eb < len) {
                    *run = len - (lblk - eb);
                    *pblk = unwritten ? 0 : start + (lblk - eb);
                    if (!unwritten && (start + len > fs->blocks || start + len < start)) return fail(fs, L1X_RANGE);
                    return 0;
                }
            }
            *pblk = 0;
            if (!next) next = (uint32_t)0xffffffffu;
            *run = next > lblk ? next - lblk : 1;
            return 0;
        }
        uint32_t best = entries;
        for (uint32_t i = 0; i < entries; i++) {
            if (le32(n + 12 + 12 * i) <= lblk) best = i; else break;
        }
        if (best == entries) { *pblk = 0; *run = 1; return 0; }
        const uint8_t *e = n + 12 + 12 * best;
        uint64_t child = le32(e + 4) | (uint64_t)le16(e + 8) << 32;
        if (rd_block(fs, child, node)) return fs->detail;
        n = node;
    }
}

int l1_ext4_read(struct l1_ext4 *fs, const struct l1_inode *in, uint64_t off, uint64_t len, void *dst)
{
    uint8_t *out = dst;
    if (off & 4095) return fail(fs, L1X_RANGE);
    if (off + len > in->size || off + len < off) return fail(fs, L1X_SHORT);
    while (len) {
        uint32_t lblk = (uint32_t)(off / 4096);
        uint64_t pblk; uint32_t run;
        if (ext_map(fs, in, lblk, &pblk, &run)) return fs->detail;
        if (len < 4096) {
            /* tail: bounce through tmp[1] (tmp[2] is the tree node buffer) */
            uint8_t *b = fs->tmp + 4096;
            if (pblk) { if (rd_block(fs, pblk, b)) return fs->detail; }
            else for (unsigned i = 0; i < 4096; i++) b[i] = 0;
            for (uint64_t i = 0; i < len; i++) out[i] = b[i];
            return 0;
        }
        uint64_t whole = len / 4096;
        uint64_t n = run < whole ? run : whole;
        if (pblk) {
            if (pblk + n > fs->blocks) return fail(fs, L1X_RANGE);
            if (l1_dev_read(fs->dev, fs->base_lba + pblk, (uint32_t)n, out)) return fail(fs, L1X_READ);
        } else {
            for (uint64_t i = 0; i < n * 4096; i++) out[i] = 0;
        }
        out += n * 4096; off += n * 4096; len -= n * 4096;
    }
    return 0;
}

/* Linear lookup of one name in a directory. */
static int dir_lookup(struct l1_ext4 *fs, const struct l1_inode *dir, const char *name, unsigned nlen, uint32_t *ino, unsigned *type)
{
    if ((dir->mode & 0xf000) != 0x4000) return fail(fs, L1X_NOTDIR);
    if (dir->size & 4095 || !dir->size) return fail(fs, L1X_DIR);
    uint64_t blocks = dir->size / 4096;
    if (blocks > 16384) return fail(fs, L1X_DIR); /* 64 MiB of directory: not a boot directory */
    uint8_t *b = fs->tmp + 4096;
    for (uint64_t lb = 0; lb < blocks; lb++) {
        uint64_t pblk; uint32_t run;
        if (ext_map(fs, dir, (uint32_t)lb, &pblk, &run)) return fs->detail;
        if (!pblk) { lb += run ? run - 1 : 0; continue; } /* hole / unwritten: skip the whole run */
        if (rd_block(fs, pblk, b)) return fs->detail;
        for (unsigned pos = 0; pos < 4096;) {
            if (pos + 8 > 4096) return fail(fs, L1X_DIR);
            const uint8_t *e = b + pos;
            uint32_t rec = le16(e + 4), nl = e[6];
            if (rec < 8 || (rec & 3) || pos + rec > 4096 || 8 + nl > rec) return fail(fs, L1X_DIR);
            if (le32(e) && nl == nlen) {
                unsigned i = 0;
                while (i < nl && e[8 + i] == (uint8_t)name[i]) i++;
                if (i == nl) { *ino = le32(e); *type = e[7]; return 0; }
            }
            pos += rec;
        }
    }
    return fail(fs, L1X_NOTFOUND);
}

int l1_ext4_lookup_path(struct l1_ext4 *fs, const char *path, uint32_t *ino)
{
    if (!l1_path_ok(path)) return fail(fs, L1X_PATH);
    uint32_t cur = ROOT_INO;
    const char *p = path;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *s = p;
        while (*p && *p != '/') p++;
        unsigned n = (unsigned)(p - s);
        struct l1_inode dir;
        if (l1_ext4_inode(fs, cur, &dir)) return fs->detail;
        unsigned type = 0;
        if (dir_lookup(fs, &dir, s, n, &cur, &type)) return fs->detail;
        if (type == 7) return fail(fs, L1X_SYMLINK);
    }
    *ino = cur;
    return 0;
}
