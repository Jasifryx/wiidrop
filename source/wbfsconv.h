#pragma once
/*
 * Streaming Wii ISO -> WBFS converter.
 *
 * Feed it the ISO from start to end (from a download or a .7z) and it writes a WBFS file in the
 * same format USB Loader GX makes itself: 512-byte HD sectors, 2 MB WBFS blocks, one disc slot,
 * header trimmed so n_hd_sec matches the file size. Blocks outside the disc header area and the
 * disc's partitions (the empty/junk padding) are left out, so the file is usually smaller than the ISO.
 *
 * Progress is checkpointed to a small state file so an interrupted download can resume.
 */
#include <stddef.h>
#include <stdint.h>

#include "splitwriter.h"

#define WBFS_BLOCK_SHIFT 21
#define WBFS_BLOCK_SIZE  (1u << WBFS_BLOCK_SHIFT)
#define WBFS_DISC_BLOCKS 4482 /* (143432 * 2 Wii sectors) / 64 per 2 MB block: dual-layer disc */
#define WC_MAX_PARTS     16

typedef struct {
    SplitWriter *out; /* configured by the caller (base/ext/split), opened by wc_open */
    char statePath[600];
    uint8_t *block;
    uint32_t fill;
    uint32_t blockIndex; /* ISO blocks consumed so far */
    uint32_t outBlocks;  /* WBFS blocks written, including block 0 (the header) */
    uint16_t wlba[WBFS_DISC_BLOCKS];
    uint8_t discHeader[0x100];
    int nParts;
    uint64_t partOffset[WC_MAX_PARTS];
    uint64_t keepEnd[WC_MAX_PARTS]; /* 0 = header not seen yet */
    int keepAll;
    uint32_t sinceSave;
    char err[200];
    /* optional: called after each checkpoint with the ISO offset it covers (e.g. to save a zip resume point) */
    void (*onSave)(void *ud, uint64_t isoPos);
    void *onSaveUd;
} WbfsConv;

void wc_init(WbfsConv *c, SplitWriter *out, const char *statePath);

/* If a valid checkpoint matches the output on disk, loads it and returns the ISO byte offset to
   resume from; otherwise returns 0 (start over). */
uint64_t wc_resume_point(WbfsConv *c);

/* Start writing. isoOffset is 0, or the value wc_resume_point() returned. 0 on success. */
int wc_open(WbfsConv *c, uint64_t isoOffset);

/* Feed ISO bytes. 0 on success, -1 on error (c->err filled). */
int wc_write(WbfsConv *c, const void *data, size_t len);

/* Bytes of ISO consumed so far (resume offset + everything written since). */
uint64_t wc_iso_pos(const WbfsConv *c);

/* Flush the last partial block, close the output, write the WBFS header, remove the checkpoint. */
int wc_finish(WbfsConv *c);

/* Checkpoint now (e.g. right before pausing). 0 on success. */
int wc_save(WbfsConv *c);

/* Close files but keep the checkpoint (for pause / resume). */
void wc_close(WbfsConv *c);

/* Remove the checkpoint and all output. */
void wc_discard(WbfsConv *c);

/* True if a checkpoint file exists (a conversion was interrupted). */
int wc_has_state(const char *statePath);
