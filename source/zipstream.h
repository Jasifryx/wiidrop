#pragma once
/*
 * Streaming .zip reader: feed it the zip as it downloads and it hands over the game inside
 * (the first .wbfs, .iso, .gcm, .rvz or .wia), skipping everything else (readme .txt, .nfo, ...).
 *
 * Supports stored and deflate entries, zip64 (files over 4 GB) and data descriptors. Never needs the
 * whole zip on disk. Resume points are taken at deflate block boundaries (bit offset + the last
 * 32 KB of output, the same technique as zlib's zran.c), so an interrupted download can continue.
 */
#include <stddef.h>
#include <stdint.h>

typedef struct {
    /* the chosen entry starts; size = uncompressed size if known, else 0. Return 0 to continue. */
    int (*begin)(void *ud, const char *name, uint64_t size);
    /* entry data. Return 0 on success. */
    int (*write)(void *ud, const void *data, size_t len);
    void *ud;
} ZipSink;

#define ZIP_WINDOW 32768

typedef struct {
    uint32_t magic, version;
    uint64_t inOffset;   /* zip offset to continue downloading from */
    uint8_t bits, lastByte;
    uint16_t method, flags;
    uint32_t crcExpected, crc;
    uint64_t csize, usize, compLeft;
    uint64_t outPos;     /* uncompressed bytes of the entry produced before this point */
    uint32_t windowLen;
    int32_t zip64, sizeKnown;
    int32_t userFlag;    /* free for the caller (e.g. "was converting") */
    char name[256];
    uint8_t window[ZIP_WINDOW];
} ZipCkpt;

typedef struct ZipStream ZipStream;

ZipStream *zs_create(const ZipSink *sink);
void zs_destroy(ZipStream *z);

/* Feed zip bytes in order. 0 ok, -1 error (zs_error). */
int zs_feed(ZipStream *z, const void *data, size_t len);

/* Call when the download ended: 0 if the game entry was complete and its CRC matched. */
int zs_finish(ZipStream *z);

const char *zs_error(const ZipStream *z);

/* Total zip bytes consumed (absolute offset in the zip). */
uint64_t zs_in_pos(const ZipStream *z);

/* Newest resume point whose output position is <= maxOut. 0 if one exists. */
int zs_checkpoint(const ZipStream *z, uint64_t maxOut, ZipCkpt *out);

/* Continue from a checkpoint; output up to skipTo (>= ck->outPos) is regenerated but not passed
   to the sink, since the caller already has it. Feed data starting at ck->inOffset afterwards.
   The sink's begin() is not called again. 0 on success. */
int zs_resume(ZipStream *z, const ZipCkpt *ck, uint64_t skipTo);

int zs_save_ckpt(const char *path, const ZipCkpt *ck);
int zs_load_ckpt(const char *path, ZipCkpt *ck);

/* True if buf starts like a zip file. */
int zs_is_zip(const uint8_t *buf, size_t len);

/* For cover art: given the first bytes of a zip, unpack the start of the game file inside
   (up to outMax bytes). 0 if anything came out. */
int zs_peek_game(const uint8_t *buf, size_t len, uint8_t *out, size_t outMax, size_t *outLen);
