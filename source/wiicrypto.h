#pragma once
/* Small portable SHA-1 and AES-128 (CBC encrypt), used to rebuild Wii disc partitions from RVZ. */
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t h[5];
    uint64_t len;
    uint8_t buf[64];
    size_t fill;
} Sha1;

void sha1_init(Sha1 *s);
void sha1_update(Sha1 *s, const void *data, size_t len);
void sha1_final(Sha1 *s, uint8_t out[20]);
void sha1(const void *data, size_t len, uint8_t out[20]);

typedef struct {
    uint32_t rk[44];
} Aes128;

void aes128_init(Aes128 *a, const uint8_t key[16]);
void aes128_encrypt_block(const Aes128 *a, const uint8_t in[16], uint8_t out[16]);
/* CBC encrypt len bytes (multiple of 16). in and out may be the same buffer. */
void aes128_cbc_encrypt(const Aes128 *a, const uint8_t iv[16], const uint8_t *in, uint8_t *out, size_t len);
