#include "zipstream.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define CK_MAGIC   0x5A43504Bu
#define CK_VERSION 1
#define NCAND      4
#define CAND_EVERY (1u << 20) /* a resume point at most every 1 MB of output */
#define OUT_CHUNK  65536
#define HBUF_SIZE  (30 + 65535 + 65535)

enum { ST_HEADER, ST_SKIP, ST_DATA, ST_DESC, ST_DONE };

struct ZipStream {
    ZipSink sink;
    int st;
    uint8_t *hbuf;
    size_t hlen, hneed;
    uint64_t inPos;
    uint8_t prevLast; /* last byte of the previous feed (for bit-level resume points) */

    /* current entry */
    int chosen, found;
    uint16_t flags, method;
    uint32_t crcExpected;
    uint64_t csize, usize, compLeft;
    int zip64, sizeKnown;
    char name[256];

    z_stream zs;
    int zInit;
    uint64_t outPos, skipTo;
    uint32_t crc;
    uint8_t window[ZIP_WINDOW];
    uint32_t winPos;
    uint64_t winTotal;
    uint8_t *out;

    ZipCkpt *cand;
    int ncand, candNext;
    uint64_t lastCand;
    char err[200];
};

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint64_t le64(const uint8_t *p) { return le32(p) | ((uint64_t)le32(p + 4) << 32); }

int zs_is_zip(const uint8_t *buf, size_t len)
{
    return len >= 4 && buf[0] == 'P' && buf[1] == 'K' && buf[2] == 3 && buf[3] == 4;
}

ZipStream *zs_create(const ZipSink *sink)
{
    ZipStream *z = calloc(1, sizeof(ZipStream));
    if (!z) return NULL;
    z->sink = *sink;
    z->hbuf = malloc(HBUF_SIZE);
    z->out = malloc(OUT_CHUNK);
    z->cand = calloc(NCAND, sizeof(ZipCkpt));
    if (!z->hbuf || !z->out || !z->cand) {
        zs_destroy(z);
        return NULL;
    }
    z->st = ST_HEADER;
    return z;
}

void zs_destroy(ZipStream *z)
{
    if (!z) return;
    if (z->zInit) inflateEnd(&z->zs);
    free(z->hbuf);
    free(z->out);
    free(z->cand);
    free(z);
}

const char *zs_error(const ZipStream *z) { return z->err; }
uint64_t zs_in_pos(const ZipStream *z) { return z->inPos; }

static int fail(ZipStream *z, const char *msg)
{
    snprintf(z->err, sizeof(z->err), "%s", msg);
    return -1;
}

static int ends_with_ci(const char *s, const char *suf)
{
    size_t a = strlen(s), b = strlen(suf);
    if (a < b) return 0;
    for (size_t i = 0; i < b; i++)
        if (tolower((unsigned char)s[a - b + i]) != suf[i]) return 0;
    return 1;
}

static int inflate_start(ZipStream *z)
{
    int rc = z->zInit ? inflateReset(&z->zs) : inflateInit2(&z->zs, -15);
    if (rc != Z_OK) return fail(z, "Out of memory (zip)");
    z->zInit = 1;
    return 0;
}

/* ------------------------------------------------------------------ resume points */

static void add_candidate(ZipStream *z, uint64_t inOffset, int bits, uint8_t lastByte)
{
    ZipCkpt *c = &z->cand[z->candNext];
    z->candNext = (z->candNext + 1) % NCAND;
    if (z->ncand < NCAND) z->ncand++;
    memset(c, 0, offsetof(ZipCkpt, window));
    c->magic = CK_MAGIC;
    c->version = CK_VERSION;
    c->inOffset = inOffset;
    c->bits = (uint8_t)bits;
    c->lastByte = lastByte;
    c->method = z->method;
    c->flags = z->flags;
    c->crcExpected = z->crcExpected;
    c->crc = z->crc;
    c->csize = z->csize;
    c->usize = z->usize;
    c->compLeft = z->compLeft;
    c->outPos = z->outPos;
    c->zip64 = z->zip64;
    c->sizeKnown = z->sizeKnown;
    snprintf(c->name, sizeof(c->name), "%s", z->name);
    /* linearise the last 32 KB of output */
    uint32_t len = z->winTotal < ZIP_WINDOW ? (uint32_t)z->winTotal : ZIP_WINDOW;
    c->windowLen = len;
    uint32_t start = (z->winPos + ZIP_WINDOW - len) % ZIP_WINDOW;
    uint32_t first = ZIP_WINDOW - start < len ? ZIP_WINDOW - start : len;
    memcpy(c->window, z->window + start, first);
    memcpy(c->window + first, z->window, len - first);
    z->lastCand = z->outPos;
}

int zs_checkpoint(const ZipStream *z, uint64_t maxOut, ZipCkpt *out)
{
    const ZipCkpt *best = NULL;
    for (int i = 0; i < z->ncand; i++) {
        const ZipCkpt *c = &z->cand[i];
        if (c->outPos <= maxOut && (!best || c->outPos > best->outPos)) best = c;
    }
    if (!best) return -1;
    *out = *best;
    return 0;
}

int zs_save_ckpt(const char *path, const ZipCkpt *ck)
{
    char tmp[620];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    int ok = f && fwrite(ck, sizeof(*ck), 1, f) == 1;
    if (f && fclose(f) != 0) ok = 0;
    if (!ok) {
        remove(tmp);
        return -1;
    }
    remove(path);
    return rename(tmp, path);
}

int zs_load_ckpt(const char *path, ZipCkpt *ck)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    int ok = fread(ck, sizeof(*ck), 1, f) == 1;
    fclose(f);
    if (!ok || ck->magic != CK_MAGIC || ck->version != CK_VERSION || ck->windowLen > ZIP_WINDOW ||
        ck->bits > 7 || (ck->method != 0 && ck->method != 8))
        return -1;
    ck->name[sizeof(ck->name) - 1] = '\0';
    return 0;
}

int zs_resume(ZipStream *z, const ZipCkpt *ck, uint64_t skipTo)
{
    if (skipTo < ck->outPos) return fail(z, "bad resume point");
    z->chosen = 1;
    z->found = 0;
    z->st = ST_DATA;
    z->inPos = ck->inOffset;
    z->prevLast = ck->lastByte;
    z->method = ck->method;
    z->flags = ck->flags;
    z->crcExpected = ck->crcExpected;
    z->crc = ck->crc;
    z->csize = ck->csize;
    z->usize = ck->usize;
    z->compLeft = ck->compLeft;
    z->zip64 = ck->zip64;
    z->sizeKnown = ck->sizeKnown;
    z->outPos = ck->outPos;
    z->skipTo = skipTo;
    snprintf(z->name, sizeof(z->name), "%s", ck->name);
    memcpy(z->window, ck->window, ck->windowLen);
    z->winPos = ck->windowLen % ZIP_WINDOW;
    z->winTotal = ck->windowLen;
    if (z->method == 8) {
        if (inflate_start(z) != 0) return -1;
        if (ck->bits && inflatePrime(&z->zs, ck->bits, ck->lastByte >> (8 - ck->bits)) != Z_OK)
            return fail(z, "bad resume point");
        if (ck->windowLen && inflateSetDictionary(&z->zs, ck->window, ck->windowLen) != Z_OK)
            return fail(z, "bad resume point");
    }
    z->ncand = 0;
    z->candNext = 0;
    z->cand[0] = *ck;
    z->ncand = 1;
    z->candNext = 1;
    z->lastCand = ck->outPos;
    return 0;
}

/* ------------------------------------------------------------------ entry handling */

static int emit(ZipStream *z, const uint8_t *buf, size_t n)
{
    if (!z->chosen || n == 0) return 0;
    z->crc = (uint32_t)crc32(z->crc, buf, (uInt)n);
    /* keep the last 32 KB for resume points */
    const uint8_t *w = buf;
    size_t wn = n;
    if (wn > ZIP_WINDOW) {
        w += wn - ZIP_WINDOW;
        wn = ZIP_WINDOW;
    }
    size_t first = ZIP_WINDOW - z->winPos < wn ? ZIP_WINDOW - z->winPos : wn;
    memcpy(z->window + z->winPos, w, first);
    memcpy(z->window, w + first, wn - first);
    z->winPos = (uint32_t)((z->winPos + wn) % ZIP_WINDOW);
    z->winTotal += n;

    uint64_t start = z->outPos, end = start + n;
    z->outPos = end;
    if (end > z->skipTo) {
        size_t off = z->skipTo > start ? (size_t)(z->skipTo - start) : 0;
        if (z->sink.write(z->sink.ud, buf + off, n - off) != 0) return fail(z, "");
    }
    return 0;
}

static int close_entry(ZipStream *z, uint32_t crc)
{
    if (z->chosen) {
        if (z->crc != crc) return fail(z, "CRC mismatch: the zip download is corrupt. Pick the game again to retry.");
        if (z->sizeKnown && z->outPos != z->usize) return fail(z, "The game file in the zip is incomplete.");
        z->found = 1;
        z->chosen = 0;
        z->st = ST_DONE; /* we have what we came for; anything after it is ignored */
    } else {
        z->st = ST_HEADER;
    }
    z->hlen = z->hneed = 0;
    return 0;
}

static int end_entry_data(ZipStream *z)
{
    if (z->flags & 8) { /* sizes and CRC follow in a data descriptor */
        z->st = ST_DESC;
        z->hlen = z->hneed = 0;
        return 0;
    }
    return close_entry(z, z->crcExpected);
}

static int parse_entry(ZipStream *z, uint64_t dataStart)
{
    const uint8_t *h = z->hbuf;
    uint16_t n = le16(h + 26), e = le16(h + 28);
    uint32_t cs = le32(h + 18), us = le32(h + 22);
    z->flags = le16(h + 6);
    z->method = le16(h + 8);
    z->crcExpected = le32(h + 14);
    z->csize = cs;
    z->usize = us;
    z->zip64 = 0;
    size_t nl = n < sizeof(z->name) - 1 ? n : sizeof(z->name) - 1;
    memcpy(z->name, h + 30, nl);
    z->name[nl] = '\0';

    for (const uint8_t *x = h + 30 + n, *xe = x + e; x + 4 <= xe;) {
        uint16_t id = le16(x), sz = le16(x + 2);
        const uint8_t *q = x + 4;
        if (q + sz > xe) break;
        if (id == 0x0001) {
            z->zip64 = 1;
            if (sz >= 16) {
                z->usize = le64(q);
                z->csize = le64(q + 8);
            } else if (sz >= 8) {
                if (us == 0xFFFFFFFFu) z->usize = le64(q);
                else z->csize = le64(q);
            }
        }
        x = q + sz;
    }
    z->sizeKnown = !(z->flags & 8);
    (void)dataStart;

    int isDir = nl > 0 && z->name[nl - 1] == '/';
    z->chosen = !z->found && !isDir && (ends_with_ci(z->name, ".wbfs") || ends_with_ci(z->name, ".iso") ||
                                        ends_with_ci(z->name, ".rvz") || ends_with_ci(z->name, ".gcm") ||
                                        ends_with_ci(z->name, ".wia"));
    if (z->chosen && (z->flags & 1)) return fail(z, "Password-protected zips aren't supported.");

    if (!z->chosen) {
        if (z->sizeKnown) {
            z->compLeft = z->csize;
            z->st = ST_SKIP;
            if (z->compLeft == 0) return end_entry_data(z);
            return 0;
        }
        if (z->method == 8) {
            z->st = ST_DATA; /* length unknown: inflate it just to find where it ends */
            return inflate_start(z);
        }
        return fail(z, "This zip can't be streamed (an entry has an unknown size). Re-zip it normally.");
    }

    if (z->method != 0 && z->method != 8) {
        char msg[160];
        snprintf(msg, sizeof(msg), "This zip uses compression method %u, which isn't supported. Re-zip it with "
                                   "normal (Deflate) settings.", z->method);
        return fail(z, msg);
    }
    if (z->method == 0 && !z->sizeKnown) return fail(z, "This zip can't be streamed. Re-zip it normally.");

    z->outPos = 0;
    z->skipTo = 0;
    z->crc = (uint32_t)crc32(0L, Z_NULL, 0);
    z->winPos = 0;
    z->winTotal = 0;
    z->ncand = z->candNext = 0;
    z->compLeft = z->csize;
    if (z->sink.begin(z->sink.ud, z->name, z->sizeKnown ? z->usize : 0) != 0) return fail(z, "");
    if (z->method == 8 && inflate_start(z) != 0) return -1;
    add_candidate(z, dataStart, 0, 0); /* start of the entry */
    z->st = ST_DATA;
    if (z->method == 0 && z->compLeft == 0) return end_entry_data(z);
    return 0;
}

static int do_header(ZipStream *z, const uint8_t *p, size_t len, size_t *used)
{
    if (z->hneed == 0) z->hneed = 4;
    size_t take = z->hneed - z->hlen < len ? z->hneed - z->hlen : len;
    memcpy(z->hbuf + z->hlen, p, take);
    z->hlen += take;
    *used = take;
    if (z->hlen < z->hneed) return 0;

    if (z->hneed == 4) {
        if (le32(z->hbuf) != 0x04034b50u) { /* central directory or end: no more entries */
            z->st = ST_DONE;
            return 0;
        }
        z->hneed = 30;
        return 0;
    }
    if (z->hneed == 30) {
        size_t full = 30 + le16(z->hbuf + 26) + le16(z->hbuf + 28);
        if (full > 30) {
            z->hneed = full;
            return 0;
        }
    }
    uint64_t dataStart = z->inPos + take;
    z->hlen = z->hneed = 0;
    return parse_entry(z, dataStart);
}

static int do_data(ZipStream *z, const uint8_t *p, size_t len, size_t *used)
{
    if (z->method == 0) {
        size_t take = z->compLeft < len ? (size_t)z->compLeft : len;
        if (emit(z, p, take) != 0) return -1;
        z->compLeft -= take;
        *used = take;
        if (z->chosen && z->outPos - z->lastCand >= CAND_EVERY) add_candidate(z, z->inPos + take, 0, 0);
        if (z->compLeft == 0) return end_entry_data(z);
        return 0;
    }

    if (len > (1u << 30)) len = 1u << 30;
    z->zs.next_in = (Bytef *)p;
    z->zs.avail_in = (uInt)len;
    for (;;) {
        z->zs.next_out = z->out;
        z->zs.avail_out = OUT_CHUNK;
        int ret = inflate(&z->zs, Z_BLOCK);
        if (ret == Z_NEED_DICT || ret == Z_DATA_ERROR || ret == Z_MEM_ERROR || ret == Z_STREAM_ERROR)
            return fail(z, "The zip data is corrupt (or the download is). Pick the game again to retry.");
        size_t produced = OUT_CHUNK - z->zs.avail_out;
        size_t consumed = len - z->zs.avail_in;
        if (emit(z, z->out, produced) != 0) return -1;

        if (ret == Z_STREAM_END) {
            *used = consumed;
            return end_entry_data(z);
        }
        if (z->chosen && (z->zs.data_type & 128) && !(z->zs.data_type & 64) && z->outPos - z->lastCand >= CAND_EVERY) {
            int bits = z->zs.data_type & 7;
            uint8_t last = consumed ? p[consumed - 1] : z->prevLast;
            add_candidate(z, z->inPos + consumed, bits, last);
        }
        if (z->zs.avail_in == 0 && z->zs.avail_out != 0) break; /* needs more input */
        if (ret == Z_BUF_ERROR && produced == 0) break;
    }
    *used = len;
    if (len) z->prevLast = p[len - 1];
    return 0;
}

static int do_desc(ZipStream *z, const uint8_t *p, size_t len, size_t *used)
{
    if (z->hneed == 0) z->hneed = 4;
    size_t take = z->hneed - z->hlen < len ? z->hneed - z->hlen : len;
    memcpy(z->hbuf + z->hlen, p, take);
    z->hlen += take;
    *used = take;
    if (z->hlen < z->hneed) return 0;
    size_t sizes = z->zip64 ? 16 : 8;
    int hasSig = le32(z->hbuf) == 0x08074b50u;
    size_t full = (hasSig ? 8 : 4) + sizes;
    if (z->hneed < full) {
        z->hneed = full;
        return 0;
    }
    uint32_t crc = le32(z->hbuf + (hasSig ? 4 : 0));
    return close_entry(z, crc);
}

int zs_feed(ZipStream *z, const void *data, size_t len)
{
    const uint8_t *p = data;
    while (len > 0) {
        size_t used = 0;
        int rc = 0;
        switch (z->st) {
        case ST_HEADER: rc = do_header(z, p, len, &used); break;
        case ST_SKIP:
            used = z->compLeft < len ? (size_t)z->compLeft : len;
            z->compLeft -= used;
            if (z->compLeft == 0) rc = end_entry_data(z);
            break;
        case ST_DATA: rc = do_data(z, p, len, &used); break;
        case ST_DESC: rc = do_desc(z, p, len, &used); break;
        default: used = len; break; /* ST_DONE */
        }
        if (used) z->prevLast = p[used - 1];
        z->inPos += used;
        p += used;
        len -= used;
        if (rc) return -1;
    }
    return 0;
}

int zs_finish(ZipStream *z)
{
    if (z->found) return 0;
    if (z->chosen) return fail(z, "The download ended in the middle of the game file.");
    return fail(z, "No .wbfs, .iso or .rvz inside the zip.");
}

static int is_game_name(const char *n)
{
    return ends_with_ci(n, ".wbfs") || ends_with_ci(n, ".iso") || ends_with_ci(n, ".rvz") || ends_with_ci(n, ".gcm") ||
           ends_with_ci(n, ".wia");
}

int zs_peek_game(const uint8_t *buf, size_t len, uint8_t *out, size_t outMax, size_t *outLen)
{
    size_t off = 0;
    *outLen = 0;
    while (off + 30 <= len && le32(buf + off) == 0x04034b50u) {
        const uint8_t *h = buf + off;
        uint16_t flags = le16(h + 6), method = le16(h + 8), nl = le16(h + 26), el = le16(h + 28);
        uint64_t cs = le32(h + 18);
        size_t data = off + 30 + nl + el;
        if (data > len) return -1;
        char name[256];
        size_t n = nl < sizeof(name) - 1 ? nl : sizeof(name) - 1;
        memcpy(name, h + 30, n);
        name[n] = '\0';
        if (is_game_name(name) && !(flags & 1)) {
            if (method == 0) {
                size_t take = len - data < outMax ? len - data : outMax;
                memcpy(out, buf + data, take);
                *outLen = take;
                return take ? 0 : -1;
            }
            if (method != 8) return -1;
            z_stream zs;
            memset(&zs, 0, sizeof(zs));
            if (inflateInit2(&zs, -15) != Z_OK) return -1;
            zs.next_in = (Bytef *)(buf + data);
            zs.avail_in = (uInt)(len - data);
            zs.next_out = out;
            zs.avail_out = (uInt)outMax;
            int ret = inflate(&zs, Z_SYNC_FLUSH);
            *outLen = outMax - zs.avail_out;
            inflateEnd(&zs);
            return (ret == Z_OK || ret == Z_STREAM_END || ret == Z_BUF_ERROR) && *outLen ? 0 : -1;
        }
        if ((flags & 8) || cs == 0xFFFFFFFFu) return -1; /* can't skip an entry of unknown size */
        off = data + (size_t)cs;
    }
    return -1;
}
