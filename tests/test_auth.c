#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include "sha256.h"
#include "auth.h"
#include "storage.h"

static int g_tests_run = 0;
static int g_tests_passed = 0;

#define TEST_ASSERT(cond, msg) do { \
    g_tests_run++; \
    if (!(cond)) { \
        printf("[-] FAIL: %s (line %d): %s\n", __func__, __LINE__, msg); \
    } else { \
        g_tests_passed++; \
        printf("[+] PASS: %s: %s\n", __func__, msg); \
    } \
} while(0)

/* Helper: count bit differences between two 64-char hex strings */
static int count_hex_diff_bits(const char *h1, const char *h2) {
    int diff_bits = 0;
    for (int i = 0; i < 64; ++i) {
        int v1 = (h1[i] >= '0' && h1[i] <= '9') ? (h1[i] - '0') :
                 (h1[i] >= 'a' && h1[i] <= 'f') ? (h1[i] - 'a' + 10) : 0;
        int v2 = (h2[i] >= '0' && h2[i] <= '9') ? (h2[i] - '0') :
                 (h2[i] >= 'a' && h2[i] <= 'f') ? (h2[i] - 'a' + 10) : 0;
        int x = v1 ^ v2;
        while (x) {
            diff_bits += (x & 1);
            x >>= 1;
        }
    }
    return diff_bits;
}

/* ========================================================================= */
/* Test Group 1: NIST Known Answer Tests (KAT) & Streaming                    */
/* ========================================================================= */
static void test_sha256_kat(void) {
    char hex[SHA256_HEX_SIZE];

    /* 1. Empty string */
    int rc = sha256_hash_hex("", 0, hex, sizeof(hex));
    TEST_ASSERT(rc == 0, "sha256_hash_hex empty input succeeds");
    TEST_ASSERT(strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0,
                "KAT empty string matches official digest");

    /* 2. 'abc' */
    rc = sha256_hash_hex("abc", 3, hex, sizeof(hex));
    TEST_ASSERT(rc == 0, "sha256_hash_hex 'abc' succeeds");
    TEST_ASSERT(strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0,
                "KAT 'abc' matches official digest");

    /* 3. 56-byte vector (448-bit padding boundary) */
    const char *v56 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    rc = sha256_hash_hex(v56, strlen(v56), hex, sizeof(hex));
    TEST_ASSERT(rc == 0, "sha256_hash_hex 56-byte vector succeeds");
    TEST_ASSERT(strcmp(hex, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1") == 0,
                "KAT 56-byte vector matches official digest");

    /* 4. Two-block 112-byte vector */
    const char *v112 = "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu";
    rc = sha256_hash_hex(v112, strlen(v112), hex, sizeof(hex));
    TEST_ASSERT(rc == 0, "sha256_hash_hex 112-byte vector succeeds");
    TEST_ASSERT(strcmp(hex, "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1") == 0,
                "KAT 112-byte vector matches official digest");

    /* 5. Streaming chunk equivalence */
    sha256_ctx_t ctx;
    uint8_t digest[SHA256_DIGEST_SIZE];
    sha256_init(&ctx);
    sha256_update(&ctx, "a", 1);
    sha256_update(&ctx, "b", 1);
    sha256_update(&ctx, "c", 1);
    sha256_final(&ctx, digest);
    sha256_to_hex(digest, hex, sizeof(hex));
    TEST_ASSERT(strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0,
                "Chunked streaming output matches one-shot KAT 'abc'");

    /* 6. Buffer too small check */
    rc = sha256_hash_hex("abc", 3, hex, 64);
    TEST_ASSERT(rc == -1, "sha256_hash_hex rejects buffer size < 65");
}

/* ========================================================================= */
/* Test Group 2: Salt Generation, Entropy & Uniqueness                       */
/* ========================================================================= */
static void test_auth_salt_generation(void) {
    char salt[AUTH_SALT_STR_SIZE];

    /* 1. Successful salt generation */
    int rc = auth_generate_salt(salt, sizeof(salt));
    TEST_ASSERT(rc == AUTH_OK, "auth_generate_salt succeeds");
    TEST_ASSERT(strlen(salt) == 32, "Generated salt length is exactly 32 hex chars");

    /* Validate hex characters */
    bool all_hex = true;
    for (int i = 0; i < 32; ++i) {
        char c = salt[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            all_hex = false;
            break;
        }
    }
    TEST_ASSERT(all_hex, "Generated salt contains strictly lowercase hex characters");

    /* 2. Buffer boundary check */
    rc = auth_generate_salt(salt, 32);
    TEST_ASSERT(rc == AUTH_ERR_BUFFER, "auth_generate_salt rejects buffer smaller than 33");

    rc = auth_generate_salt(NULL, 33);
    TEST_ASSERT(rc == AUTH_ERR_BUFFER, "auth_generate_salt rejects NULL pointer");

    /* 3. Uniqueness of 100 consecutive salts */
    char salts[100][AUTH_SALT_STR_SIZE];
    for (int i = 0; i < 100; ++i) {
        rc = auth_generate_salt(salts[i], sizeof(salts[i]));
        if (rc != AUTH_OK) break;
    }
    TEST_ASSERT(rc == AUTH_OK, "100 consecutive salts generated successfully");

    bool has_collision = false;
    for (int i = 0; i < 100; ++i) {
        for (int j = i + 1; j < 100; ++j) {
            if (strcmp(salts[i], salts[j]) == 0) {
                has_collision = true;
                break;
            }
        }
        if (has_collision) break;
    }
    TEST_ASSERT(!has_collision, "No collisions among 100 randomly generated 16-byte salts");
}

/* ========================================================================= */
/* Test Group 3: Password Hashing Determinism & Avalanche Effect             */
/* ========================================================================= */
static void test_auth_hashing(void) {
    const char *pwd = "SecretPass123!";
    const char *salt1 = "a1b2c3d4e5f60718293a4b5c6d7e8f90";
    const char *salt2 = "a1b2c3d4e5f60718293a4b5c6d7e8f91"; /* 1-bit difference in salt */
    char hash1[AUTH_HASH_STR_SIZE];
    char hash2[AUTH_HASH_STR_SIZE];
    char hash3[AUTH_HASH_STR_SIZE];

    /* 1. Determinism */
    int rc1 = auth_hash_password(pwd, salt1, hash1, sizeof(hash1));
    int rc2 = auth_hash_password(pwd, salt1, hash2, sizeof(hash2));
    TEST_ASSERT(rc1 == AUTH_OK && rc2 == AUTH_OK, "auth_hash_password succeeds for both calls");
    TEST_ASSERT(strcmp(hash1, hash2) == 0, "Identical password and salt produce identical hash");
    TEST_ASSERT(strlen(hash1) == 64, "Password hash is exactly 64 hex characters");

    /* 2. Avalanche effect on 1-char salt variation */
    int rc3 = auth_hash_password(pwd, salt2, hash3, sizeof(hash3));
    TEST_ASSERT(rc3 == AUTH_OK, "auth_hash_password with altered salt succeeds");
    TEST_ASSERT(strcmp(hash1, hash3) != 0, "Altered salt produces different hash");
    int diff_bits = count_hex_diff_bits(hash1, hash3);
    TEST_ASSERT(diff_bits >= 50, "Avalanche effect: flipping 1 hex digit alters >50 bits in 256-bit hash");

    /* 3. Parameter validation */
    TEST_ASSERT(auth_hash_password(NULL, salt1, hash1, sizeof(hash1)) == AUTH_ERR_INVALID,
                "auth_hash_password rejects NULL password");
    TEST_ASSERT(auth_hash_password(pwd, NULL, hash1, sizeof(hash1)) == AUTH_ERR_INVALID,
                "auth_hash_password rejects NULL salt");
    TEST_ASSERT(auth_hash_password(pwd, "short_salt", hash1, sizeof(hash1)) == AUTH_ERR_INVALID,
                "auth_hash_password rejects salt with length != 32");
    TEST_ASSERT(auth_hash_password(pwd, salt1, hash1, 64) == AUTH_ERR_INVALID,
                "auth_hash_password rejects max_len < 65");
}

/* ========================================================================= */
/* Test Group 4: Constant-Time Password Verification                         */
/* ========================================================================= */
static void test_auth_verification(void) {
    const char *pwd = "CorrectHorseBatteryStaple";
    const char *wrong_pwd = "WrongHorseBatteryStaple";
    char salt[AUTH_SALT_STR_SIZE];
    char hash[AUTH_HASH_STR_SIZE];

    auth_generate_salt(salt, sizeof(salt));
    auth_hash_password(pwd, salt, hash, sizeof(hash));

    /* 1. Correct password verification */
    TEST_ASSERT(auth_verify_password(pwd, salt, hash) == true,
                "auth_verify_password accepts matching password");

    /* 2. Wrong password rejection */
    TEST_ASSERT(auth_verify_password(wrong_pwd, salt, hash) == false,
                "auth_verify_password rejects mismatched password");

    /* 3. Wrong salt rejection */
    char wrong_salt[AUTH_SALT_STR_SIZE];
    auth_generate_salt(wrong_salt, sizeof(wrong_salt));
    TEST_ASSERT(auth_verify_password(pwd, wrong_salt, hash) == false,
                "auth_verify_password rejects wrong salt");

    /* 4. Single-bit tampered hash rejection */
    char tampered_hash[AUTH_HASH_STR_SIZE];
    strcpy(tampered_hash, hash);
    tampered_hash[0] = (tampered_hash[0] == 'a') ? 'b' : 'a';
    TEST_ASSERT(auth_verify_password(pwd, salt, tampered_hash) == false,
                "auth_verify_password rejects tampered hash at byte 0");

    strcpy(tampered_hash, hash);
    tampered_hash[31] = (tampered_hash[31] == 'a') ? 'b' : 'a';
    TEST_ASSERT(auth_verify_password(pwd, salt, tampered_hash) == false,
                "auth_verify_password rejects tampered hash at byte 31");

    strcpy(tampered_hash, hash);
    tampered_hash[63] = (tampered_hash[63] == 'a') ? 'b' : 'a';
    TEST_ASSERT(auth_verify_password(pwd, salt, tampered_hash) == false,
                "auth_verify_password rejects tampered hash at byte 63");

    /* 5. Case-insensitivity support for stored hash */
    char upper_hash[AUTH_HASH_STR_SIZE];
    for (int i = 0; i < 64; ++i) {
        char c = hash[i];
        if (c >= 'a' && c <= 'f') c = (char)(c - 'a' + 'A');
        upper_hash[i] = c;
    }
    upper_hash[64] = '\0';
    TEST_ASSERT(auth_verify_password(pwd, salt, upper_hash) == true,
                "auth_verify_password accepts uppercase stored hash hex");

    /* 6. Boundary conditions */
    TEST_ASSERT(auth_verify_password(NULL, salt, hash) == false,
                "auth_verify_password rejects NULL password");
    TEST_ASSERT(auth_verify_password(pwd, NULL, hash) == false,
                "auth_verify_password rejects NULL salt");
    TEST_ASSERT(auth_verify_password(pwd, salt, NULL) == false,
                "auth_verify_password rejects NULL stored hash");
    TEST_ASSERT(auth_verify_password(pwd, "bad_salt", hash) == false,
                "auth_verify_password rejects malformed salt length");
    TEST_ASSERT(auth_verify_password(pwd, salt, "bad_hash") == false,
                "auth_verify_password rejects malformed hash length");
}

/* ========================================================================= */
/* Test Group 5: SQLite Database Storage Roundtrip                           */
/* ========================================================================= */
static void test_auth_sqlite_roundtrip(void) {
    int rc = storage_init(":memory:");
    TEST_ASSERT(rc == STORAGE_OK, "storage_init(':memory:') succeeds");

    const char *username = "caro_master";
    const char *password = "SuperSecure123!";
    char salt[AUTH_SALT_STR_SIZE];
    char hash[AUTH_HASH_STR_SIZE];

    /* Generate salt & hash */
    rc = auth_generate_salt(salt, sizeof(salt));
    TEST_ASSERT(rc == AUTH_OK, "auth_generate_salt for user succeeds");
    rc = auth_hash_password(password, salt, hash, sizeof(hash));
    TEST_ASSERT(rc == AUTH_OK, "auth_hash_password for user succeeds");

    /* Register user in SQLite */
    uint32_t user_id = 0;
    rc = storage_register_user(username, salt, hash, &user_id);
    TEST_ASSERT(rc == STORAGE_OK, "storage_register_user succeeds");
    TEST_ASSERT(user_id > 0, "Registered user_id > 0");

    /* Retrieve user auth from SQLite */
    uint32_t retrieved_id = 0;
    char db_salt[AUTH_SALT_STR_SIZE];
    char db_hash[AUTH_HASH_STR_SIZE];
    rc = storage_get_user_auth(username, &retrieved_id, db_salt, sizeof(db_salt), db_hash, sizeof(db_hash));
    TEST_ASSERT(rc == STORAGE_OK, "storage_get_user_auth succeeds");
    TEST_ASSERT(retrieved_id == user_id, "Retrieved user_id matches registered user_id");
    TEST_ASSERT(strcmp(salt, db_salt) == 0, "Retrieved salt matches registered salt");
    TEST_ASSERT(strcmp(hash, db_hash) == 0, "Retrieved hash matches registered hash");

    /* Verify with correct password */
    bool auth_ok = auth_verify_password(password, db_salt, db_hash);
    TEST_ASSERT(auth_ok == true, "auth_verify_password returns true for retrieved DB credentials");

    /* Verify with wrong password */
    bool auth_bad = auth_verify_password("WrongPassword!", db_salt, db_hash);
    TEST_ASSERT(auth_bad == false, "auth_verify_password returns false for incorrect password against DB credentials");

    /* Check user stats */
    int points = 0, wins = 0, draws = 0, losses = 0;
    rc = storage_get_user_stats(user_id, &points, &wins, &draws, &losses);
    TEST_ASSERT(rc == STORAGE_OK, "storage_get_user_stats succeeds");
    TEST_ASSERT(points == 0 && wins == 0 && draws == 0 && losses == 0, "Initial stats are 0");

    storage_close();
}

/* ========================================================================= */
/* Main Test Runner                                                          */
/* ========================================================================= */
int main(void) {
    printf("====================================================\n");
    printf("        MILESTONE 3 - AUTH & SHA-256 TEST SUITE     \n");
    printf("====================================================\n");

    test_sha256_kat();
    test_auth_salt_generation();
    test_auth_hashing();
    test_auth_verification();
    test_auth_sqlite_roundtrip();

    printf("====================================================\n");
    printf("Auth Tests Summary: %d / %d passed\n", g_tests_passed, g_tests_run);
    printf("====================================================\n");

    return (g_tests_passed == g_tests_run) ? 0 : 1;
}
