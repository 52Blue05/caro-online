#ifndef SHA256_H
#define SHA256_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SHA256_BLOCK_SIZE  64  /* 512-bit block */
#define SHA256_DIGEST_SIZE 32  /* 256-bit digest */
#define SHA256_HEX_SIZE    65  /* 64 hex characters + null terminator */

/**
 * @brief SHA-256 state context structure.
 */
typedef struct {
    uint8_t  data[SHA256_BLOCK_SIZE]; /* Incomplete 64-byte block buffer */
    uint32_t datalen;                 /* Bytes currently held in data buffer */
    uint64_t bitlen;                  /* Total bits hashed so far */
    uint32_t state[8];                /* Working state words H0..H7 */
} sha256_ctx_t;

/**
 * @brief Initializes SHA-256 context with standard FIPS 180-4 initial hash values.
 * @param ctx Pointer to sha256_ctx_t to initialize.
 */
void sha256_init(sha256_ctx_t *ctx);

/**
 * @brief Ingests arbitrary input stream into SHA-256 context.
 * @param ctx Pointer to initialized sha256_ctx_t.
 * @param data Input byte buffer.
 * @param len Length of input buffer in bytes.
 */
void sha256_update(sha256_ctx_t *ctx, const void *data, size_t len);

/**
 * @brief Applies FIPS 180-4 padding, processes final block, and outputs 32-byte digest.
 *        Sanitizes (zeroes) internal context state upon return.
 * @param ctx Pointer to sha256_ctx_t.
 * @param hash Output buffer for 32-byte binary digest.
 */
void sha256_final(sha256_ctx_t *ctx, uint8_t hash[SHA256_DIGEST_SIZE]);

/**
 * @brief Convenience helper: one-shot SHA-256 hashing into binary digest.
 */
void sha256_hash(const void *data, size_t len, uint8_t hash[SHA256_DIGEST_SIZE]);

/**
 * @brief Formats 32-byte binary digest into 64 lowercase hex characters + '\0'.
 * @param hash 32-byte binary digest input.
 * @param out_hex Output string buffer (must have max_out >= 65).
 * @param max_out Capacity of out_hex buffer.
 * @return 0 on success, -1 on invalid parameter or buffer too small.
 */
int sha256_to_hex(const uint8_t hash[SHA256_DIGEST_SIZE], char *out_hex, size_t max_out);

/**
 * @brief Convenience helper: one-shot SHA-256 computation directly to hex string.
 * @return 0 on success, -1 on failure.
 */
int sha256_hash_hex(const void *data, size_t len, char *out_hex, size_t max_out);

#ifdef __cplusplus
}
#endif

#endif /* SHA256_H */
