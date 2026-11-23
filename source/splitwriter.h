#pragma once

#include <stdint.h>
#include <stdio.h>

/* FAT32 can't hold files >= 4 GiB. USB Loader GX reads WBFS split at 4 GiB - 32 KiB as
   GAMEID.wbfs, GAMEID.wbf1, GAMEID.wbf2, ... (same scheme Wii Backup Manager uses). */
#define WBFS_SPLIT_SIZE 0xFFFF8000ULL

typedef struct {
    char base[512];      /* path without extension */
    int split;           /* 1 = .wbfs/.wbf1..., 0 = single file with ext0 */
    const char *ext0;    /* extension for part 0, e.g. ".wbfs" or ".iso" */
    const char *partfmt; /* printf format for part n >= 1; NULL = ".wbf%d" */
    uint64_t part_size;
    uint64_t pos;        /* bytes written in total */
    FILE *fp;
    char *buf;
} SplitWriter;

void sw_part_path(const SplitWriter *sw, int part, char *out, size_t n);

/* Bytes already on disk from an earlier, interrupted download (0 if none or inconsistent). */
uint64_t sw_existing_size(SplitWriter *sw);

/* Delete every part file. */
void sw_remove_all(SplitWriter *sw);

/* Start writing at byte offset start (0 = fresh, deletes old parts). 0 on success. */
int sw_open(SplitWriter *sw, uint64_t start);
int sw_write(SplitWriter *sw, const void *data, size_t len);
int sw_close(SplitWriter *sw);
