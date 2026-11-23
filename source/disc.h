#pragma once

#include <stddef.h>
#include <stdint.h>

/* Enough bytes to see a WBFS file's embedded disc header (0x200 + 0x60). */
#define DISC_HEADER_NEED 0x260

typedef enum {
    DISC_UNKNOWN = 0, /* not a disc image (often an HTML page from a share link) */
    DISC_WII_WBFS,
    DISC_GC_ISO,
    DISC_WII_ISO,     /* recognised, but won't fit FAT32 - must be converted to WBFS */
} DiscKind;

typedef struct {
    DiscKind kind;
    char id[7];     /* e.g. "RMCE01" */
    char title[65]; /* printable ASCII, trimmed */
    int disc;       /* 0 = disc 1, 1 = disc 2 (two-disc GameCube games share one ID) */
} DiscInfo;

/* Identify a disc image from its first bytes. len should be >= DISC_HEADER_NEED. */
DiscKind disc_identify(const uint8_t *buf, size_t len, DiscInfo *out);

/* Read the header from a file on disk and identify it. */
DiscKind disc_identify_file(const char *path, DiscInfo *out);
