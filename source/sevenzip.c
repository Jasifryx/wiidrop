#include "sevenzip.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lzma/7z.h"
#include "lzma/7zCrc.h"
#include "lzma/Lzma2Dec.h"
#include "lzma/LzmaDec.h"

#ifdef _MSC_VER
#define fseeko _fseeki64
#endif

#define IN_BUF_SIZE   (1024 * 1024)
#define OUT_BUF_SIZE  (1024 * 1024)
#define MAX_DICT_SIZE (512u * 1024 * 1024) /* the Wii U has ~1 GB for apps */

#define METHOD_COPY  0x00
#define METHOD_LZMA2 0x21
#define METHOD_LZMA  0x030101

int sz_is_7z(const uint8_t *buf, size_t len)
{
    static const uint8_t sig[6] = {'7', 'z', 0xBC, 0xAF, 0x27, 0x1C};
    return len >= 6 && !memcmp(buf, sig, 6);
}

/* ---------------------------------------------------------------- allocator */

static void *sz_alloc(ISzAllocPtr p, size_t size)
{
    (void)p;
    return size ? malloc(size) : NULL;
}

static void sz_free(ISzAllocPtr p, void *addr)
{
    (void)p;
    free(addr);
}

static const ISzAlloc g_alloc = {sz_alloc, sz_free};

/* ---------------------------------------------------------------- split-file input stream */

typedef struct {
    ISeekInStream vt; /* must be first */
    const SplitWriter *sw;
    uint64_t pos, size;
    FILE *fp;
    int part;
} SplitIn;

static SRes splitin_read(ISeekInStreamPtr pp, void *buf, size_t *size)
{
    SplitIn *p = (SplitIn *)(void *)pp;
    size_t want = *size;
    *size = 0;
    if (p->pos >= p->size || want == 0) return SZ_OK;

    int part = (int)(p->pos / p->sw->part_size);
    uint64_t off = p->pos % p->sw->part_size;
    if (part != p->part || !p->fp) {
        if (p->fp) fclose(p->fp);
        char path[600];
        sw_part_path(p->sw, part, path, sizeof(path));
        p->fp = fopen(path, "rb");
        p->part = part;
        if (!p->fp) return SZ_ERROR_READ;
    }
    if (fseeko(p->fp, (int64_t)off, SEEK_SET) != 0) return SZ_ERROR_READ;

    uint64_t room = p->sw->part_size - off;
    if (want > room) want = (size_t)room;
    if (want > p->size - p->pos) want = (size_t)(p->size - p->pos);
    size_t n = fread(buf, 1, want, p->fp);
    if (n == 0 && want > 0) return SZ_ERROR_READ;
    p->pos += n;
    *size = n;
    return SZ_OK;
}

static SRes splitin_seek(ISeekInStreamPtr pp, Int64 *pos, ESzSeek origin)
{
    SplitIn *p = (SplitIn *)(void *)pp;
    int64_t base = origin == SZ_SEEK_SET ? 0 : origin == SZ_SEEK_CUR ? (int64_t)p->pos : (int64_t)p->size;
    int64_t np = base + *pos;
    if (np < 0) return SZ_ERROR_PARAM;
    p->pos = (uint64_t)np;
    *pos = np;
    return SZ_OK;
}

/* Read exactly `len` bytes starting at absolute offset `at` (used for packed data). */
static int splitin_read_at(SplitIn *in, uint64_t at, uint8_t *buf, size_t len)
{
    in->pos = at;
    while (len > 0) {
        size_t n = len;
        if (splitin_read(&in->vt, buf, &n) != SZ_OK || n == 0) return -1;
        buf += n;
        at += n;
        len -= n;
    }
    return 0;
}

/* ---------------------------------------------------------------- helpers */

static int ends_with(const char *s, const char *suffix)
{
    size_t a = strlen(s), b = strlen(suffix);
    return a >= b && !strcmp(s + a - b, suffix);
}

/* UTF-16 name -> lower-case ASCII (non-ASCII becomes '?'), for picking by extension. */
static void file_name(const CSzArEx *db, UInt32 i, char *out, size_t n, int lower)
{
    UInt16 tmp[512];
    size_t len = SzArEx_GetFileNameUtf16(db, i, NULL);
    if (len > 512) {
        snprintf(out, n, "?");
        return;
    }
    SzArEx_GetFileNameUtf16(db, i, tmp);
    size_t j = 0;
    for (size_t k = 0; tmp[k] && j + 1 < n; k++) {
        UInt16 c = tmp[k];
        char ch = c < 0x80 ? (char)c : '?';
        out[j++] = lower ? (char)tolower((unsigned char)ch) : ch;
    }
    out[j] = '\0';
}

static const char *sres_text(SRes r)
{
    switch (r) {
    case SZ_ERROR_DATA: return "archive data is corrupt";
    case SZ_ERROR_MEM: return "not enough memory";
    case SZ_ERROR_CRC: return "CRC error (corrupt download?)";
    case SZ_ERROR_UNSUPPORTED: return "unsupported 7z feature";
    case SZ_ERROR_NO_ARCHIVE: return "not a 7z archive";
    case SZ_ERROR_ARCHIVE: return "broken 7z archive";
    case SZ_ERROR_INPUT_EOF: return "archive is cut off (incomplete download?)";
    case SZ_ERROR_READ: return "could not read the archive from SD";
    default: return "unknown 7z error";
    }
}

/* ---------------------------------------------------------------- extraction */

int sz_extract_game(const SplitWriter *archive, SzSink *sink, char *err, size_t errlen)
{
    static int crcReady;
    if (!crcReady) {
        CrcGenerateTable();
        crcReady = 1;
    }

    SplitIn in;
    memset(&in, 0, sizeof(in));
    in.vt.Read = splitin_read;
    in.vt.Seek = splitin_seek;
    in.sw = archive;
    in.size = sw_existing_size((SplitWriter *)archive);
    in.part = -1;

    CLookToRead2 look;
    memset(&look, 0, sizeof(look));
    LookToRead2_CreateVTable(&look, False);
    look.realStream = &in.vt;
    look.bufSize = 1 << 16;
    look.buf = malloc(look.bufSize);
    LookToRead2_INIT(&look);

    CSzArEx db;
    SzArEx_Init(&db);

    uint8_t *inBuf = NULL, *outBuf = NULL;
    CLzmaDec lzma;
    CLzma2Dec lzma2;
    int lzmaInit = 0, lzma2Init = 0;
    int rc = -1;

    if (!look.buf) {
        snprintf(err, errlen, "Out of memory");
        goto done;
    }

    SRes res = SzArEx_Open(&db, &look.vt, &g_alloc, &g_alloc);
    if (res != SZ_OK) {
        snprintf(err, errlen, "Could not open the .7z: %s", sres_text(res));
        goto done;
    }

    /* pick the game file */
    UInt32 fi = (UInt32)-1, isoIdx = (UInt32)-1;
    char name[256];
    for (UInt32 i = 0; i < db.NumFiles; i++) {
        if (SzArEx_IsDir(&db, i)) continue;
        file_name(&db, i, name, sizeof(name), 1);
        if (ends_with(name, ".wbfs")) {
            fi = i;
            break;
        }
        if (ends_with(name, ".iso") && isoIdx == (UInt32)-1) isoIdx = i;
    }
    if (fi == (UInt32)-1) fi = isoIdx;
    if (fi == (UInt32)-1) {
        snprintf(err, errlen, "No .wbfs or .iso inside the .7z.");
        goto done;
    }
    file_name(&db, fi, name, sizeof(name), 0);

    uint64_t fileSize = SzArEx_GetFileSize(&db, fi);
    UInt32 folder = db.FileToFolder[fi];
    if (folder == (UInt32)-1 || fileSize == 0) {
        snprintf(err, errlen, "%s inside the .7z is empty.", name);
        goto done;
    }

    /* describe the folder (solid block) that holds it */
    const Byte *coderData = db.db.CodersData + db.db.FoCodersOffsets[folder];
    CSzData sd;
    sd.Data = coderData;
    sd.Size = db.db.FoCodersOffsets[folder + 1] - db.db.FoCodersOffsets[folder];
    CSzFolder f;
    if (SzGetNextFolderItem(&f, &sd) != SZ_OK || f.NumCoders != 1 || f.NumPackStreams != 1) {
        snprintf(err, errlen, "This .7z uses filters WiiDrop can't stream.\nRepack it with plain LZMA2 (7-Zip defaults).");
        goto done;
    }
    const CSzCoderInfo *coder = &f.Coders[0];
    const Byte *props = coderData + coder->PropsOffset;
    UInt32 method = coder->MethodID;

    UInt32 packIdx = db.db.FoStartPackStreamIndex[folder];
    uint64_t packPos = db.dataPos + db.db.PackPositions[packIdx];
    uint64_t packLeft = db.db.PackPositions[packIdx + 1] - db.db.PackPositions[packIdx];
    uint64_t folderSize = SzAr_GetFolderUnpackSize(&db.db, folder);
    uint64_t fileStart = db.UnpackPositions[fi] - db.UnpackPositions[db.FolderToFile[folder]];
    uint64_t fileEnd = fileStart + fileSize;

    if (method == METHOD_LZMA2) {
        if (coder->PropsSize != 1 || props[0] > 40) {
            snprintf(err, errlen, "Bad LZMA2 settings in the .7z.");
            goto done;
        }
        uint32_t dict = props[0] == 40 ? 0xFFFFFFFFu : (2u | (props[0] & 1u)) << (props[0] / 2 + 11);
        if (dict > MAX_DICT_SIZE) {
            snprintf(err, errlen, "The .7z dictionary (%u MB) is too big for the Wii U.\nRepack with a dictionary of 256 MB or less.", dict >> 20);
            goto done;
        }
        Lzma2Dec_CONSTRUCT(&lzma2);
        if (Lzma2Dec_Allocate(&lzma2, props[0], &g_alloc) != SZ_OK) {
            snprintf(err, errlen, "Not enough memory for the .7z dictionary.");
            goto done;
        }
        lzma2Init = 1;
        Lzma2Dec_Init(&lzma2);
    } else if (method == METHOD_LZMA) {
        if (coder->PropsSize != 5) {
            snprintf(err, errlen, "Bad LZMA settings in the .7z.");
            goto done;
        }
        uint32_t dict = props[1] | (props[2] << 8) | (props[3] << 16) | ((uint32_t)props[4] << 24);
        if (dict > MAX_DICT_SIZE) {
            snprintf(err, errlen, "The .7z dictionary (%u MB) is too big for the Wii U.\nRepack with a dictionary of 256 MB or less.", dict >> 20);
            goto done;
        }
        LzmaDec_CONSTRUCT(&lzma);
        if (LzmaDec_Allocate(&lzma, props, 5, &g_alloc) != SZ_OK) {
            snprintf(err, errlen, "Not enough memory for the .7z dictionary.");
            goto done;
        }
        lzmaInit = 1;
        LzmaDec_Init(&lzma);
    } else if (method != METHOD_COPY) {
        snprintf(err, errlen, "Unsupported 7z compression method (0x%X).\nRepack with LZMA2 (7-Zip default).", (unsigned)method);
        goto done;
    }

    inBuf = malloc(IN_BUF_SIZE);
    outBuf = malloc(OUT_BUF_SIZE);
    if (!inBuf || !outBuf) {
        snprintf(err, errlen, "Out of memory");
        goto done;
    }

    if (sink->begin(sink->ud, name, fileSize) != 0) {
        err[0] = '\0'; /* sink reports its own error */
        goto done;
    }

    UInt32 crc = CRC_INIT_VAL;
    uint64_t outPos = 0; /* position in the folder's unpacked stream */
    size_t inPos = 0, inLen = 0;

    while (outPos < fileEnd) {
        if (inPos == inLen && packLeft > 0) {
            inLen = packLeft < IN_BUF_SIZE ? (size_t)packLeft : IN_BUF_SIZE;
            if (splitin_read_at(&in, packPos, inBuf, inLen) != 0) {
                snprintf(err, errlen, "Could not read the archive (cut off?)");
                goto done;
            }
            packPos += inLen;
            packLeft -= inLen;
            inPos = 0;
        }

        SizeT outLen = OUT_BUF_SIZE;
        SizeT srcLen = inLen - inPos;
        uint64_t folderLeft = folderSize - outPos;
        if (outLen > folderLeft) outLen = (SizeT)folderLeft;

        if (method == METHOD_COPY) {
            if (outLen > srcLen) outLen = srcLen;
            memcpy(outBuf, inBuf + inPos, outLen);
            srcLen = outLen;
        } else {
            ELzmaFinishMode mode = (outLen == folderLeft) ? LZMA_FINISH_END : LZMA_FINISH_ANY;
            ELzmaStatus status;
            res = method == METHOD_LZMA2
                      ? Lzma2Dec_DecodeToBuf(&lzma2, outBuf, &outLen, inBuf + inPos, &srcLen, mode, &status)
                      : LzmaDec_DecodeToBuf(&lzma, outBuf, &outLen, inBuf + inPos, &srcLen, mode, &status);
            if (res != SZ_OK) {
                snprintf(err, errlen, "Extraction failed: %s", sres_text(res));
                goto done;
            }
        }
        inPos += srcLen;

        if (outLen == 0 && srcLen == 0) {
            snprintf(err, errlen, "Archive ended early (incomplete download?)");
            goto done;
        }

        /* hand over the part of this chunk that belongs to our file */
        uint64_t chunkEnd = outPos + outLen;
        if (chunkEnd > fileStart && outPos < fileEnd) {
            uint64_t from = outPos > fileStart ? outPos : fileStart;
            uint64_t to = chunkEnd < fileEnd ? chunkEnd : fileEnd;
            const uint8_t *p = outBuf + (from - outPos);
            size_t n = (size_t)(to - from);
            crc = CrcUpdate(crc, p, n);
            if (sink->write(sink->ud, p, n) != 0) {
                err[0] = '\0';
                goto done;
            }
            if (sink->progress(sink->ud, to - fileStart, fileSize) != 0) {
                rc = 1;
                goto done;
            }
        }
        outPos = chunkEnd;
    }

    if (SzBitWithVals_Check(&db.CRCs, fi) && CRC_GET_DIGEST(crc) != db.CRCs.Vals[fi]) {
        snprintf(err, errlen, "CRC mismatch: the downloaded .7z is corrupt.\nDelete it and download again.");
        goto done;
    }
    rc = 0;

done:
    if (lzmaInit) LzmaDec_Free(&lzma, &g_alloc);
    if (lzma2Init) Lzma2Dec_Free(&lzma2, &g_alloc);
    free(inBuf);
    free(outBuf);
    SzArEx_Free(&db, &g_alloc);
    free(look.buf);
    if (in.fp) fclose(in.fp);
    return rc;
}
