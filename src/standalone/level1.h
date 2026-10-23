/* SPDX-License-Identifier: MIT */
/* Level 1 (rev 11, Boot ABI v1): the loader reads the kernel Image, the initramfs and the board.dtb of the default
 * entry as files from the SSD (ext4 root, p4). This header is the portable core: SHA-256, CRC-32, GPT, a read-only
 * ext4 reader, the loader.conf / entry.conf parser and the load policy. It has no hardware code; the ANS/NVMe
 * driver (level1-ans.c) supplies a block device. The same C files are built on the host for the offline tests.
 */
#ifndef M4_LEVEL1_H
#define M4_LEVEL1_H
#include <stdint.h>
#include <stddef.h>

/* ---- fixed facts (LEVEL1-SPEC.md) ------------------------------------- */
#define L1_BLOCK 4096u                    /* NVMe LBA = ext4 block = 4 KiB */
#include "level1-install.h" /* PRIVATE per install: L1_P4_FIRST, L1_P4_END, L1_P4_GUID_BYTES (+ L1_EXTRA_*) */
#ifndef L1_EXTRA_N
#define L1_EXTRA_N 0               /* other Linux partitions Linux may write; level1-install.py --extra */
#endif
/* Boot ABI v1 (docs: FOR-INTEGRATION.md). Linux writes these; the loader only reads. */
#define L1_ABI 1u
#define L1_LOADER_CONF "/boot/m4/loader.conf"
#define L1_PENDING_PATH "/boot/m4/boot-pending"
#define L1_ENTRY_DIR "/boot/m4/entries/"
/* rev 17: one-shot token, exactly one 4 KiB block of text ("m4_oneshot=armed", "entry=<name>"). The loader boots that entry
 * once: before it reads the entry it rewrites "armed" to "spent" in place (the token's only block, inside the root partition). */
#define L1_ONESHOT_PATH "/boot/m4/oneshot"
#define L1_KIND_LINUX 0u
#define L1_KIND_M1N1 1u          /* entry.conf type=m1n1: one file, m1n1.bin, handed to m1n1 as a chainloaded stage */
#define L1_CONF_MAX 4096u        /* loader.conf and entry.conf */
#define L1_CMDLINE_MAX 1024u     /* cmdline= value, bytes without the NUL */
#define L1_NAME_MAX 64u          /* entry name and kver, with the NUL */
#define L1_LOG_MAX 3072u

/* ---- status ------------------------------------------------------------ */
enum l1_status {
    L1_OK = 0,
    L1_DISABLED,        /* embedded variant or runtime refusal */
    L1_NO_CONF,
    L1_CARRIER_MISMATCH,
    L1_BAD_CONF,
    L1_GPT,
    L1_EXT4,            /* any ext4 structural refusal; detail in l1_result.detail */
    L1_NOT_FOUND,
    L1_TOO_LARGE,
    L1_SHA_IMAGE,
    L1_SHA_INITRD,
    L1_IMAGE_HEADER,
    L1_NVME_READ,
    L1_TIMEOUT,
    L1_ANS_POWER,
    L1_RTKIT_BOOT,
    L1_NVME_ENABLE,
    L1_NVME_IDENTIFY,
    L1_NVME_QUEUES,
    L1_ABORT,           /* synchronous abort inside the attempt */
    L1_PENDING,         /* /boot/m4/boot-pending exists: the previous boot never reached the display manager + 20 s */
    L1_BAD_ABI,         /* loader.conf or entry.conf abi= is not 1 */
    L1_NO_ENTRY,        /* default= names no entry directory / entry.conf */
    L1_SIZE,            /* a file's size differs from <file>_size= */
    L1_DIGEST,          /* a file's CRC-32 differs from <file>_crc32= */
    L1_BAD_DTB,         /* board.dtb is not a complete flattened device tree of its size */
    L1_STATUS_COUNT
};
const char *l1_status_name(enum l1_status s);

/* ---- block device ------------------------------------------------------ */
/* Reads n 4 KiB blocks at absolute disk LBA into dst (4 KiB aligned unless
 * the device says otherwise). Returns 0 or an enum l1_status (non-zero). */
struct l1_dev {
    int (*read)(void *ctx, uint64_t lba, uint32_t n, void *dst);
    void *ctx;
    uint64_t reads, blocks; /* bookkeeping */
    /* rev 17, optional: writes one 4 KiB block (the one-shot token only); NULL = read-only device */
    int (*write)(void *ctx, uint64_t lba, const void *src);
};
/* Range guard in front of every device read: GPT (LBA 0..7) or inside p4. */
int l1_dev_read(struct l1_dev *d, uint64_t lba, uint32_t n, void *dst);
/* The only write: one block inside p4 (the root partition). Returns 0 or non-zero. */
int l1_dev_write1(struct l1_dev *d, uint64_t lba, const void *src);

/* ---- SHA-256 ----------------------------------------------------------- */
struct l1_sha {
    uint32_t h[8];
    uint64_t bytes;
    uint8_t buf[64];
    uint32_t fill;
};
void l1_sha_init(struct l1_sha *);
void l1_sha_update(struct l1_sha *, const void *, size_t);
void l1_sha_final(struct l1_sha *, uint8_t out[32]);
void l1_sha_hex(const uint8_t in[32], char out[65]);
uint32_t l1_crc32(const void *, size_t);

/* ---- ext4 -------------------------------------------------------------- */
struct l1_inode {
    uint16_t mode;
    uint32_t flags;
    uint64_t size;
    uint8_t iblock[60];
};
struct l1_ext4 {
    struct l1_dev *dev;
    uint64_t base_lba;      /* LBA of ext4 block 0 */
    uint64_t blocks;        /* blocks_count */
    uint32_t inodes_per_group, inode_size, blocks_per_group, desc_size, groups;
    uint32_t first_data_block, incompat, ro_compat;
    uint8_t *tmp;           /* 3 x 4 KiB, 4 KiB aligned, supplied by the caller */
    int detail;             /* last refusal reason */
};
enum l1_ext4_detail {
    L1X_OK = 0, L1X_READ, L1X_MAGIC, L1X_BLOCKSIZE, L1X_FEATURES, L1X_DIRTY, L1X_SIZE,
    L1X_GDT, L1X_INODE, L1X_INLINE, L1X_NOEXTENTS, L1X_ENCRYPTED, L1X_EXTENT, L1X_DIR,
    L1X_NOTFOUND, L1X_NOTDIR, L1X_NOTREG, L1X_PATH, L1X_RANGE, L1X_SYMLINK, L1X_SHORT
};
const char *l1_ext4_detail_name(int);
int l1_ext4_mount(struct l1_ext4 *, struct l1_dev *, uint64_t base_lba, uint64_t part_blocks, uint8_t *tmp3);
int l1_ext4_lookup_path(struct l1_ext4 *, const char *path, uint32_t *ino);
int l1_ext4_inode(struct l1_ext4 *, uint32_t ino, struct l1_inode *);
/* Reads [off, off+len) of a regular file into dst. dst must be 4 KiB aligned
 * when len >= 4 KiB; the byte range must lie inside the file. */
int l1_ext4_read(struct l1_ext4 *, const struct l1_inode *, uint64_t off, uint64_t len, void *dst);
/* Absolute disk LBA of a file's logical block lblk; 0 = refused (hole, unwritten, out of the file system). */
int l1_ext4_block_of(struct l1_ext4 *, const struct l1_inode *, uint32_t lblk, uint64_t *lba);
/* Same, hashing as it goes would need a second pass; the caller hashes. */

/* ---- conf text (key=value lines) --------------------------------------- */
/* Finds `key` in `text` (len bytes). 0 = found once (value, vlen without the line end), 1 = absent, -1 = duplicate
 * or a malformed line. Lines are `key=value`; blank lines and `#` comments are skipped. */
int l1_kv_get(const char *text, size_t len, const char *key, const char **value, size_t *vlen);
int l1_token_ok(const char *t, size_t n);
int l1_path_ok(const char *p);

/* ---- loader policy ----------------------------------------------------- */
struct l1_load {
    struct l1_dev *dev;
    uint8_t *meta;              /* 4 x 4 KiB scratch, 4 KiB aligned */
    uint8_t *image_dst;         /* kernel slot (final location) */
    uint64_t image_window;      /* max Image memory (header image_size) */
    uint8_t *initrd_dst;
    uint64_t initrd_window;
    uint8_t *dtb_dst;           /* 16 KiB aligned staging buffer for board.dtb */
    uint64_t dtb_window;
    uint64_t image_floor;       /* minimum acceptable Image file size */
    /* hooks (optional) */
    void (*step)(void *ctx, const char *name, uint64_t value);
    void (*pre_verify)(void *ctx, void *p, uint64_t n); /* cache invalidate before the CPU reads DMA data */
    void *hook_ctx;
};
struct l1_result {
    enum l1_status status;
    int detail;                 /* ext4 detail / header reason */
    char entry[L1_NAME_MAX];    /* the default= entry (also when it failed, once it was read) */
    char kver[L1_NAME_MAX];
    char cmdline[L1_CMDLINE_MAX + 1];
    char file[16];              /* the file a size/digest/missing refusal is about */
    uint64_t image_bytes, image_memory, initrd_bytes, dtb_bytes;
    uint32_t extra_ok;          /* bit x: L1_EXTRA x was found in the GPT with its exact range and PARTUUID */
    uint32_t kind;              /* L1_KIND_LINUX or L1_KIND_M1N1 (image_bytes = m1n1.bin bytes, at image_dst) */
    uint32_t oneshot;           /* 0 none, 1 the token chose this entry (and was spent), 2 an armed token could not be spent */
};
/* Runs the whole policy: GPT, ext4 (clean), boot-pending, loader.conf, the one-shot token, entry.conf, load + verify of
 * Image, initramfs.img and board.dtb (type=m1n1: m1n1.bin). On L1_OK they are at image_dst/initrd_dst/dtb_dst. The only
 * device write is the spent one-shot token (l1_dev_write1). */
int l1_run(struct l1_load *, struct l1_result *);
/* The part after the mount (boot-pending ... files); the caller has zeroed the result. */
int l1_run_fs(struct l1_load *, struct l1_ext4 *, struct l1_result *);
/* /chosen m4,boot-reason word for a status (detail: the ext4 detail). */
const char *l1_reason(enum l1_status, int detail);

/* GPT: finds p4; returns 0 or L1_GPT. */
int l1_gpt_find_p4(struct l1_dev *, uint8_t *buf8k, int *why, uint32_t *extra_ok);

/* Image header check shared with the loader. Returns 0 or non-zero reason. */
int l1_image_header_ok(const uint8_t *hdr64, uint64_t file_bytes, uint64_t window, uint64_t *image_memory);

/* CRC-32 (IEEE 802.3, reflected: zlib.crc32); the ARMv8 CRC32 instructions when built for them. */
uint32_t l1_crc32_fast(const void *, size_t);

#endif
