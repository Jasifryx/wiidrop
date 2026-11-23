#pragma once

#include <stddef.h>
#include <stdint.h>

#include "splitwriter.h"

typedef struct {
    /* Called once with the chosen file's name and size, before any data. Return 0 to continue. */
    int (*begin)(void *ud, const char *name, uint64_t size);
    /* Return 0 on success, nonzero to abort. */
    int (*write)(void *ud, const void *data, size_t len);
    /* Return nonzero to cancel. */
    int (*progress)(void *ud, uint64_t done, uint64_t total);
    void *ud;
} SzSink;

/* True if buf starts with the 7z signature. */
int sz_is_7z(const uint8_t *buf, size_t len);

/*
 * Opens the .7z stored in the (possibly split) files described by `archive` and streams the game
 * inside it to `sink` without loading it into memory. Picks the first .wbfs, else the first .iso;
 * every other file (readme .txt, .nfo, ...) is skipped.
 *
 * Supports LZMA, LZMA2 and uncompressed ("Copy") archives, solid or not - what 7-Zip makes by
 * default. Returns 0 on success, 1 if cancelled, -1 on error (err filled).
 */
int sz_extract_game(const SplitWriter *archive, SzSink *sink, char *err, size_t errlen);
