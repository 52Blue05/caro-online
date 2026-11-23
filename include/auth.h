#ifndef AUTH_H
#define AUTH_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Auth status codes */
#define AUTH_OK              0
#define AUTH_ERR_INVALID    -1
#define AUTH_ERR_RANDOM     -2
#define AUTH_ERR_BUFFER     -3

/* Buffer & Sizing Constants */
#define AUTH_SALT_BYTES      16
#define AUTH_SALT_HEX_LEN    32
#define AUTH_SALT_STR_SIZE   33  /* 32 hex chars + null terminator */

#define AUTH_HASH_BYTES      32
#define AUTH_HASH_HEX_LEN    64
#define AUTH_HASH_STR_SIZE   65  /* 64 hex chars + null terminator */

/**
 * @brief Generates 16 cryptographically secure random bytes from /dev/urandom
 *        (or CryptGenRandom on Windows) and encodes them as 32 lowercase hex characters + null terminator.
 * 
 * @param salt_hex Output buffer of at least 33 bytes.
 * @param max_len Size of salt_hex buffer (must be >= AUTH_SALT_STR_SIZE).
 * @return AUTH_OK (0) on success, or AUTH_ERR_* code on failure.
 */
int auth_generate_salt(char *salt_hex, size_t max_len);

/**
 * @brief Computes SHA-256(password + salt_hex) and encodes digest as 64 lowercase
 *        hex characters + null terminator.
 * 
 * @param password Plaintext password string.
 * @param salt_hex 32-character lowercase hex salt.
 * @param hash_hex Output buffer of at least 65 bytes.
 * @param max_len Size of hash_hex buffer (must be >= AUTH_HASH_STR_SIZE).
 * @return AUTH_OK (0) on success, or AUTH_ERR_* code on failure.
 */
int auth_hash_password(const char *password, const char *salt_hex,
                       char *hash_hex, size_t max_len);

/**
 * @brief Verifies candidate password against stored salt and hash in constant time.
 * 
 * @param password Plaintext password to test.
 * @param salt_hex Stored 32-character hex salt.
 * @param stored_hash_hex Stored 64-character hex hash.
 * @return true if password matches stored credentials; false otherwise.
 */
bool auth_verify_password(const char *password, const char *salt_hex,
                          const char *stored_hash_hex);

#ifdef __cplusplus
}
#endif

#endif /* AUTH_H */
