#include "su_sha256.h"

#include <string.h>

static const uint32_t K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

static uint32_t rotr(uint32_t x, unsigned n) {
    return (x >> n) | (x << (32u - n));
}

static void compress(uint32_t h[8], const uint8_t block[64]) {
    uint32_t w[64];
    uint32_t a, b, c, d, e, f, g, hh;
    unsigned i;

    for (i = 0; i < 16u; i++) {
        w[i] = ((uint32_t)block[4u * i] << 24) | ((uint32_t)block[4u * i + 1u] << 16) |
               ((uint32_t)block[4u * i + 2u] << 8) | (uint32_t)block[4u * i + 3u];
    }
    for (i = 16u; i < 64u; i++) {
        uint32_t s0 = rotr(w[i - 15u], 7) ^ rotr(w[i - 15u], 18) ^ (w[i - 15u] >> 3);
        uint32_t s1 = rotr(w[i - 2u], 17) ^ rotr(w[i - 2u], 19) ^ (w[i - 2u] >> 10);
        w[i] = w[i - 16u] + s0 + w[i - 7u] + s1;
    }

    a = h[0]; b = h[1]; c = h[2]; d = h[3];
    e = h[4]; f = h[5]; g = h[6]; hh = h[7];

    for (i = 0; i < 64u; i++) {
        uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = hh + S1 + ch + K[i] + w[i];
        uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + maj;
        hh = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

void su_sha256_init(SuSha256 *ctx) {
    static const uint32_t iv[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                                   0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    memcpy(ctx->h, iv, sizeof iv);
    ctx->buf_len = 0;
    ctx->total_bytes = 0;
}

void su_sha256_update(SuSha256 *ctx, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    ctx->total_bytes += (uint64_t)len;
    while (len > 0) {
        size_t take = 64u - ctx->buf_len;
        if (take > len) {
            take = len;
        }
        memcpy(ctx->buf + ctx->buf_len, p, take);
        ctx->buf_len += take;
        p += take;
        len -= take;
        if (ctx->buf_len == 64u) {
            compress(ctx->h, ctx->buf);
            ctx->buf_len = 0;
        }
    }
}

void su_sha256_final(SuSha256 *ctx, uint8_t digest[32]) {
    uint64_t bits = ctx->total_bytes * 8u;
    uint8_t pad[72];
    /* Pad so that buf_len + pad_len == 56 (mod 64), then 8 length bytes. */
    size_t pad_len = (ctx->buf_len < 56u) ? (56u - ctx->buf_len) : (120u - ctx->buf_len);
    uint64_t saved = ctx->total_bytes;
    unsigned i;

    memset(pad, 0, sizeof pad);
    pad[0] = 0x80u;
    for (i = 0; i < 8u; i++) {
        pad[pad_len + i] = (uint8_t)(bits >> (56u - 8u * i));
    }
    su_sha256_update(ctx, pad, pad_len + 8u);
    ctx->total_bytes = saved;

    for (i = 0; i < 8u; i++) {
        digest[4u * i] = (uint8_t)(ctx->h[i] >> 24);
        digest[4u * i + 1u] = (uint8_t)(ctx->h[i] >> 16);
        digest[4u * i + 2u] = (uint8_t)(ctx->h[i] >> 8);
        digest[4u * i + 3u] = (uint8_t)ctx->h[i];
    }
}

void su_sha256_hex(const uint8_t digest[32], char out[65]) {
    static const char digits[] = "0123456789abcdef";
    unsigned i;
    for (i = 0; i < 32u; i++) {
        out[2u * i] = digits[digest[i] >> 4];
        out[2u * i + 1u] = digits[digest[i] & 0x0Fu];
    }
    out[64] = '\0';
}
