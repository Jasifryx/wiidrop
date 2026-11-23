#include "wiicrypto.h"

#include <string.h>

/* ------------------------------------------------------------------ SHA-1 (FIPS 180-4) */

#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static void sha1_block(uint32_t h[5], const uint8_t *p)
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) | ((uint32_t)p[i * 4 + 2] << 8) | p[i * 4 + 3];
    for (int i = 16; i < 80; i++) w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], t;
    for (int i = 0; i < 20; i++) {
        t = ROL(a, 5) + ((b & c) | (~b & d)) + e + w[i] + 0x5A827999u;
        e = d; d = c; c = ROL(b, 30); b = a; a = t;
    }
    for (int i = 20; i < 40; i++) {
        t = ROL(a, 5) + (b ^ c ^ d) + e + w[i] + 0x6ED9EBA1u;
        e = d; d = c; c = ROL(b, 30); b = a; a = t;
    }
    for (int i = 40; i < 60; i++) {
        t = ROL(a, 5) + ((b & c) | (b & d) | (c & d)) + e + w[i] + 0x8F1BBCDCu;
        e = d; d = c; c = ROL(b, 30); b = a; a = t;
    }
    for (int i = 60; i < 80; i++) {
        t = ROL(a, 5) + (b ^ c ^ d) + e + w[i] + 0xCA62C1D6u;
        e = d; d = c; c = ROL(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

void sha1_init(Sha1 *s)
{
    s->h[0] = 0x67452301u;
    s->h[1] = 0xEFCDAB89u;
    s->h[2] = 0x98BADCFEu;
    s->h[3] = 0x10325476u;
    s->h[4] = 0xC3D2E1F0u;
    s->len = 0;
    s->fill = 0;
}

void sha1_update(Sha1 *s, const void *data, size_t len)
{
    const uint8_t *p = data;
    s->len += len;
    if (s->fill) {
        size_t take = 64 - s->fill < len ? 64 - s->fill : len;
        memcpy(s->buf + s->fill, p, take);
        s->fill += take;
        p += take;
        len -= take;
        if (s->fill == 64) {
            sha1_block(s->h, s->buf);
            s->fill = 0;
        }
    }
    while (len >= 64) {
        sha1_block(s->h, p);
        p += 64;
        len -= 64;
    }
    if (len) {
        memcpy(s->buf, p, len);
        s->fill = len;
    }
}

void sha1_final(Sha1 *s, uint8_t out[20])
{
    uint64_t bits = s->len * 8;
    uint8_t pad = 0x80;
    sha1_update(s, &pad, 1);
    uint8_t zero = 0;
    while (s->fill != 56) sha1_update(s, &zero, 1);
    uint8_t lenb[8];
    for (int i = 0; i < 8; i++) lenb[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha1_update(s, lenb, 8);
    for (int i = 0; i < 5; i++) {
        out[i * 4] = (uint8_t)(s->h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(s->h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(s->h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)s->h[i];
    }
}

void sha1(const void *data, size_t len, uint8_t out[20])
{
    Sha1 s;
    sha1_init(&s);
    sha1_update(&s, data, len);
    sha1_final(&s, out);
}

/* ------------------------------------------------------------------ AES-128 (FIPS 197), encrypt only */

static const uint8_t SBOX[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76, 0xca, 0x82, 0xc9,
    0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0, 0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f,
    0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15, 0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07,
    0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75, 0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3,
    0x29, 0xe3, 0x2f, 0x84, 0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58,
    0xcf, 0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8, 0x51, 0xa3,
    0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2, 0xcd, 0x0c, 0x13, 0xec, 0x5f,
    0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73, 0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88,
    0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb, 0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac,
    0x62, 0x91, 0x95, 0xe4, 0x79, 0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a,
    0xae, 0x08, 0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a, 0x70,
    0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e, 0xe1, 0xf8, 0x98, 0x11,
    0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf, 0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42,
    0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16};

static uint32_t TE[256]; /* combined SubBytes + MixColumns table (one column, rotated for the others) */
static int teReady;

static uint8_t xtime(uint8_t x)
{
    return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1b : 0));
}

static void build_tables(void)
{
    for (int i = 0; i < 256; i++) {
        uint8_t s = SBOX[i], s2 = xtime(s), s3 = (uint8_t)(s2 ^ s);
        TE[i] = ((uint32_t)s2 << 24) | ((uint32_t)s << 16) | ((uint32_t)s << 8) | s3;
    }
    teReady = 1;
}

#define ROR8(x) (((x) >> 8) | ((x) << 24))

void aes128_init(Aes128 *a, const uint8_t key[16])
{
    if (!teReady) build_tables();
    static const uint8_t RCON[10] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36};
    for (int i = 0; i < 4; i++)
        a->rk[i] = ((uint32_t)key[4 * i] << 24) | ((uint32_t)key[4 * i + 1] << 16) | ((uint32_t)key[4 * i + 2] << 8) | key[4 * i + 3];
    for (int i = 4; i < 44; i++) {
        uint32_t t = a->rk[i - 1];
        if (i % 4 == 0) {
            t = ((uint32_t)SBOX[(t >> 16) & 0xff] << 24) | ((uint32_t)SBOX[(t >> 8) & 0xff] << 16) |
                ((uint32_t)SBOX[t & 0xff] << 8) | SBOX[t >> 24];
            t ^= (uint32_t)RCON[i / 4 - 1] << 24;
        }
        a->rk[i] = a->rk[i - 4] ^ t;
    }
}

void aes128_encrypt_block(const Aes128 *a, const uint8_t in[16], uint8_t out[16])
{
    const uint32_t *rk = a->rk;
    uint32_t s0 = (((uint32_t)in[0] << 24) | ((uint32_t)in[1] << 16) | ((uint32_t)in[2] << 8) | in[3]) ^ rk[0];
    uint32_t s1 = (((uint32_t)in[4] << 24) | ((uint32_t)in[5] << 16) | ((uint32_t)in[6] << 8) | in[7]) ^ rk[1];
    uint32_t s2 = (((uint32_t)in[8] << 24) | ((uint32_t)in[9] << 16) | ((uint32_t)in[10] << 8) | in[11]) ^ rk[2];
    uint32_t s3 = (((uint32_t)in[12] << 24) | ((uint32_t)in[13] << 16) | ((uint32_t)in[14] << 8) | in[15]) ^ rk[3];
    uint32_t t0, t1, t2, t3;
    for (int r = 1; r < 10; r++) {
        rk += 4;
        t0 = TE[s0 >> 24] ^ ROR8(TE[(s1 >> 16) & 0xff]) ^ ROR8(ROR8(TE[(s2 >> 8) & 0xff])) ^ ROR8(ROR8(ROR8(TE[s3 & 0xff]))) ^ rk[0];
        t1 = TE[s1 >> 24] ^ ROR8(TE[(s2 >> 16) & 0xff]) ^ ROR8(ROR8(TE[(s3 >> 8) & 0xff])) ^ ROR8(ROR8(ROR8(TE[s0 & 0xff]))) ^ rk[1];
        t2 = TE[s2 >> 24] ^ ROR8(TE[(s3 >> 16) & 0xff]) ^ ROR8(ROR8(TE[(s0 >> 8) & 0xff])) ^ ROR8(ROR8(ROR8(TE[s1 & 0xff]))) ^ rk[2];
        t3 = TE[s3 >> 24] ^ ROR8(TE[(s0 >> 16) & 0xff]) ^ ROR8(ROR8(TE[(s1 >> 8) & 0xff])) ^ ROR8(ROR8(ROR8(TE[s2 & 0xff]))) ^ rk[3];
        s0 = t0; s1 = t1; s2 = t2; s3 = t3;
    }
    rk += 4;
    uint32_t o[4];
    uint32_t st[4] = {s0, s1, s2, s3};
    for (int c = 0; c < 4; c++) {
        o[c] = ((uint32_t)SBOX[st[c] >> 24] << 24) | ((uint32_t)SBOX[(st[(c + 1) & 3] >> 16) & 0xff] << 16) |
               ((uint32_t)SBOX[(st[(c + 2) & 3] >> 8) & 0xff] << 8) | SBOX[st[(c + 3) & 3] & 0xff];
        o[c] ^= rk[c];
        out[4 * c] = (uint8_t)(o[c] >> 24);
        out[4 * c + 1] = (uint8_t)(o[c] >> 16);
        out[4 * c + 2] = (uint8_t)(o[c] >> 8);
        out[4 * c + 3] = (uint8_t)o[c];
    }
}

void aes128_cbc_encrypt(const Aes128 *a, const uint8_t iv[16], const uint8_t *in, uint8_t *out, size_t len)
{
    uint8_t prev[16], blk[16];
    memcpy(prev, iv, 16);
    for (size_t off = 0; off + 16 <= len; off += 16) {
        for (int i = 0; i < 16; i++) blk[i] = in[off + i] ^ prev[i];
        aes128_encrypt_block(a, blk, out + off);
        memcpy(prev, out + off, 16);
    }
}
