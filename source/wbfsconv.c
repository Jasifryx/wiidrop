#include "wbfsconv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef _MSC_VER
#include <io.h>
#define ftruncate(fd, len) _chsize_s(fd, (__int64)(len))
#define fileno _fileno
#else
#include <unistd.h>
#endif

#define HD_SEC         512
#define DISC_INFO_SIZE 9728 /* ALIGN(0x100 + 4482 * 2, 512) */
#define SAVE_EVERY     8    /* checkpoint every 8 blocks (16 MB of ISO) */
#define STATE_MAGIC    0x57434F4Eu /* "WCON" */
#define STATE_VERSION  1
#define MAX_DISC_BYTES ((uint64_t)WBFS_DISC_BLOCKS * WBFS_BLOCK_SIZE)

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

void wc_init(WbfsConv *c, SplitWriter *out, const char *statePath)
{
    memset(c, 0, sizeof(*c));
    c->out = out;
    snprintf(c->statePath, sizeof(c->statePath), "%s", statePath);
}

int wc_has_state(const char *statePath)
{
    struct stat st;
    return stat(statePath, &st) == 0;
}

/* ------------------------------------------------------------------ checkpoint */

typedef struct {
    uint32_t magic, version;
    uint32_t blockIndex, outBlocks;
    int32_t nParts, keepAll;
    uint64_t partOffset[WC_MAX_PARTS];
    uint64_t keepEnd[WC_MAX_PARTS];
    uint8_t discHeader[0x100];
    uint16_t wlba[WBFS_DISC_BLOCKS];
} State;

static int save_state(WbfsConv *c)
{
    if (c->out->fp) fflush(c->out->fp); /* output must be on disk before the checkpoint claims it */
    State *s = calloc(1, sizeof(State));
    if (!s) return -1;
    s->magic = STATE_MAGIC;
    s->version = STATE_VERSION;
    s->blockIndex = c->blockIndex;
    s->outBlocks = c->outBlocks;
    s->nParts = c->nParts;
    s->keepAll = c->keepAll;
    memcpy(s->partOffset, c->partOffset, sizeof(s->partOffset));
    memcpy(s->keepEnd, c->keepEnd, sizeof(s->keepEnd));
    memcpy(s->discHeader, c->discHeader, sizeof(s->discHeader));
    memcpy(s->wlba, c->wlba, sizeof(s->wlba));

    char tmp[620];
    snprintf(tmp, sizeof(tmp), "%s.tmp", c->statePath);
    FILE *f = fopen(tmp, "wb");
    int ok = f && fwrite(s, sizeof(State), 1, f) == 1;
    if (f && fclose(f) != 0) ok = 0;
    free(s);
    if (!ok) {
        remove(tmp);
        return -1;
    }
    remove(c->statePath);
    if (rename(tmp, c->statePath) != 0) return -1;
    if (c->onSave) c->onSave(c->onSaveUd, (uint64_t)c->blockIndex * WBFS_BLOCK_SIZE);
    return 0;
}

int wc_save(WbfsConv *c)
{
    return c->block ? save_state(c) : -1;
}

/* Cut the split output back to exactly `size` bytes (it may have run ahead of the checkpoint). */
static int truncate_output(SplitWriter *sw, uint64_t size)
{
    char path[600];
    int keepPart = (int)(size / sw->part_size);
    uint64_t keepLen = size % sw->part_size;
    if (keepLen == 0 && keepPart > 0) {
        keepPart--;
        keepLen = sw->part_size;
    }
    for (int part = keepPart + 1; part < 64; part++) {
        sw_part_path(sw, part, path, sizeof(path));
        if (remove(path) != 0) break;
    }
    sw_part_path(sw, keepPart, path, sizeof(path));
    FILE *f = fopen(path, "r+b");
    if (!f) return size == 0 ? 0 : -1;
    int rc = ftruncate(fileno(f), (off_t)keepLen);
    fclose(f);
    return rc == 0 ? 0 : -1;
}

uint64_t wc_resume_point(WbfsConv *c)
{
    FILE *f = fopen(c->statePath, "rb");
    if (!f) return 0;
    State *s = calloc(1, sizeof(State));
    int ok = s && fread(s, sizeof(State), 1, f) == 1;
    fclose(f);
    if (!ok || s->magic != STATE_MAGIC || s->version != STATE_VERSION || s->outBlocks < 1 ||
        s->blockIndex > WBFS_DISC_BLOCKS || s->nParts < 0 || s->nParts > WC_MAX_PARTS) {
        free(s);
        return 0;
    }
    uint64_t want = (uint64_t)s->outBlocks * WBFS_BLOCK_SIZE;
    uint64_t have = sw_existing_size(c->out);
    if (have < want || (have > want && truncate_output(c->out, want) != 0)) {
        free(s);
        return 0;
    }
    c->blockIndex = s->blockIndex;
    c->outBlocks = s->outBlocks;
    c->nParts = s->nParts;
    c->keepAll = s->keepAll;
    memcpy(c->partOffset, s->partOffset, sizeof(c->partOffset));
    memcpy(c->keepEnd, s->keepEnd, sizeof(c->keepEnd));
    memcpy(c->discHeader, s->discHeader, sizeof(c->discHeader));
    memcpy(c->wlba, s->wlba, sizeof(c->wlba));
    free(s);
    return (uint64_t)c->blockIndex * WBFS_BLOCK_SIZE;
}

/* ------------------------------------------------------------------ conversion */

int wc_open(WbfsConv *c, uint64_t isoOffset)
{
    if (!c->block) c->block = malloc(WBFS_BLOCK_SIZE);
    if (!c->block) {
        snprintf(c->err, sizeof(c->err), "Out of memory");
        return -1;
    }
    c->fill = 0;
    c->sinceSave = 0;
    if (isoOffset == 0) {
        /* fresh start: block 0 is a placeholder for the header, written at the end */
        memset(c->wlba, 0, sizeof(c->wlba));
        c->blockIndex = 0;
        c->outBlocks = 1;
        c->nParts = 0;
        c->keepAll = 0;
        remove(c->statePath);
        if (sw_open(c->out, 0) != 0) goto fail;
        memset(c->block, 0, WBFS_BLOCK_SIZE);
        if (sw_write(c->out, c->block, WBFS_BLOCK_SIZE) != 0) goto fail;
        return 0;
    }
    if (sw_open(c->out, (uint64_t)c->outBlocks * WBFS_BLOCK_SIZE) != 0) goto fail;
    return 0;
fail:
    snprintf(c->err, sizeof(c->err), "Could not create files on the SD card.");
    return -1;
}

uint64_t wc_iso_pos(const WbfsConv *c)
{
    return (uint64_t)c->blockIndex * WBFS_BLOCK_SIZE + c->fill;
}

/* Partition table at 0x40000: 4 groups of {count, table offset >> 2}; each entry {offset >> 2, type}. */
static int parse_partition_table(WbfsConv *c, const uint8_t *b, uint32_t len)
{
    if (len < 0x40020) return -1;
    for (int g = 0; g < 4; g++) {
        uint32_t count = be32(b + 0x40000 + g * 8);
        uint64_t table = (uint64_t)be32(b + 0x40004 + g * 8) << 2;
        if (count == 0) continue;
        if (count > WC_MAX_PARTS || table + count * 8 > len) return -1;
        for (uint32_t i = 0; i < count; i++) {
            if (c->nParts == WC_MAX_PARTS) return -1;
            uint64_t off = (uint64_t)be32(b + table + i * 8) << 2;
            if (off < 0x50000 || off >= MAX_DISC_BYTES) return -1;
            c->partOffset[c->nParts] = off;
            c->keepEnd[c->nParts] = 0;
            c->nParts++;
        }
    }
    return c->nParts > 0 ? 0 : -1;
}

static void process_block(WbfsConv *c, uint32_t len, int *error)
{
    uint8_t *b = c->block;
    uint64_t start = (uint64_t)c->blockIndex * WBFS_BLOCK_SIZE, end = start + len;

    if (c->blockIndex == 0) {
        if (len < 0x60 || be32(b + 0x18) != 0x5D1C9EA3u) {
            snprintf(c->err, sizeof(c->err), "Not a Wii disc image.");
            *error = 1;
            return;
        }
        memcpy(c->discHeader, b, 0x100);
        if (parse_partition_table(c, b, len) != 0) c->keepAll = 1; /* odd disc: keep everything */
    }

    /* partition headers that start in this block: data starts at +0x2B8 (>>2), size at +0x2BC (>>2) */
    for (int i = 0; i < c->nParts && !c->keepAll; i++) {
        uint64_t p = c->partOffset[i];
        if (c->keepEnd[i] || p < start || p >= end) continue;
        if (p + 0x2C0 > end) {
            c->keepAll = 1;
            break;
        }
        const uint8_t *h = b + (p - start);
        uint64_t dataOff = (uint64_t)be32(h + 0x2B8) << 2, dataSize = (uint64_t)be32(h + 0x2BC) << 2;
        if (dataOff < 0x2C0 || dataOff > 0x1000000 || dataSize == 0 || p + dataOff + dataSize > MAX_DISC_BYTES) {
            c->keepAll = 1;
            break;
        }
        c->keepEnd[i] = p + dataOff + dataSize;
    }

    int keep = c->blockIndex == 0 || c->keepAll;
    for (int i = 0; i < c->nParts && !keep; i++)
        if (c->keepEnd[i] && c->partOffset[i] < end && c->keepEnd[i] > start) keep = 1;

    if (keep) {
        if (len < WBFS_BLOCK_SIZE) memset(b + len, 0, WBFS_BLOCK_SIZE - len);
        if (sw_write(c->out, b, WBFS_BLOCK_SIZE) != 0) {
            snprintf(c->err, sizeof(c->err), "Writing to the SD card failed (full or removed?)");
            *error = 1;
            return;
        }
        c->wlba[c->blockIndex] = (uint16_t)c->outBlocks++;
    }
    c->blockIndex++;
    if (++c->sinceSave >= SAVE_EVERY) {
        c->sinceSave = 0;
        save_state(c); /* best effort: without it we just can't resume as far */
    }
}

int wc_write(WbfsConv *c, const void *data, size_t len)
{
    const uint8_t *p = data;
    while (len > 0) {
        if (c->blockIndex >= WBFS_DISC_BLOCKS) {
            snprintf(c->err, sizeof(c->err), "Disc image is bigger than a dual-layer Wii disc.");
            return -1;
        }
        uint32_t take = WBFS_BLOCK_SIZE - c->fill;
        if (take > len) take = (uint32_t)len;
        memcpy(c->block + c->fill, p, take);
        c->fill += take;
        p += take;
        len -= take;
        if (c->fill == WBFS_BLOCK_SIZE) {
            int error = 0;
            process_block(c, WBFS_BLOCK_SIZE, &error);
            c->fill = 0;
            if (error) return -1;
        }
    }
    return 0;
}

int wc_finish(WbfsConv *c)
{
    if (c->fill > 0) {
        int error = 0;
        process_block(c, c->fill, &error);
        c->fill = 0;
        if (error) return -1;
    }
    if (c->blockIndex == 0) {
        snprintf(c->err, sizeof(c->err), "The disc image is empty.");
        return -1;
    }
    if (sw_close(c->out) != 0) {
        snprintf(c->err, sizeof(c->err), "Could not finish writing to the SD card.");
        return -1;
    }

    /* header (hd sector 0) + disc info (hd sectors 1..19); the free-block bitmap stays all zero = full */
    uint8_t *h = calloc(1, HD_SEC + DISC_INFO_SIZE);
    if (!h) {
        snprintf(c->err, sizeof(c->err), "Out of memory");
        return -1;
    }
    memcpy(h, "WBFS", 4);
    put_be32(h + 4, c->outBlocks << (WBFS_BLOCK_SHIFT - 9)); /* n_hd_sec == file size / 512 */
    h[8] = 9;                                                 /* hd sector = 512 */
    h[9] = WBFS_BLOCK_SHIFT;                                  /* wbfs sector = 2 MB */
    h[12] = 1;                                                /* disc slot 0 in use */
    memcpy(h + HD_SEC, c->discHeader, 0x100);
    for (int i = 0; i < WBFS_DISC_BLOCKS; i++) {
        h[HD_SEC + 0x100 + i * 2] = (uint8_t)(c->wlba[i] >> 8);
        h[HD_SEC + 0x100 + i * 2 + 1] = (uint8_t)c->wlba[i];
    }

    char path[600];
    sw_part_path(c->out, 0, path, sizeof(path));
    FILE *f = fopen(path, "r+b");
    int ok = f && fwrite(h, 1, HD_SEC + DISC_INFO_SIZE, f) == HD_SEC + DISC_INFO_SIZE;
    if (f && fclose(f) != 0) ok = 0;
    free(h);
    free(c->block);
    c->block = NULL;
    if (!ok) {
        snprintf(c->err, sizeof(c->err), "Could not write the WBFS header.");
        return -1;
    }
    remove(c->statePath);
    return 0;
}

void wc_close(WbfsConv *c)
{
    sw_close(c->out);
    free(c->block);
    c->block = NULL;
}

void wc_discard(WbfsConv *c)
{
    wc_close(c);
    sw_remove_all(c->out);
    remove(c->statePath);
}
