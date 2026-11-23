#pragma once
/*
 * RVZ (and WIA) reader: rebuilds the original GameCube / Wii disc image, byte for byte, from
 * Dolphin's compressed format, so it can be fed to the WBFS converter (Wii) or written as an ISO (GC).
 *
 * Wii partition data is stored decrypted without hashes; it is re-hashed, Dolphin's hash
 * exceptions are applied, and it is re-encrypted with the partition's title key, which RVZ stores
 * already decrypted (no console keys needed). Junk padding is regenerated from stored seeds.
 *
 * Supports RVZ with Zstandard / LZMA / LZMA2 / no compression, and WIA with LZMA / LZMA2 / none.
 */
#include <stddef.h>
#include <stdint.h>

typedef struct Rvz Rvz;

/* Random access to the .rvz file. Return 0 on success (all bytes read). */
typedef int (*RvzReadAt)(void *ud, uint64_t offset, void *buf, size_t len);
/* Receives the rebuilt disc in order. Return 0 to continue. */
typedef int (*RvzOut)(void *ud, const void *data, size_t len);
/* Progress (bytes of disc produced). Return nonzero to cancel. */
typedef int (*RvzProgress)(void *ud, uint64_t done, uint64_t total);

int rvz_is_rvz(const uint8_t *buf, size_t len); /* "RVZ\1" or "WIA\1" */

/* Disc header (first 0x80 bytes of the disc) straight from an RVZ/WIA file's first 0xDC bytes,
   e.g. for reading the game ID before downloading the whole file. Returns NULL if not RVZ/WIA. */
const uint8_t *rvz_peek_disc_header(const uint8_t *fileStart, size_t len);

Rvz *rvz_open(RvzReadAt read, void *ud, char *err, size_t errlen);
void rvz_close(Rvz *r);

int rvz_is_wii(const Rvz *r);
uint64_t rvz_disc_size(const Rvz *r);
const uint8_t *rvz_disc_header(const Rvz *r); /* 0x80 bytes */

/* Rebuild the whole disc. 0 on success, 1 if cancelled, -1 on error (err filled). */
int rvz_extract(Rvz *r, RvzOut out, RvzProgress progress, void *ud, char *err, size_t errlen);
