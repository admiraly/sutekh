/* SHA-256 (FIPS 180-4), clean-room C17 implementation for SU-LIR hashes. */
#ifndef SU_SHA256_H
#define SU_SHA256_H

#include <stddef.h>
#include <stdint.h>

typedef struct SuSha256 {
    uint32_t h[8];
    uint8_t buf[64];
    size_t buf_len;
    uint64_t total_bytes;
} SuSha256;

void su_sha256_init(SuSha256 *ctx);
void su_sha256_update(SuSha256 *ctx, const void *data, size_t len);
void su_sha256_final(SuSha256 *ctx, uint8_t digest[32]);

/* Writes 64 lowercase hex characters plus a NUL terminator. */
void su_sha256_hex(const uint8_t digest[32], char out[65]);

#endif
