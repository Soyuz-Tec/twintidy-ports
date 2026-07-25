/*
 * sha256.h — FIPS 180-4 SHA-256, used so duplicate-group hashes in reports
 * match the identifiers TwinTidy's Go engine publishes.
 */

#ifndef TWINTIDY_SHA256_H
#define TWINTIDY_SHA256_H

#include <stddef.h>
#include <stdint.h>

#define TD_SHA256_DIGEST_SIZE 32
/* 64 hex characters plus a terminator. */
#define TD_SHA256_HEX_SIZE 65

typedef struct {
    uint32_t state[8];
    uint64_t bit_length;
    unsigned char buffer[64];
    size_t buffered;
} td_sha256;

void td_sha256_init(td_sha256 *context);
void td_sha256_update(td_sha256 *context, const unsigned char *data, size_t length);
void td_sha256_final(td_sha256 *context, unsigned char digest[TD_SHA256_DIGEST_SIZE]);

/* Render a digest as lower-case hex into a TD_SHA256_HEX_SIZE buffer. */
void td_sha256_hex(const unsigned char digest[TD_SHA256_DIGEST_SIZE], char *out);

#endif /* TWINTIDY_SHA256_H */
