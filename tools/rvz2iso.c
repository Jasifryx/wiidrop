/*
 * rvz2iso - PC build of WiiDrop's RVZ reader (the exact code the console runs).
 *
 *   rvz2iso game.rvz game.iso            rebuild the disc image
 *   rvz2iso game.rvz --compare game.iso  rebuild in memory and compare with an ISO made by Dolphin
 *   rvz2iso --selftest                   check the AES / SHA-1 code against official test vectors
 */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "rvz.h"
#include "wiicrypto.h"

#ifdef _MSC_VER
#define fseeko _fseeki64
#define ftello _ftelli64
#endif

static int read_at(void *ud, uint64_t off, void *buf, size_t len)
{
    FILE *f = ud;
    if (fseeko(f, (long long)off, SEEK_SET) != 0) return -1;
    return fread(buf, 1, len, f) == len ? 0 : -1;
}

typedef struct {
    FILE *out, *ref;
    uint64_t pos, firstDiff;
    int diff;
    unsigned char *tmp;
    clock_t t0, last;
} Job;

static int out_cb(void *ud, const void *data, size_t len)
{
    Job *j = ud;
    if (j->out && fwrite(data, 1, len, j->out) != len) return -1;
    if (j->ref) {
        if (fread(j->tmp, 1, len, j->ref) != len || memcmp(j->tmp, data, len) != 0) {
            if (!j->diff) {
                size_t i = 0;
                while (i < len && ((const unsigned char *)data)[i] == j->tmp[i]) i++;
                j->firstDiff = j->pos + i;
            }
            j->diff = 1;
        }
    }
    j->pos += len;
    return 0;
}

static int progress_cb(void *ud, uint64_t done, uint64_t total)
{
    Job *j = ud;
    clock_t now = clock();
    if (now - j->last > CLOCKS_PER_SEC / 2 || done == total) {
        j->last = now;
        double secs = (double)(now - j->t0) / CLOCKS_PER_SEC;
        fprintf(stderr, "\r  %5.1f%%  %.0f MB  %.1f MB/s   ", total ? 100.0 * done / total : 0, done / 1048576.0,
                secs > 0 ? done / 1048576.0 / secs : 0);
    }
    return 0;
}

static int hex_eq(const unsigned char *b, size_t n, const char *hex)
{
    char s[256];
    for (size_t i = 0; i < n; i++) sprintf(s + 2 * i, "%02x", b[i]);
    return strcmp(s, hex) == 0;
}

static int selftest(void)
{
    int ok = 1;
    unsigned char d[20], o[64];
    sha1("abc", 3, d);
    ok &= hex_eq(d, 20, "a9993e364706816aba3e25717850c26c9cd0d89d");
    sha1("", 0, d);
    ok &= hex_eq(d, 20, "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    sha1("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, d);
    ok &= hex_eq(d, 20, "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
    unsigned char *m = malloc(1000000);
    memset(m, 'a', 1000000);
    sha1(m, 1000000, d);
    free(m);
    ok &= hex_eq(d, 20, "34aa973cd4c4daa4f61eeb2bdbad27316534016f");

    Aes128 a;
    unsigned char key[16], pt[16];
    for (int i = 0; i < 16; i++) key[i] = (unsigned char)i, pt[i] = (unsigned char)(i * 0x11);
    aes128_init(&a, key);
    aes128_encrypt_block(&a, pt, o);
    ok &= hex_eq(o, 16, "69c4e0d86a7b0430d8cdb78070b4c55a"); /* FIPS-197 C.1 */

    /* NIST SP 800-38A F.2.1, CBC-AES128 encrypt (4 blocks) */
    static const unsigned char k2[16] = {0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
                                         0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c};
    static const unsigned char iv[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    static const char *pthex = "6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51"
                               "30c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710";
    unsigned char p2[64];
    for (int i = 0; i < 64; i++) {
        unsigned int v;
        sscanf(pthex + 2 * i, "%2x", &v);
        p2[i] = (unsigned char)v;
    }
    aes128_init(&a, k2);
    aes128_cbc_encrypt(&a, iv, p2, o, 64);
    ok &= hex_eq(o, 64, "7649abac8119b246cee98e9b12e9197d5086cb9b507219ee95db113a917678b2"
                        "73bed6b8e3c1743b7116e69e222295163ff1caa1681fac09120eca307586e1a7");
    aes128_cbc_encrypt(&a, iv, p2, p2, 64); /* in place */
    ok &= hex_eq(p2, 64, "7649abac8119b246cee98e9b12e9197d5086cb9b507219ee95db113a917678b2"
                         "73bed6b8e3c1743b7116e69e222295163ff1caa1681fac09120eca307586e1a7");
    printf(ok ? "selftest: SHA-1 and AES-128 match the official test vectors\n" : "selftest: FAILED\n");
    return ok ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--selftest")) return selftest();
    if (argc != 3 && !(argc == 4 && !strcmp(argv[2], "--compare"))) {
        fprintf(stderr, "usage: rvz2iso in.rvz out.iso | rvz2iso in.rvz --compare dolphin.iso | rvz2iso --selftest\n");
        return 2;
    }
    FILE *in = fopen(argv[1], "rb");
    if (!in) {
        fprintf(stderr, "can't open %s\n", argv[1]);
        return 2;
    }
    char err[256] = {0};
    Rvz *r = rvz_open(read_at, in, err, sizeof(err));
    if (!r) {
        fprintf(stderr, "error: %s\n", err);
        return 1;
    }
    const unsigned char *h = rvz_disc_header(r);
    printf("%s disc %.6s \"%.64s\", %.2f GB\n", rvz_is_wii(r) ? "Wii" : "GameCube", h, h + 0x20,
           rvz_disc_size(r) / 1073741824.0);

    Job j = {0};
    j.tmp = malloc(4 << 20);
    j.t0 = j.last = clock();
    if (argc == 4) {
        j.ref = fopen(argv[3], "rb");
        if (!j.ref) {
            fprintf(stderr, "can't open %s\n", argv[3]);
            return 2;
        }
    } else {
        j.out = fopen(argv[2], "wb");
        if (!j.out) {
            fprintf(stderr, "can't create %s\n", argv[2]);
            return 2;
        }
    }
    int rc = rvz_extract(r, out_cb, progress_cb, &j, err, sizeof(err));
    fprintf(stderr, "\n");
    if (rc != 0) {
        fprintf(stderr, "error: %s\n", err[0] ? err : "failed");
        return 1;
    }
    if (j.ref) {
        fseeko(j.ref, 0, SEEK_END);
        long long refSize = ftello(j.ref);
        if (j.diff) printf("DIFFERENT: first mismatch at offset 0x%llx\n", (unsigned long long)j.firstDiff);
        else if ((unsigned long long)refSize != j.pos)
            printf("DIFFERENT: sizes differ (%llu vs %lld)\n", (unsigned long long)j.pos, refSize);
        else printf("IDENTICAL: all %llu bytes match\n", (unsigned long long)j.pos);
        return j.diff ? 1 : 0;
    }
    if (fclose(j.out) != 0) return 1;
    printf("wrote %s\n", argv[2]);
    rvz_close(r);
    return 0;
}
