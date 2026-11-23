#define _POSIX_C_SOURCE 200809L

#include "auth.h"
#include "sha256.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>

#if defined(_WIN32)
#include <windows.h>
#include <wincrypt.h>
#else
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#endif

int auth_generate_salt(char *salt_hex, size_t max_len) {
    if (!salt_hex || max_len < AUTH_SALT_STR_SIZE) {
        return AUTH_ERR_BUFFER;
    }

    uint8_t raw_salt[AUTH_SALT_BYTES];
    memset(raw_salt, 0, sizeof(raw_salt));

#if defined(_WIN32)
    HCRYPTPROV hProv = 0;
    if (!CryptAcquireContext(&hProv, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) {
        return AUTH_ERR_RANDOM;
    }
    if (!CryptGenRandom(hProv, sizeof(raw_salt), raw_salt)) {
        CryptReleaseContext(hProv, 0);
        return AUTH_ERR_RANDOM;
    }
    CryptReleaseContext(hProv, 0);
#else
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return AUTH_ERR_RANDOM;
    }
    size_t total_read = 0;
    while (total_read < sizeof(raw_salt)) {
        ssize_t n = read(fd, raw_salt + total_read, sizeof(raw_salt) - total_read);
        if (n < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return AUTH_ERR_RANDOM;
        }
        if (n == 0) {
            close(fd);
            return AUTH_ERR_RANDOM;
        }
        total_read += (size_t)n;
    }
    close(fd);
#endif

    static const char hex_digits[] = "0123456789abcdef";
    for (size_t i = 0; i < AUTH_SALT_BYTES; ++i) {
        salt_hex[i * 2]     = hex_digits[(raw_salt[i] >> 4) & 0x0F];
        salt_hex[i * 2 + 1] = hex_digits[raw_salt[i] & 0x0F];
    }
    salt_hex[AUTH_SALT_HEX_LEN] = '\0';

    /* Sanitize raw random bytes from stack */
    memset(raw_salt, 0, sizeof(raw_salt));
    return AUTH_OK;
}

int auth_hash_password(const char *password, const char *salt_hex,
                       char *hash_hex, size_t max_len) {
    if (!password || !salt_hex || !hash_hex || max_len < AUTH_HASH_STR_SIZE) {
        return AUTH_ERR_INVALID;
    }

    size_t salt_len = strlen(salt_hex);
    if (salt_len != AUTH_SALT_HEX_LEN) {
        return AUTH_ERR_INVALID;
    }

    /* Stream: SHA-256(password + salt_hex) without intermediate heap allocation */
    sha256_ctx_t ctx;
    uint8_t digest[SHA256_DIGEST_SIZE];

    sha256_init(&ctx);
    sha256_update(&ctx, password, strlen(password));
    sha256_update(&ctx, salt_hex, salt_len);
    sha256_final(&ctx, digest);

    int rc = sha256_to_hex(digest, hash_hex, max_len);
    memset(digest, 0, sizeof(digest));
    return (rc == 0) ? AUTH_OK : AUTH_ERR_BUFFER;
}

bool auth_verify_password(const char *password, const char *salt_hex,
                          const char *stored_hash_hex) {
    if (!password || !salt_hex || !stored_hash_hex) {
        return false;
    }

    if (strlen(salt_hex) != AUTH_SALT_HEX_LEN || strlen(stored_hash_hex) != AUTH_HASH_HEX_LEN) {
        return false;
    }

    char candidate_hash[AUTH_HASH_STR_SIZE];
    if (auth_hash_password(password, salt_hex, candidate_hash, sizeof(candidate_hash)) != AUTH_OK) {
        return false;
    }

    /* Constant-time comparison: evaluate all 64 characters unconditionally */
    volatile unsigned char diff = 0;
    for (size_t i = 0; i < AUTH_HASH_HEX_LEN; ++i) {
        unsigned char c1 = (unsigned char)candidate_hash[i];
        unsigned char c2 = (unsigned char)stored_hash_hex[i];
        /* Lowercase normalization in case stored_hash_hex was uppercase */
        if (c2 >= 'A' && c2 <= 'Z') c2 = (unsigned char)(c2 + ('a' - 'A'));
        diff |= (c1 ^ c2);
    }

    /* Sanitize candidate hash */
    memset(candidate_hash, 0, sizeof(candidate_hash));
    return (diff == 0);
}
