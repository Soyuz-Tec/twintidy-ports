/* sha256.c — FIPS 180-4 SHA-256. See sha256.h. */

#include "sha256.h"

#include <string.h>

static const uint32_t round_constants[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static uint32_t rotate_right(uint32_t value, unsigned bits) {
    return (value >> bits) | (value << (32 - bits));
}

static void transform(td_sha256 *context, const unsigned char block[64]) {
    uint32_t schedule[64];
    for (size_t i = 0; i < 16; i++) {
        schedule[i] = ((uint32_t)block[i * 4] << 24) |
                      ((uint32_t)block[i * 4 + 1] << 16) |
                      ((uint32_t)block[i * 4 + 2] << 8) |
                      ((uint32_t)block[i * 4 + 3]);
    }
    for (size_t i = 16; i < 64; i++) {
        uint32_t s0 = rotate_right(schedule[i - 15], 7) ^
                      rotate_right(schedule[i - 15], 18) ^ (schedule[i - 15] >> 3);
        uint32_t s1 = rotate_right(schedule[i - 2], 17) ^
                      rotate_right(schedule[i - 2], 19) ^ (schedule[i - 2] >> 10);
        schedule[i] = schedule[i - 16] + s0 + schedule[i - 7] + s1;
    }

    uint32_t a = context->state[0], b = context->state[1];
    uint32_t c = context->state[2], d = context->state[3];
    uint32_t e = context->state[4], f = context->state[5];
    uint32_t g = context->state[6], h = context->state[7];

    for (size_t i = 0; i < 64; i++) {
        uint32_t s1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
        uint32_t choose = (e & f) ^ ((~e) & g);
        uint32_t temp1 = h + s1 + choose + round_constants[i] + schedule[i];
        uint32_t s0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
        uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = s0 + majority;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    context->state[0] += a;
    context->state[1] += b;
    context->state[2] += c;
    context->state[3] += d;
    context->state[4] += e;
    context->state[5] += f;
    context->state[6] += g;
    context->state[7] += h;
}

void td_sha256_init(td_sha256 *context) {
    context->state[0] = 0x6a09e667;
    context->state[1] = 0xbb67ae85;
    context->state[2] = 0x3c6ef372;
    context->state[3] = 0xa54ff53a;
    context->state[4] = 0x510e527f;
    context->state[5] = 0x9b05688c;
    context->state[6] = 0x1f83d9ab;
    context->state[7] = 0x5be0cd19;
    context->bit_length = 0;
    context->buffered = 0;
}

void td_sha256_update(td_sha256 *context, const unsigned char *data, size_t length) {
    context->bit_length += (uint64_t)length * 8;
    while (length > 0) {
        size_t space = 64 - context->buffered;
        size_t take = length < space ? length : space;
        memcpy(context->buffer + context->buffered, data, take);
        context->buffered += take;
        data += take;
        length -= take;
        if (context->buffered == 64) {
            transform(context, context->buffer);
            context->buffered = 0;
        }
    }
}

void td_sha256_final(td_sha256 *context, unsigned char digest[TD_SHA256_DIGEST_SIZE]) {
    uint64_t bit_length = context->bit_length;
    unsigned char padding = 0x80;
    td_sha256_update(context, &padding, 1);
    unsigned char zero = 0x00;
    while (context->buffered != 56) {
        td_sha256_update(context, &zero, 1);
    }
    /* Length is appended directly; update() would corrupt the counter. */
    unsigned char length_bytes[8];
    for (size_t i = 0; i < 8; i++) {
        length_bytes[7 - i] = (unsigned char)(bit_length >> (i * 8));
    }
    memcpy(context->buffer + 56, length_bytes, 8);
    transform(context, context->buffer);
    context->buffered = 0;

    for (size_t i = 0; i < 8; i++) {
        digest[i * 4] = (unsigned char)(context->state[i] >> 24);
        digest[i * 4 + 1] = (unsigned char)(context->state[i] >> 16);
        digest[i * 4 + 2] = (unsigned char)(context->state[i] >> 8);
        digest[i * 4 + 3] = (unsigned char)(context->state[i]);
    }
}

void td_sha256_hex(const unsigned char digest[TD_SHA256_DIGEST_SIZE], char *out) {
    static const char alphabet[] = "0123456789abcdef";
    for (size_t i = 0; i < TD_SHA256_DIGEST_SIZE; i++) {
        out[i * 2] = alphabet[digest[i] >> 4];
        out[i * 2 + 1] = alphabet[digest[i] & 0x0f];
    }
    out[TD_SHA256_DIGEST_SIZE * 2] = '\0';
}
