#include "splitwriter.h"

#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define IO_BUF_SIZE (2 * 1024 * 1024)

void sw_part_path(const SplitWriter *sw, int part, char *out, size_t n)
{
    if (part == 0 || !sw->split)
        snprintf(out, n, "%s%s", sw->base, sw->ext0);
    else {
        char ext[32];
        snprintf(ext, sizeof(ext), sw->partfmt ? sw->partfmt : ".wbf%d", part);
        snprintf(out, n, "%s%s", sw->base, ext);
    }
}

static int64_t file_size(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    return (int64_t)st.st_size;
}

uint64_t sw_existing_size(SplitWriter *sw)
{
    char path[600];
    uint64_t total = 0;
    for (int part = 0;; part++) {
        sw_part_path(sw, part, path, sizeof(path));
        int64_t sz = file_size(path);
        if (sz < 0) break;
        if (part > 0 && total % sw->part_size != 0) return 0; /* gap: earlier part not full */
        if ((uint64_t)sz > sw->part_size) return 0;
        total += (uint64_t)sz;
        if (!sw->split) break;
    }
    return total;
}

void sw_remove_all(SplitWriter *sw)
{
    char path[600];
    for (int part = 0; part < 64; part++) {
        sw_part_path(sw, part, path, sizeof(path));
        if (remove(path) != 0 && part > 0) break;
        if (!sw->split) break;
    }
}

static int open_part(SplitWriter *sw, int part, const char *mode)
{
    char path[600];
    sw_part_path(sw, part, path, sizeof(path));
    sw->fp = fopen(path, mode);
    if (!sw->fp) return -1;
    if (sw->buf) setvbuf(sw->fp, sw->buf, _IOFBF, IO_BUF_SIZE);
    return 0;
}

int sw_open(SplitWriter *sw, uint64_t start)
{
    if (sw->part_size == 0) sw->part_size = sw->split ? WBFS_SPLIT_SIZE : UINT64_MAX;
    if (!sw->buf) sw->buf = memalign(0x40, IO_BUF_SIZE); /* aligned buffers write faster on Wii U */

    if (start == 0) sw_remove_all(sw);
    sw->pos = start;
    sw->fp = NULL;

    /* Resume by appending to the last, partially written part. If it is exactly full the next
       part is opened lazily on the next write. */
    if (start > 0 && start % sw->part_size != 0) {
        int part = (int)(start / sw->part_size);
        if (open_part(sw, part, "ab") != 0) return -1;
    }
    return 0;
}

int sw_write(SplitWriter *sw, const void *data, size_t len)
{
    const uint8_t *p = data;
    while (len > 0) {
        if (!sw->fp) {
            int part = (int)(sw->pos / sw->part_size);
            if (open_part(sw, part, "wb") != 0) return -1;
        }
        uint64_t room = sw->part_size - (sw->pos % sw->part_size);
        size_t n = len < room ? len : (size_t)room;
        if (fwrite(p, 1, n, sw->fp) != n) return -1;
        sw->pos += n;
        p += n;
        len -= n;
        if (sw->pos % sw->part_size == 0) {
            if (fclose(sw->fp) != 0) {
                sw->fp = NULL;
                return -1;
            }
            sw->fp = NULL;
        }
    }
    return 0;
}

int sw_close(SplitWriter *sw)
{
    int rc = 0;
    if (sw->fp) {
        rc = fclose(sw->fp) == 0 ? 0 : -1;
        sw->fp = NULL;
    }
    free(sw->buf);
    sw->buf = NULL;
    return rc;
}
