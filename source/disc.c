#include "disc.h"

#include <stdio.h>
#include <string.h>

#define WII_MAGIC 0x5D1C9EA3u /* at 0x18 of a Wii disc header */
#define GC_MAGIC  0xC2339F3Du /* at 0x1C of a GameCube disc header */

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static int valid_id(const uint8_t *p)
{
    for (int i = 0; i < 6; i++) {
        uint8_t c = p[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) return 0;
    }
    return 1;
}

/* Disc header: ID at 0x00, title at 0x20 (64 bytes). */
static void fill(const uint8_t *hdr, DiscInfo *out)
{
    memcpy(out->id, hdr, 6);
    out->id[6] = '\0';
    out->disc = hdr[6] == 1 ? 1 : 0;
    int j = 0;
    for (int i = 0; i < 64 && hdr[0x20 + i]; i++) {
        uint8_t c = hdr[0x20 + i];
        out->title[j++] = (c >= 0x20 && c < 0x7F) ? (char)c : ' ';
    }
    while (j > 0 && out->title[j - 1] == ' ') j--;
    out->title[j] = '\0';
}

DiscKind disc_identify(const uint8_t *buf, size_t len, DiscInfo *out)
{
    memset(out, 0, sizeof(*out));
    if (len < 0x60) return DISC_UNKNOWN;

    if (len >= DISC_HEADER_NEED && !memcmp(buf, "WBFS", 4)) {
        const uint8_t *hdr = buf + 0x200;
        if (be32(hdr + 0x18) == WII_MAGIC && valid_id(hdr)) {
            fill(hdr, out);
            return out->kind = DISC_WII_WBFS;
        }
        return DISC_UNKNOWN;
    }
    if (be32(buf + 0x1C) == GC_MAGIC && valid_id(buf)) {
        fill(buf, out);
        return out->kind = DISC_GC_ISO;
    }
    if (be32(buf + 0x18) == WII_MAGIC && valid_id(buf)) {
        fill(buf, out);
        return out->kind = DISC_WII_ISO;
    }
    return DISC_UNKNOWN;
}

DiscKind disc_identify_file(const char *path, DiscInfo *out)
{
    uint8_t buf[DISC_HEADER_NEED];
    memset(out, 0, sizeof(*out));
    FILE *f = fopen(path, "rb");
    if (!f) return DISC_UNKNOWN;
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    return disc_identify(buf, n, out);
}
