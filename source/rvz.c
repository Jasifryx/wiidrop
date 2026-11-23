/*
 * RVZ / WIA reader. Format: Dolphin's docs/WiaAndRvz.md and Source/Core/DiscIO/WIABlob.cpp.
 * Rebuilds the disc sequentially: raw areas are decompressed (and RVZ-unpacked), Wii partition data
 * is decompressed, hashed (H0/H1/H2), patched with the stored hash exceptions and AES-encrypted.
 */
#include "rvz.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lzma/Lzma2Dec.h"
#include "lzma/LzmaDec.h"
#include "wiicrypto.h"
#include "zstd/zstd.h"
#ifdef HAVE_BZIP2
#include <bzlib.h>
#endif

#define SECTOR        0x8000u
#define SECTOR_DATA   0x7C00u
#define HASH_SIZE     0x400u
#define GROUP_SECTORS 64u
#define GROUP_DATA    (SECTOR_DATA * GROUP_SECTORS) /* 0x1F0000 */
#define MAX_EXC_LIST  (2 + 52 * 64 * 22)           /* Dolphin's MAX_SIZE_PER_EXCEPTION_LIST */

enum { C_NONE = 0, C_PURGE = 1, C_BZIP2 = 2, C_LZMA = 3, C_LZMA2 = 4, C_ZSTD = 5 };

typedef struct { uint32_t first, count, gi, ng; } Seg;
typedef struct { uint8_t key[16]; Seg seg[2]; } Part;
typedef struct { uint64_t off, size; uint32_t gi, ng; } Raw;
typedef struct { uint32_t off4, size, packed; } Group;
typedef struct { uint16_t off; uint8_t hash[20]; } Exc;

struct Rvz {
    RvzReadAt read;
    void *ud;
    int isRvz;
    uint8_t h2[0xDC];
    uint32_t discType, compression, chunk;
    uint8_t comprData[7], comprLen;
    uint64_t isoSize;
    Part *parts;
    uint32_t nParts;
    Raw *raws;
    uint32_t nRaws;
    Group *groups;
    uint32_t nGroups;

    /* decoded-chunk cache */
    int64_t cacheGroup;
    uint8_t *cin, *cdec, *cdata;
    size_t cinCap, cdecCap, cdataCap, cdataLen;
    uint8_t *excBytes; /* exception lists of the cached chunk */
    size_t excBytesLen, excBytesCap;
    uint32_t excLists;
    ZSTD_DCtx *zd;
    char *err;
    size_t errlen;
};

static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static uint64_t be64(const uint8_t *p) { return ((uint64_t)be32(p) << 32) | be32(p + 4); }

static int fail(Rvz *r, const char *msg)
{
    if (r && r->err) snprintf(r->err, r->errlen, "%s", msg);
    return -1;
}

static void *sz_alloc(ISzAllocPtr p, size_t n) { (void)p; return n ? malloc(n) : NULL; }
static void sz_free(ISzAllocPtr p, void *a) { (void)p; free(a); }
static const ISzAlloc g_alloc = {sz_alloc, sz_free};

static int grow(uint8_t **buf, size_t *cap, size_t need)
{
    if (*cap >= need) return 0;
    uint8_t *n = realloc(*buf, need);
    if (!n) return -1;
    *buf = n;
    *cap = need;
    return 0;
}

int rvz_is_rvz(const uint8_t *buf, size_t len)
{
    return len >= 4 && ((buf[0] == 'R' && buf[1] == 'V' && buf[2] == 'Z') || (buf[0] == 'W' && buf[1] == 'I' && buf[2] == 'A')) &&
           buf[3] == 1;
}

const uint8_t *rvz_peek_disc_header(const uint8_t *fileStart, size_t len)
{
    if (len < 0x48 + 0x10 + 0x80 || !rvz_is_rvz(fileStart, len)) return NULL;
    return fileStart + 0x48 + 0x10;
}

int rvz_is_wii(const Rvz *r) { return r->discType == 2; }
uint64_t rvz_disc_size(const Rvz *r) { return r->isoSize; }
const uint8_t *rvz_disc_header(const Rvz *r) { return r->h2 + 0x10; }

/* ------------------------------------------------------------------ decompression */

static int decompress(Rvz *r, uint32_t method, const uint8_t *in, size_t inLen, uint8_t *out, size_t cap, size_t *outLen)
{
    *outLen = 0;
    switch (method) {
    case C_NONE:
        if (inLen > cap) return fail(r, "RVZ: stored block too big");
        memcpy(out, in, inLen);
        *outLen = inLen;
        return 0;
    case C_ZSTD: {
        if (!r->zd && !(r->zd = ZSTD_createDCtx())) return fail(r, "Out of memory (zstd)");
        size_t n = ZSTD_decompressDCtx(r->zd, out, cap, in, inLen);
        if (ZSTD_isError(n)) return fail(r, "RVZ: zstd data is corrupt (incomplete download?)");
        *outLen = n;
        return 0;
    }
    case C_LZMA:
    case C_LZMA2: {
        SizeT dst = cap, src = inLen;
        ELzmaStatus st;
        SRes res;
        if (method == C_LZMA) {
            if (r->comprLen < 5) return fail(r, "RVZ: bad LZMA settings");
            CLzmaDec d;
            LzmaDec_CONSTRUCT(&d);
            if (LzmaDec_Allocate(&d, r->comprData, 5, &g_alloc) != SZ_OK) return fail(r, "Out of memory (LZMA)");
            LzmaDec_Init(&d);
            res = LzmaDec_DecodeToBuf(&d, out, &dst, in, &src, LZMA_FINISH_ANY, &st);
            LzmaDec_Free(&d, &g_alloc);
        } else {
            if (r->comprLen < 1) return fail(r, "RVZ: bad LZMA2 settings");
            CLzma2Dec d;
            Lzma2Dec_CONSTRUCT(&d);
            if (Lzma2Dec_Allocate(&d, r->comprData[0], &g_alloc) != SZ_OK) return fail(r, "Out of memory (LZMA2)");
            Lzma2Dec_Init(&d);
            res = Lzma2Dec_DecodeToBuf(&d, out, &dst, in, &src, LZMA_FINISH_ANY, &st);
            Lzma2Dec_Free(&d, &g_alloc);
        }
        if (res != SZ_OK) return fail(r, "RVZ: LZMA data is corrupt (incomplete download?)");
        *outLen = dst;
        return 0;
    }
    case C_BZIP2: {
#ifdef HAVE_BZIP2
        unsigned int dst = (unsigned int)cap;
        if (BZ2_bzBuffToBuffDecompress((char *)out, &dst, (char *)in, (unsigned int)inLen, 0, 0) != BZ_OK)
            return fail(r, "RVZ: bzip2 data is corrupt (incomplete download?)");
        *outLen = dst;
        return 0;
#else
        return fail(r, "RVZ: bzip2 compression isn't supported in this build.");
#endif
    }
    default:
        return fail(r, "RVZ: unsupported compression method. Re-convert with Zstandard in Dolphin.");
    }
}

/* ------------------------------------------------------------------ junk data (Dolphin's LFG) */

typedef struct {
    uint32_t buf[521]; /* words already in output form: bytes come out big-endian */
    size_t pos;        /* byte position in buf */
} Lfg;

static void lfg_forward(Lfg *g)
{
    for (int i = 0; i < 32; i++) g->buf[i] ^= g->buf[i + 521 - 32];
    for (int i = 32; i < 521; i++) g->buf[i] ^= g->buf[i - 32];
}

static void lfg_seed(Lfg *g, const uint8_t seed[68])
{
    for (int i = 0; i < 17; i++) g->buf[i] = be32(seed + i * 4);
    for (int i = 17; i < 521; i++) g->buf[i] = (g->buf[i - 17] << 23) ^ (g->buf[i - 16] >> 9) ^ g->buf[i - 1];
    for (int i = 0; i < 521; i++) { /* the "shift by 18 instead of 16" quirk, baked in */
        uint32_t x = g->buf[i];
        g->buf[i] = (x & 0xFF00FFFFu) | ((x >> 2) & 0x00FF0000u);
    }
    for (int i = 0; i < 4; i++) lfg_forward(g);
    g->pos = 0;
}

static void lfg_skip(Lfg *g, size_t n)
{
    g->pos += n;
    while (g->pos >= 521 * 4) {
        lfg_forward(g);
        g->pos -= 521 * 4;
    }
}

static void lfg_bytes(Lfg *g, uint8_t *out, size_t n)
{
    while (n > 0) {
        if ((g->pos & 3) == 0 && n >= 4 && g->pos + 4 <= 521 * 4) { /* fast path: whole words */
            size_t words = (521 * 4 - g->pos) / 4;
            if (words > n / 4) words = n / 4;
            const uint32_t *w = g->buf + g->pos / 4;
            for (size_t i = 0; i < words; i++) {
                out[0] = (uint8_t)(w[i] >> 24);
                out[1] = (uint8_t)(w[i] >> 16);
                out[2] = (uint8_t)(w[i] >> 8);
                out[3] = (uint8_t)w[i];
                out += 4;
            }
            n -= words * 4;
            g->pos += words * 4;
        } else {
            uint32_t w = g->buf[g->pos / 4];
            *out++ = (uint8_t)(w >> (24 - 8 * (g->pos & 3)));
            g->pos++;
            n--;
        }
        if (g->pos == 521 * 4) {
            lfg_forward(g);
            g->pos = 0;
        }
    }
}

/* RVZ packing: [u32 size][data] or [u32 size|0x80000000][68-byte seed] -> junk, repeated. */
static int rvz_unpack(Rvz *r, const uint8_t *in, size_t inLen, uint8_t *out, size_t outLen, uint64_t dataOffset)
{
    size_t ip = 0, op = 0;
    Lfg g;
    while (op < outLen) {
        if (ip + 4 > inLen) return fail(r, "RVZ: packed data is cut short");
        uint32_t size = be32(in + ip);
        ip += 4;
        int junk = (size & 0x80000000u) != 0;
        size &= 0x7FFFFFFFu;
        if (size > outLen - op) return fail(r, "RVZ: packed data is too long");
        if (junk) {
            if (ip + 68 > inLen) return fail(r, "RVZ: packed data is cut short");
            lfg_seed(&g, in + ip);
            ip += 68;
            lfg_skip(&g, (size_t)(dataOffset % SECTOR));
            lfg_bytes(&g, out + op, size);
        } else {
            if (ip + size > inLen) return fail(r, "RVZ: packed data is cut short");
            memcpy(out + op, in + ip, size);
            ip += size;
        }
        op += size;
        dataOffset += size;
    }
    return 0;
}

/* ------------------------------------------------------------------ groups (chunks) */

/* Decode group gidx into r->cdata (dataSize bytes). excLists: number of hash-exception lists in front. */
static int decode_group(Rvz *r, uint32_t gidx, size_t dataSize, uint32_t excLists, uint64_t dataOffset)
{
    if (r->cacheGroup == (int64_t)gidx) return 0;
    r->cacheGroup = -1;
    if (gidx >= r->nGroups) return fail(r, "RVZ: group index out of range");
    const Group *g = &r->groups[gidx];
    uint32_t size = g->size, packed = 0;
    uint32_t method = r->compression;
    if (r->isRvz) {
        if (!(size & 0x80000000u)) method = C_NONE;
        size &= 0x7FFFFFFFu;
        packed = g->packed;
    }
    if (grow(&r->cdata, &r->cdataCap, dataSize ? dataSize : 1)) return fail(r, "Out of memory");
    r->cdataLen = dataSize;
    r->excLists = excLists;
    r->excBytesLen = 0;

    if (size == 0) { /* all zeroes, no exceptions */
        memset(r->cdata, 0, dataSize);
        if (excLists) {
            if (grow(&r->excBytes, &r->excBytesCap, (size_t)excLists * 2)) return fail(r, "Out of memory");
            memset(r->excBytes, 0, (size_t)excLists * 2);
            r->excBytesLen = (size_t)excLists * 2;
        }
        r->cacheGroup = gidx;
        return 0;
    }

    if (grow(&r->cin, &r->cinCap, size)) return fail(r, "Out of memory");
    if (r->read(r->ud, (uint64_t)g->off4 << 2, r->cin, size) != 0) return fail(r, "RVZ: couldn't read the file (cut off?)");

    const uint8_t *stream;
    size_t streamLen;
    int inlineExc = (method == C_NONE || method == C_PURGE); /* stored uncompressed, padded to 4 */
    if (method == C_PURGE) return fail(r, "WIA: PURGE compression isn't supported. Re-convert to RVZ in Dolphin.");
    if (method == C_NONE) {
        stream = r->cin;
        streamLen = size;
    } else {
        size_t cap = (size_t)excLists * MAX_EXC_LIST + (packed ? packed : dataSize) + 64;
        if (grow(&r->cdec, &r->cdecCap, cap)) return fail(r, "Out of memory");
        if (decompress(r, method, r->cin, size, r->cdec, cap, &streamLen)) return -1;
        stream = r->cdec;
    }

    size_t pos = 0;
    for (uint32_t i = 0; i < excLists; i++) {
        if (pos + 2 > streamLen) return fail(r, "RVZ: hash exceptions are cut short");
        pos += 2 + (size_t)be16(stream + pos) * 22;
        if (inlineExc && i == excLists - 1) pos = (pos + 3) & ~(size_t)3;
        if (pos > streamLen) return fail(r, "RVZ: hash exceptions are cut short");
    }
    if (pos) {
        if (grow(&r->excBytes, &r->excBytesCap, pos)) return fail(r, "Out of memory");
        memcpy(r->excBytes, stream, pos);
        r->excBytesLen = pos;
    }

    const uint8_t *payload = stream + pos;
    size_t payloadLen = streamLen - pos;
    if (packed) {
        if (packed > payloadLen) return fail(r, "RVZ: packed data is cut short");
        if (rvz_unpack(r, payload, packed, r->cdata, dataSize, dataOffset)) return -1;
    } else {
        if (payloadLen < dataSize) return fail(r, "RVZ: block is cut short (incomplete download?)");
        memcpy(r->cdata, payload, dataSize);
    }
    r->cacheGroup = gidx;
    return 0;
}

/* Append exception list `index` of the cached chunk, shifting offsets by `add`. */
static int collect_exceptions(Rvz *r, uint32_t index, uint16_t add, Exc **list, size_t *n, size_t *cap)
{
    if (index >= r->excLists) return 0;
    const uint8_t *p = r->excBytes, *end = r->excBytes + r->excBytesLen;
    for (uint32_t i = 0; i < index; i++) {
        if (p + 2 > end) return fail(r, "RVZ: bad hash exceptions");
        p += 2 + (size_t)be16(p) * 22;
    }
    if (p + 2 > end) return fail(r, "RVZ: bad hash exceptions");
    uint16_t count = be16(p);
    p += 2;
    if (p + (size_t)count * 22 > end) return fail(r, "RVZ: bad hash exceptions");
    if (*n + count > *cap) {
        size_t nc = (*n + count) * 2 + 64;
        Exc *nl = realloc(*list, nc * sizeof(Exc));
        if (!nl) return fail(r, "Out of memory");
        *list = nl;
        *cap = nc;
    }
    for (uint16_t i = 0; i < count; i++, p += 22) {
        (*list)[*n].off = (uint16_t)(be16(p) + add);
        memcpy((*list)[*n].hash, p + 2, 20);
        (*n)++;
    }
    return 0;
}

/* ------------------------------------------------------------------ open / close */

static int read_table(Rvz *r, uint64_t off, uint32_t compSize, size_t rawSize, uint8_t **out)
{
    *out = NULL;
    if (rawSize == 0) return 0;
    uint8_t *in = malloc(compSize ? compSize : 1);
    uint8_t *dst = malloc(rawSize + 64);
    if (!in || !dst) {
        free(in);
        free(dst);
        return fail(r, "Out of memory");
    }
    size_t got = 0;
    int rc = r->read(r->ud, off, in, compSize) ? fail(r, "RVZ: couldn't read the file") : decompress(r, r->compression, in, compSize, dst, rawSize + 64, &got);
    free(in);
    if (rc == 0 && got < rawSize) rc = fail(r, "RVZ: table is cut short");
    if (rc) {
        free(dst);
        return -1;
    }
    *out = dst;
    return 0;
}

Rvz *rvz_open(RvzReadAt read, void *ud, char *err, size_t errlen)
{
    Rvz *r = calloc(1, sizeof(Rvz));
    if (!r) {
        snprintf(err, errlen, "Out of memory");
        return NULL;
    }
    r->read = read;
    r->ud = ud;
    r->err = err;
    r->errlen = errlen;
    r->cacheGroup = -1;

    uint8_t h1[0x48], digest[20];
    if (read(ud, 0, h1, sizeof(h1)) != 0 || !rvz_is_rvz(h1, sizeof(h1))) {
        fail(r, "Not an RVZ/WIA file.");
        goto bad;
    }
    r->isRvz = h1[0] == 'R';
    uint32_t version = be32(h1 + 4), compat = be32(h1 + 8), h2size = be32(h1 + 12);
    if (r->isRvz ? (0x01000000u < compat || 0x00030000u > version) : (0x01000000u < compat || 0x00080000u > version)) {
        fail(r, "This RVZ/WIA version isn't supported. Re-convert it with a current Dolphin.");
        goto bad;
    }
    sha1(h1, 0x34, digest);
    if (memcmp(digest, h1 + 0x34, 20) != 0) {
        fail(r, "RVZ header is corrupt (incomplete download?)");
        goto bad;
    }
    r->isoSize = be64(h1 + 0x24);
    if (h2size < 0xDC - 7 || h2size > 0x1000) {
        fail(r, "RVZ header is corrupt.");
        goto bad;
    }
    uint8_t *h2 = malloc(h2size);
    if (!h2 || read(ud, 0x48, h2, h2size) != 0) {
        free(h2);
        fail(r, "RVZ: couldn't read the file");
        goto bad;
    }
    sha1(h2, h2size, digest);
    if (memcmp(digest, h1 + 0x10, 20) != 0) {
        free(h2);
        fail(r, "RVZ header is corrupt (incomplete download?)");
        goto bad;
    }
    memcpy(r->h2, h2, h2size < 0xDC ? h2size : 0xDC);
    free(h2);

    r->discType = be32(r->h2 + 0);
    r->compression = be32(r->h2 + 4);
    r->chunk = be32(r->h2 + 12);
    r->comprLen = r->h2[0xD4] > 7 ? 7 : r->h2[0xD4];
    memcpy(r->comprData, r->h2 + 0xD5, 7);
    if (r->chunk < SECTOR || (r->chunk % SECTOR) != 0 || (r->chunk > 0x200000 && r->chunk % 0x200000) ||
        (r->chunk < 0x200000 && (r->chunk & (r->chunk - 1)))) {
        fail(r, "RVZ: unsupported chunk size.");
        goto bad;
    }

    /* partitions (stored uncompressed) */
    r->nParts = be32(r->h2 + 0x90);
    uint32_t partSize = be32(r->h2 + 0x94);
    if (r->nParts > 64 || (r->nParts && partSize < 0x30)) {
        fail(r, "RVZ: bad partition table.");
        goto bad;
    }
    if (r->nParts) {
        size_t bytes = (size_t)r->nParts * partSize;
        uint8_t *pt = malloc(bytes);
        r->parts = calloc(r->nParts, sizeof(Part));
        if (!pt || !r->parts || read(ud, be64(r->h2 + 0x98), pt, bytes) != 0) {
            free(pt);
            fail(r, "RVZ: couldn't read the partition table");
            goto bad;
        }
        sha1(pt, bytes, digest);
        if (memcmp(digest, r->h2 + 0xA0, 20) != 0) {
            free(pt);
            fail(r, "RVZ partition table is corrupt (incomplete download?)");
            goto bad;
        }
        for (uint32_t i = 0; i < r->nParts; i++) {
            const uint8_t *p = pt + (size_t)i * partSize;
            memcpy(r->parts[i].key, p, 16);
            for (int k = 0; k < 2; k++) {
                const uint8_t *s = p + 16 + k * 16;
                r->parts[i].seg[k] = (Seg){be32(s), be32(s + 4), be32(s + 8), be32(s + 12)};
            }
        }
        free(pt);
    }

    /* raw data entries and group entries (compressed tables) */
    r->nRaws = be32(r->h2 + 0xB4);
    r->nGroups = be32(r->h2 + 0xC4);
    if (r->nRaws > 1000000 || r->nGroups > 50000000) {
        fail(r, "RVZ: bad tables.");
        goto bad;
    }
    uint8_t *rt = NULL, *gt = NULL;
    size_t gsz = r->isRvz ? 12 : 8;
    if (read_table(r, be64(r->h2 + 0xB8), be32(r->h2 + 0xC0), (size_t)r->nRaws * 24, &rt) ||
        read_table(r, be64(r->h2 + 0xC8), be32(r->h2 + 0xD0), (size_t)r->nGroups * gsz, &gt)) {
        free(rt);
        goto bad;
    }
    r->raws = calloc(r->nRaws ? r->nRaws : 1, sizeof(Raw));
    r->groups = calloc(r->nGroups ? r->nGroups : 1, sizeof(Group));
    if (!r->raws || !r->groups) {
        free(rt);
        free(gt);
        fail(r, "Out of memory");
        goto bad;
    }
    for (uint32_t i = 0; i < r->nRaws; i++) {
        const uint8_t *p = rt + (size_t)i * 24;
        r->raws[i] = (Raw){be64(p), be64(p + 8), be32(p + 16), be32(p + 20)};
    }
    for (uint32_t i = 0; i < r->nGroups; i++) {
        const uint8_t *p = gt + i * gsz;
        r->groups[i].off4 = be32(p);
        r->groups[i].size = be32(p + 4);
        r->groups[i].packed = r->isRvz ? be32(p + 8) : 0;
    }
    free(rt);
    free(gt);
    return r;

bad:
    rvz_close(r);
    return NULL;
}

void rvz_close(Rvz *r)
{
    if (!r) return;
    free(r->parts);
    free(r->raws);
    free(r->groups);
    free(r->cin);
    free(r->cdec);
    free(r->cdata);
    free(r->excBytes);
    if (r->zd) ZSTD_freeDCtx(r->zd);
    free(r);
}

/* ------------------------------------------------------------------ rebuilding the disc */

typedef struct {
    Rvz *r;
    RvzOut out;
    RvzProgress progress;
    void *ud;
    uint64_t cursor; /* disc bytes emitted so far */
    uint64_t lastProgress;
    int cancelled;
} Emit;

/* Emit disc bytes [at, at+len) taken from data; parts before the cursor are skipped. */
static int emit(Emit *e, uint64_t at, const uint8_t *data, size_t len)
{
    if (at + len <= e->cursor) return 0;
    if (at < e->cursor) {
        size_t skip = (size_t)(e->cursor - at);
        data += skip;
        len -= skip;
        at = e->cursor;
    }
    if (at > e->cursor) return fail(e->r, "RVZ: internal ordering error");
    /* the first 0x80 bytes come from the header (the first raw block's copy is not authoritative) */
    if (at < 0x80) {
        size_t n = (size_t)(0x80 - at) < len ? (size_t)(0x80 - at) : len;
        if (e->out(e->ud, e->r->h2 + 0x10 + at, n)) return fail(e->r, "");
        data += n;
        len -= n;
        at += n;
    }
    if (len && e->out(e->ud, data, len)) return fail(e->r, "");
    e->cursor = at + len;
    if (e->progress && e->cursor - e->lastProgress >= (4u << 20)) {
        e->lastProgress = e->cursor;
        if (e->progress(e->ud, e->cursor, e->r->isoSize)) {
            e->cancelled = 1;
            return -1;
        }
    }
    return 0;
}

static int emit_zeros(Emit *e, uint64_t upTo)
{
    static const uint8_t zeros[65536];
    while (e->cursor < upTo) {
        size_t n = upTo - e->cursor < sizeof(zeros) ? (size_t)(upTo - e->cursor) : sizeof(zeros);
        if (emit(e, e->cursor, zeros, n)) return -1;
    }
    return 0;
}

static int emit_raw(Emit *e, const Raw *raw)
{
    Rvz *r = e->r;
    uint64_t start = raw->off - raw->off % SECTOR, size = raw->size + raw->off % SECTOR;
    for (uint32_t i = 0; i < raw->ng; i++) {
        uint64_t off = (uint64_t)i * r->chunk;
        if (off >= size) break;
        size_t len = (size_t)(size - off < r->chunk ? size - off : r->chunk);
        if (start + off + len <= e->cursor) continue;
        if (decode_group(r, raw->gi + i, len, 0, start + off)) return -1;
        if (emit(e, start + off, r->cdata, len)) return -1;
    }
    return 0;
}

/* Hash a 2 MB Wii group (64 blocks of 0x7C00) into 64 hash blocks of 0x400 (H0, H1, H2). */
static void hash_group(const uint8_t *data, uint8_t *hb)
{
    memset(hb, 0, GROUP_SECTORS * HASH_SIZE);
    for (uint32_t i = 0; i < GROUP_SECTORS; i++) {
        uint8_t *b = hb + i * HASH_SIZE;
        for (int j = 0; j < 31; j++) sha1(data + (size_t)i * SECTOR_DATA + j * 0x400, 0x400, b + j * 20);
        uint32_t base = i & ~7u;
        sha1(b, 31 * 20, hb + base * HASH_SIZE + 0x280 + (i - base) * 20); /* H1 */
    }
    for (uint32_t sub = 0; sub < 8; sub++) {
        uint8_t *first = hb + sub * 8 * HASH_SIZE;
        for (int j = 1; j < 8; j++) memcpy(first + j * HASH_SIZE + 0x280, first + 0x280, 160);
        sha1(first + 0x280, 160, hb + 0x340 + sub * 20); /* H2 */
    }
    for (uint32_t i = 1; i < GROUP_SECTORS; i++) memcpy(hb + i * HASH_SIZE + 0x340, hb + 0x340, 160);
}

/* ---- worker threads: hashing + AES for Wii groups run on the other CPU cores ----
   The main thread decompresses each 2 MB group (serial: it shares the decoder state) and writes the
   result out in order; up to RVZ_PIPE groups are hashed and encrypted in parallel meanwhile. */

#define RVZ_PIPE    4
#define RVZ_WORKERS 2

#if defined(__WIIU__)
#include <coreinit/condition.h>
#include <coreinit/mutex.h>
#include <coreinit/thread.h>
#include <malloc.h>
typedef OSMutex RMutex;
typedef OSCondition RCond;
#define rm_init(m)    OSInitMutex(m)
#define rm_lock(m)    OSLockMutex(m)
#define rm_unlock(m)  OSUnlockMutex(m)
#define rc_init(c)    OSInitCond(c)
#define rc_wait(c, m) OSWaitCond(c, m)
#define rc_wake(c)    OSSignalCond(c) /* wakes every waiter */
#define rm_destroy(m) ((void)0)
#define rc_destroy(c) ((void)0)
typedef struct {
    OSThread *t;
    uint8_t *stack;
} RThread;
#define RVZ_STACK (128 * 1024)
static int rt_entry(int argc, void *argv);
static int rt_start(RThread *th, void *arg, int core)
{
    th->t = memalign(16, sizeof(OSThread));
    th->stack = memalign(16, RVZ_STACK);
    if (!th->t || !th->stack) return -1;
    memset(th->t, 0, sizeof(OSThread));
    /* cores 0 and 2: the app's main thread runs on core 1 */
    OSThreadAttributes aff = core == 0 ? OS_THREAD_ATTRIB_AFFINITY_CPU0 : OS_THREAD_ATTRIB_AFFINITY_CPU2;
    if (!OSCreateThread(th->t, rt_entry, 0, arg, th->stack + RVZ_STACK, RVZ_STACK, 16, aff)) return -1;
    OSResumeThread(th->t);
    return 0;
}
static void rt_join(RThread *th)
{
    if (th->t) {
        int ret;
        OSJoinThread(th->t, &ret);
    }
    free(th->t);
    free(th->stack);
}
#elif defined(_WIN32)
#include <windows.h>
typedef CRITICAL_SECTION RMutex;
typedef CONDITION_VARIABLE RCond;
#define rm_init(m)    InitializeCriticalSection(m)
#define rm_lock(m)    EnterCriticalSection(m)
#define rm_unlock(m)  LeaveCriticalSection(m)
#define rc_init(c)    InitializeConditionVariable(c)
#define rc_wait(c, m) SleepConditionVariableCS(c, m, INFINITE)
#define rc_wake(c)    WakeAllConditionVariable(c)
#define rm_destroy(m) DeleteCriticalSection(m)
#define rc_destroy(c) ((void)0)
typedef struct {
    HANDLE h;
} RThread;
static int rt_entry(int argc, void *argv);
static DWORD WINAPI rt_win(LPVOID arg)
{
    return (DWORD)rt_entry(0, arg);
}
static int rt_start(RThread *th, void *arg, int core)
{
    (void)core;
    th->h = CreateThread(NULL, 0, rt_win, arg, 0, NULL);
    return th->h ? 0 : -1;
}
static void rt_join(RThread *th)
{
    if (th->h) {
        WaitForSingleObject(th->h, INFINITE);
        CloseHandle(th->h);
    }
}
#else
#include <pthread.h>
typedef pthread_mutex_t RMutex;
typedef pthread_cond_t RCond;
#define rm_init(m)    pthread_mutex_init(m, NULL)
#define rm_lock(m)    pthread_mutex_lock(m)
#define rm_unlock(m)  pthread_mutex_unlock(m)
#define rc_init(c)    pthread_cond_init(c, NULL)
#define rc_wait(c, m) pthread_cond_wait(c, m)
#define rc_wake(c)    pthread_cond_broadcast(c)
#define rm_destroy(m) pthread_mutex_destroy(m)
#define rc_destroy(c) pthread_cond_destroy(c)
typedef struct {
    pthread_t t;
    int ok;
} RThread;
static int rt_entry(int argc, void *argv);
static void *rt_posix(void *arg)
{
    rt_entry(0, arg);
    return NULL;
}
static int rt_start(RThread *th, void *arg, int core)
{
    (void)core;
    th->ok = pthread_create(&th->t, NULL, rt_posix, arg) == 0;
    return th->ok ? 0 : -1;
}
static void rt_join(RThread *th)
{
    if (th->ok) pthread_join(th->t, NULL);
}
#endif

enum { J_FREE, J_READY, J_WORKING, J_DONE };

typedef struct {
    int state;
    uint32_t s0, n;
    uint8_t *data, *hb, *out;
    Exc *exc;
    size_t nexc, excCap;
    int bad; /* a hash exception pointed outside the group */
} GJob;

typedef struct {
    RMutex m;
    RCond c;
    int stop;
    Aes128 aes;
    GJob job[RVZ_PIPE];
} Pipe;

/* hash, patch and encrypt one group: data -> out (n sectors of 0x8000) */
static void finish_group(const Aes128 *aes, GJob *j)
{
    static const uint8_t zeroIv[16];
    hash_group(j->data, j->hb);
    for (size_t i = 0; i < j->nexc; i++) {
        uint32_t blk = j->exc[i].off / HASH_SIZE, in = j->exc[i].off % HASH_SIZE;
        if (blk >= GROUP_SECTORS || in + 20 > HASH_SIZE) {
            j->bad = 1;
            return;
        }
        memcpy(j->hb + blk * HASH_SIZE + in, j->exc[i].hash, 20);
    }
    for (uint32_t b = 0; b < j->n; b++) {
        uint8_t *o = j->out + (size_t)b * SECTOR;
        aes128_cbc_encrypt(aes, zeroIv, j->hb + b * HASH_SIZE, o, HASH_SIZE);
        aes128_cbc_encrypt(aes, o + 0x3D0, j->data + (size_t)b * SECTOR_DATA, o + HASH_SIZE, SECTOR_DATA);
    }
}

static int rt_entry(int argc, void *argv)
{
    (void)argc;
    Pipe *pp = (Pipe *)argv;
    rm_lock(&pp->m);
    for (;;) {
        GJob *j = NULL;
        while (!pp->stop) {
            for (int i = 0; i < RVZ_PIPE && !j; i++)
                if (pp->job[i].state == J_READY) j = &pp->job[i];
            if (j) break;
            rc_wait(&pp->c, &pp->m);
        }
        if (!j) break; /* stopping */
        j->state = J_WORKING;
        rm_unlock(&pp->m);
        finish_group(&pp->aes, j);
        rm_lock(&pp->m);
        j->state = J_DONE;
        rc_wake(&pp->c);
    }
    rm_unlock(&pp->m);
    return 0;
}

/* write a finished group out (sectors before the cursor were already written) */
static int emit_group(Emit *e, uint64_t discStart, GJob *j)
{
    if (j->bad) return fail(e->r, "RVZ: bad hash exception");
    for (uint32_t b = 0; b < j->n; b++) {
        uint64_t at = discStart + (uint64_t)(j->s0 + b) * SECTOR;
        if (at + SECTOR <= e->cursor) continue;
        if (emit(e, at, j->out + (size_t)b * SECTOR, SECTOR)) return -1;
    }
    return 0;
}

/* decompress Wii group w into j->data and collect its hash exceptions */
static int fill_group(Rvz *r, const Part *p, uint32_t w, uint32_t total, GJob *j)
{
    const uint32_t first = p->seg[0].first;
    const size_t chunkData = (size_t)r->chunk / SECTOR * SECTOR_DATA;
    const uint32_t excLists = chunkData / GROUP_DATA > 1 ? (uint32_t)(chunkData / GROUP_DATA) : 1;
    uint32_t s0 = w * GROUP_SECTORS;
    uint32_t n = total - s0 < GROUP_SECTORS ? total - s0 : GROUP_SECTORS;
    j->s0 = s0;
    j->n = n;
    j->bad = 0;
    j->nexc = 0;
    memset(j->data, 0, GROUP_DATA);
    int64_t lastKey = -1;
    for (uint32_t b = 0; b < n; b++) {
        uint32_t s = s0 + b; /* sector relative to the partition data */
        const Seg *seg = NULL;
        uint32_t segRel = 0;
        for (int k = 0; k < 2; k++) {
            uint32_t rel = p->seg[k].first - first;
            if (p->seg[k].count && s >= rel && s < rel + p->seg[k].count) {
                seg = &p->seg[k];
                segRel = rel;
                break;
            }
        }
        if (!seg) continue; /* gap: zeroes */
        uint64_t offInSeg = (uint64_t)(s - segRel) * SECTOR_DATA;
        uint32_t jx = (uint32_t)(offInSeg / chunkData);
        if (jx >= seg->ng) return fail(r, "RVZ: partition data is missing");
        uint64_t segDataSize = (uint64_t)seg->count * SECTOR_DATA;
        size_t len = (size_t)(segDataSize - (uint64_t)jx * chunkData < chunkData ? segDataSize - (uint64_t)jx * chunkData
                                                                                 : chunkData);
        uint64_t chunkPartOff = (uint64_t)segRel * SECTOR_DATA + (uint64_t)jx * chunkData; /* partition-relative */
        if (decode_group(r, seg->gi + jx, len, excLists, chunkPartOff)) return -1;
        int64_t key = ((int64_t)(seg - p->seg) << 40) | jx;
        if (key != lastKey) { /* this chunk's exceptions for Wii group w */
            lastKey = key;
            int rc;
            if (excLists > 1) {
                uint64_t firstGroup = chunkPartOff / GROUP_DATA;
                rc = collect_exceptions(r, (uint32_t)(w - firstGroup), 0, &j->exc, &j->nexc, &j->excCap);
            } else {
                uint16_t add = (uint16_t)((chunkPartOff % GROUP_DATA) / SECTOR_DATA * HASH_SIZE);
                rc = collect_exceptions(r, 0, add, &j->exc, &j->nexc, &j->excCap);
            }
            if (rc) return rc;
        }
        size_t inChunk = (size_t)(offInSeg - (uint64_t)jx * chunkData);
        if (inChunk + SECTOR_DATA > r->cdataLen) return fail(r, "RVZ: block is cut short");
        memcpy(j->data + (size_t)b * SECTOR_DATA, r->cdata + inChunk, SECTOR_DATA);
    }
    return 0;
}

static int emit_partition(Emit *e, const Part *p)
{
    Rvz *r = e->r;
    const uint64_t discStart = (uint64_t)p->seg[0].first * SECTOR;
    const uint32_t total = p->seg[1].count ? p->seg[1].first - p->seg[0].first + p->seg[1].count : p->seg[0].count;
    static Pipe pp; /* one rebuild at a time */
    memset(&pp, 0, sizeof(pp));
    aes128_init(&pp.aes, p->key);
    int rc = 0, njobs = RVZ_PIPE;
    for (int i = 0; i < RVZ_PIPE; i++) {
        GJob *j = &pp.job[i];
        j->data = malloc(GROUP_DATA);
        j->hb = malloc(GROUP_SECTORS * HASH_SIZE);
        j->out = malloc((size_t)GROUP_SECTORS * SECTOR);
        if (!j->data || !j->hb || !j->out) {
            njobs = i;
            break;
        }
    }
    if (njobs < 1) {
        rc = fail(r, "Out of memory");
        goto done;
    }

    /* workers (none if they can't start: then everything runs here, one group at a time) */
    rm_init(&pp.m);
    rc_init(&pp.c);
    RThread th[RVZ_WORKERS];
    memset(th, 0, sizeof(th));
    int nth = 0;
    if (njobs >= 2)
        for (int i = 0; i < RVZ_WORKERS; i++)
            if (rt_start(&th[nth], &pp, i == 0 ? 0 : 2) == 0) nth++;

    int fillAt = 0, emitAt = 0, inFlight = 0;
    for (uint32_t w = 0; w * GROUP_SECTORS < total && rc == 0; w++) {
        uint32_t s0 = w * GROUP_SECTORS;
        uint32_t n = total - s0 < GROUP_SECTORS ? total - s0 : GROUP_SECTORS;
        if (discStart + (uint64_t)(s0 + n) * SECTOR <= e->cursor) continue;
        GJob *j = &pp.job[fillAt];
        if (nth == 0) { /* single-threaded */
            rc = fill_group(r, p, w, total, j);
            if (rc == 0) finish_group(&pp.aes, j);
            if (rc == 0) rc = emit_group(e, discStart, j);
            continue;
        }
        /* the slot is busy: write out finished groups (in order) until it frees up */
        while (rc == 0 && inFlight == njobs) {
            GJob *o = &pp.job[emitAt];
            rm_lock(&pp.m);
            while (o->state != J_DONE) rc_wait(&pp.c, &pp.m);
            rm_unlock(&pp.m);
            rc = emit_group(e, discStart, o);
            o->state = J_FREE;
            emitAt = (emitAt + 1) % njobs;
            inFlight--;
        }
        if (rc) break;
        rc = fill_group(r, p, w, total, j);
        if (rc) break;
        rm_lock(&pp.m);
        j->state = J_READY;
        rc_wake(&pp.c);
        rm_unlock(&pp.m);
        fillAt = (fillAt + 1) % njobs;
        inFlight++;
    }
    /* drain */
    while (rc == 0 && inFlight > 0) {
        GJob *o = &pp.job[emitAt];
        rm_lock(&pp.m);
        while (o->state != J_DONE) rc_wait(&pp.c, &pp.m);
        rm_unlock(&pp.m);
        rc = emit_group(e, discStart, o);
        o->state = J_FREE;
        emitAt = (emitAt + 1) % njobs;
        inFlight--;
    }
    rm_lock(&pp.m);
    pp.stop = 1;
    rc_wake(&pp.c);
    rm_unlock(&pp.m);
    for (int i = 0; i < nth; i++) rt_join(&th[i]);
    rm_destroy(&pp.m);
    rc_destroy(&pp.c);
done:
    for (int i = 0; i < RVZ_PIPE; i++) {
        free(pp.job[i].data);
        free(pp.job[i].hb);
        free(pp.job[i].out);
        free(pp.job[i].exc);
    }
    return rc;
}

typedef struct {
    uint64_t start;
    int isPart;
    uint32_t index;
} Region;

static int region_cmp(const void *a, const void *b)
{
    const Region *x = a, *y = b;
    return x->start < y->start ? -1 : x->start > y->start;
}

int rvz_extract(Rvz *r, RvzOut out, RvzProgress progress, void *ud, char *err, size_t errlen)
{
    r->err = err;
    r->errlen = errlen;
    err[0] = '\0';
    size_t nreg = 0;
    Region *reg = calloc((size_t)r->nRaws + r->nParts + 1, sizeof(Region));
    if (!reg) return fail(r, "Out of memory");
    for (uint32_t i = 0; i < r->nRaws; i++)
        if (r->raws[i].size) reg[nreg++] = (Region){r->raws[i].off - r->raws[i].off % SECTOR, 0, i};
    for (uint32_t i = 0; i < r->nParts; i++)
        if (r->parts[i].seg[0].count) reg[nreg++] = (Region){(uint64_t)r->parts[i].seg[0].first * SECTOR, 1, i};
    qsort(reg, nreg, sizeof(Region), region_cmp);

    Emit e = {r, out, progress, ud, 0, 0, 0};
    int rc = 0;
    for (size_t i = 0; i < nreg && rc == 0; i++) {
        if (reg[i].start > e.cursor) rc = emit_zeros(&e, reg[i].start);
        if (rc == 0) rc = reg[i].isPart ? emit_partition(&e, &r->parts[reg[i].index]) : emit_raw(&e, &r->raws[reg[i].index]);
    }
    if (rc == 0 && e.cursor < r->isoSize) rc = emit_zeros(&e, r->isoSize);
    if (rc == 0 && e.cursor != r->isoSize) rc = fail(r, "RVZ: rebuilt disc has the wrong size");
    free(reg);
    if (e.cancelled) return 1;
    if (rc == 0 && progress) progress(ud, r->isoSize, r->isoSize);
    return rc;
}
