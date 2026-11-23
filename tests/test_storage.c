#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
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

/* ========================================================================= */
/* Test 1: Storage Init, Close, and Re-init                                  */
/* ========================================================================= */
static void test_storage_init_and_close(void) {
    /* NULL path should fail */
    int rc = storage_init(NULL);
    TEST_ASSERT(rc == STORAGE_ERR_INVALID, "storage_init(NULL) returns STORAGE_ERR_INVALID");

    /* In-memory DB should succeed */
    rc = storage_init(":memory:");
    TEST_ASSERT(rc == STORAGE_OK, "storage_init(':memory:') returns STORAGE_OK");

    /* Close DB */
    storage_close();

    /* Subsequent operation on closed DB should return STORAGE_ERR_GENERIC */
    int points = 0, wins = 0, draws = 0, losses = 0;
    rc = storage_get_user_stats(1, &points, &wins, &draws, &losses);
    TEST_ASSERT(rc == STORAGE_ERR_GENERIC, "storage_get_user_stats on closed DB returns STORAGE_ERR_GENERIC");

    /* Safe double-close */
    storage_close();
    TEST_ASSERT(true, "Double storage_close() is safe");

    /* Re-open in-memory DB */
    rc = storage_init(":memory:");
    TEST_ASSERT(rc == STORAGE_OK, "Re-opening storage_init(':memory:') returns STORAGE_OK");
}

/* ========================================================================= */
/* Test 2: User Registration & Duplicate Constraints                         */
/* ========================================================================= */
static void test_storage_register_user(void) {
    const char *salt1 = "11112222333344445555666677778888";
    const char *hash1 = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    const char *salt2 = "99998888777766665555444433332222";
    const char *hash2 = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

    uint32_t uid1 = 0;
    int rc = storage_register_user("alice", salt1, hash1, &uid1);
    TEST_ASSERT(rc == STORAGE_OK, "Register 'alice' succeeds");
    TEST_ASSERT(uid1 == 1, "First user ID is 1");

    uint32_t uid2 = 0;
    rc = storage_register_user("bob", salt2, hash2, &uid2);
    TEST_ASSERT(rc == STORAGE_OK, "Register 'bob' succeeds");
    TEST_ASSERT(uid2 == 2, "Second user ID is 2");

    /* Duplicate username exact match */
    uint32_t dummy_uid = 0;
    rc = storage_register_user("alice", salt1, hash1, &dummy_uid);
    TEST_ASSERT(rc == STORAGE_ERR_EXISTS, "Exact duplicate 'alice' returns STORAGE_ERR_EXISTS");

    /* Case-variant duplicate: COLLATE NOCASE */
    rc = storage_register_user("Alice", salt1, hash1, &dummy_uid);
    TEST_ASSERT(rc == STORAGE_ERR_EXISTS, "Case-variant duplicate 'Alice' returns STORAGE_ERR_EXISTS");

    rc = storage_register_user("ALICE", salt1, hash1, &dummy_uid);
    TEST_ASSERT(rc == STORAGE_ERR_EXISTS, "Case-variant duplicate 'ALICE' returns STORAGE_ERR_EXISTS");

    /* Parameter validation checks */
    rc = storage_register_user(NULL, salt1, hash1, &dummy_uid);
    TEST_ASSERT(rc == STORAGE_ERR_INVALID, "Register NULL username returns STORAGE_ERR_INVALID");

    rc = storage_register_user("", salt1, hash1, &dummy_uid);
    TEST_ASSERT(rc == STORAGE_ERR_INVALID, "Register empty username returns STORAGE_ERR_INVALID");

    rc = storage_register_user("charlie", NULL, hash1, &dummy_uid);
    TEST_ASSERT(rc == STORAGE_ERR_INVALID, "Register NULL salt returns STORAGE_ERR_INVALID");

    rc = storage_register_user("charlie", salt1, NULL, &dummy_uid);
    TEST_ASSERT(rc == STORAGE_ERR_INVALID, "Register NULL hash returns STORAGE_ERR_INVALID");

    /* NULL out_user_id should still succeed */
    rc = storage_register_user("charlie", salt1, hash1, NULL);
    TEST_ASSERT(rc == STORAGE_OK, "Register with NULL out_user_id succeeds");
}

/* ========================================================================= */
/* Test 3: User Auth Retrieval                                               */
/* ========================================================================= */
static void test_storage_get_user_auth(void) {
    char salt_buf[64] = {0};
    char hash_buf[128] = {0};
    uint32_t uid = 0;

    /* Successful lookup */
    int rc = storage_get_user_auth("alice", &uid, salt_buf, sizeof(salt_buf), hash_buf, sizeof(hash_buf));
    TEST_ASSERT(rc == STORAGE_OK, "Auth lookup for 'alice' returns STORAGE_OK");
    TEST_ASSERT(uid == 1, "Auth lookup returns correct uid (1)");
    TEST_ASSERT(strcmp(salt_buf, "11112222333344445555666677778888") == 0, "Auth salt matches");
    TEST_ASSERT(strcmp(hash_buf, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa") == 0, "Auth hash matches");

    /* Case-insensitive lookup */
    memset(salt_buf, 0, sizeof(salt_buf));
    memset(hash_buf, 0, sizeof(hash_buf));
    rc = storage_get_user_auth("ALICE", &uid, salt_buf, sizeof(salt_buf), hash_buf, sizeof(hash_buf));
    TEST_ASSERT(rc == STORAGE_OK, "Case-insensitive auth lookup 'ALICE' returns STORAGE_OK");
    TEST_ASSERT(uid == 1, "Case-insensitive auth returns correct uid (1)");

    /* Non-existent user */
    rc = storage_get_user_auth("nonexistent_user", &uid, salt_buf, sizeof(salt_buf), hash_buf, sizeof(hash_buf));
    TEST_ASSERT(rc == STORAGE_ERR_NOTFOUND, "Auth lookup for nonexistent user returns STORAGE_ERR_NOTFOUND");

    /* Buffer capacity limits: salt requires >= 33, hash requires >= 65 */
    rc = storage_get_user_auth("alice", &uid, salt_buf, 32, hash_buf, sizeof(hash_buf));
    TEST_ASSERT(rc == STORAGE_ERR_INVALID, "Undersized salt buffer (<33) returns STORAGE_ERR_INVALID");

    rc = storage_get_user_auth("alice", &uid, salt_buf, sizeof(salt_buf), hash_buf, 64);
    TEST_ASSERT(rc == STORAGE_ERR_INVALID, "Undersized hash buffer (<65) returns STORAGE_ERR_INVALID");

    /* NULL parameters */
    rc = storage_get_user_auth(NULL, &uid, salt_buf, sizeof(salt_buf), hash_buf, sizeof(hash_buf));
    TEST_ASSERT(rc == STORAGE_ERR_INVALID, "NULL username returns STORAGE_ERR_INVALID");

    rc = storage_get_user_auth("alice", &uid, NULL, sizeof(salt_buf), hash_buf, sizeof(hash_buf));
    TEST_ASSERT(rc == STORAGE_ERR_INVALID, "NULL salt_buf returns STORAGE_ERR_INVALID");

    rc = storage_get_user_auth("alice", &uid, salt_buf, sizeof(salt_buf), NULL, sizeof(hash_buf));
    TEST_ASSERT(rc == STORAGE_ERR_INVALID, "NULL hash_buf returns STORAGE_ERR_INVALID");
}

/* ========================================================================= */
/* Test 4: User Stats Retrieval                                              */
/* ========================================================================= */
static void test_storage_get_user_stats(void) {
    int points = -1, wins = -1, draws = -1, losses = -1;

    /* Initial stats for newly registered user should be 0 */
    int rc = storage_get_user_stats(1, &points, &wins, &draws, &losses);
    TEST_ASSERT(rc == STORAGE_OK, "get_user_stats for user 1 returns STORAGE_OK");
    TEST_ASSERT(points == 0, "Initial points == 0");
    TEST_ASSERT(wins == 0, "Initial wins == 0");
    TEST_ASSERT(draws == 0, "Initial draws == 0");
    TEST_ASSERT(losses == 0, "Initial losses == 0");

    /* Partial retrieval with some NULL pointers */
    int points_only = -1;
    rc = storage_get_user_stats(1, &points_only, NULL, NULL, NULL);
    TEST_ASSERT(rc == STORAGE_OK, "Partial get_user_stats (points only) returns STORAGE_OK");
    TEST_ASSERT(points_only == 0, "Partial points value is 0");

    /* Invalid user_id == 0 */
    rc = storage_get_user_stats(0, &points, &wins, &draws, &losses);
    TEST_ASSERT(rc == STORAGE_ERR_INVALID, "user_id == 0 returns STORAGE_ERR_INVALID");

    /* Non-existent user_id */
    rc = storage_get_user_stats(9999, &points, &wins, &draws, &losses);
    TEST_ASSERT(rc == STORAGE_ERR_NOTFOUND, "Nonexistent user_id returns STORAGE_ERR_NOTFOUND");
}

/* ========================================================================= */
/* Test 5: Game Creation & Constraints                                       */
/* ========================================================================= */
static void test_storage_create_game(void) {
    uint32_t gid1 = 0;

    /* Valid game creation between alice (1) and bob (2) */
    int rc = storage_create_game(1, 2, &gid1);
    TEST_ASSERT(rc == STORAGE_OK, "storage_create_game(1, 2) succeeds");
    TEST_ASSERT(gid1 == 1, "First game ID is 1");

    /* Same player for X and O rejected by CHECK (player_x_id <> player_o_id) */
    uint32_t dummy_gid = 0;
    rc = storage_create_game(1, 1, &dummy_gid);
    TEST_ASSERT(rc == STORAGE_ERR_INVALID, "Game with same player (1, 1) returns STORAGE_ERR_INVALID");

    /* Player ID == 0 rejected */
    rc = storage_create_game(0, 2, &dummy_gid);
    TEST_ASSERT(rc == STORAGE_ERR_INVALID, "Game with player_x_id == 0 returns STORAGE_ERR_INVALID");

    rc = storage_create_game(1, 0, &dummy_gid);
    TEST_ASSERT(rc == STORAGE_ERR_INVALID, "Game with player_o_id == 0 returns STORAGE_ERR_INVALID");

    /* NULL out_game_id rejected */
    rc = storage_create_game(1, 2, NULL);
    TEST_ASSERT(rc == STORAGE_ERR_INVALID, "Game with NULL out_game_id returns STORAGE_ERR_INVALID");

    /* Non-existent player rejected by Foreign Key constraint */
    rc = storage_create_game(9999, 2, &dummy_gid);
    TEST_ASSERT(rc == STORAGE_ERR_NOTFOUND, "Game with nonexistent player_x_id returns STORAGE_ERR_NOTFOUND (FK)");

    rc = storage_create_game(1, 9999, &dummy_gid);
    TEST_ASSERT(rc == STORAGE_ERR_NOTFOUND, "Game with nonexistent player_o_id returns STORAGE_ERR_NOTFOUND (FK)");
}

/* ========================================================================= */
/* Test 6: Move Recording, Bounds & Sequence Constraints                     */
/* ========================================================================= */
static void test_storage_record_move(void) {
    /* Valid moves in game 1 */
    int rc = storage_record_move(1, 1, 1, "X", 7, 7);
    TEST_ASSERT(rc == STORAGE_OK, "Record valid move 1 (seq=1, stone='X', row=7, col=7) succeeds");

    rc = storage_record_move(1, 2, 2, "O", 7, 8);
    TEST_ASSERT(rc == STORAGE_OK, "Record valid move 2 (seq=2, stone='O', row=7, col=8) succeeds");

    /* Duplicate sequence number in same game (UNIQUE(game_id, seq)) */
    rc = storage_record_move(1, 2, 1, "X", 8, 8);
    TEST_ASSERT(rc == STORAGE_ERR_EXISTS, "Duplicate seq=2 in game 1 returns STORAGE_ERR_EXISTS");

    /* Duplicate coordinate placement in same game (UNIQUE(game_id, row, col)) */
    rc = storage_record_move(1, 3, 1, "X", 7, 7);
    TEST_ASSERT(rc == STORAGE_ERR_EXISTS, "Duplicate coordinate (7,7) returns STORAGE_ERR_EXISTS");

    /* Corner coordinate tests [0..14] */
    rc = storage_record_move(1, 3, 1, "X", 0, 0);
    TEST_ASSERT(rc == STORAGE_OK, "Boundary move at (0, 0) succeeds");

    rc = storage_record_move(1, 4, 2, "O", 14, 14);
    TEST_ASSERT(rc == STORAGE_OK, "Boundary move at (14, 14) succeeds");

    /* Out of bounds row/col rejected */
    rc = storage_record_move(1, 5, 1, "X", -1, 5);
    TEST_ASSERT(rc == STORAGE_ERR_INVALID, "Negative row (-1) returns STORAGE_ERR_INVALID");

    rc = storage_record_move(1, 5, 1, "X", 15, 5);
    TEST_ASSERT(rc == STORAGE_ERR_INVALID, "Row 15 returns STORAGE_ERR_INVALID");

    rc = storage_record_move(1, 5, 1, "X", 5, -1);
    TEST_ASSERT(rc == STORAGE_ERR_INVALID, "Negative col (-1) returns STORAGE_ERR_INVALID");

    rc = storage_record_move(1, 5, 1, "X", 5, 15);
    TEST_ASSERT(rc == STORAGE_ERR_INVALID, "Col 15 returns STORAGE_ERR_INVALID");

    /* Sequence boundary checks [1..225] */
    rc = storage_record_move(1, 0, 1, "X", 5, 5);
    TEST_ASSERT(rc == STORAGE_ERR_INVALID, "seq == 0 returns STORAGE_ERR_INVALID");

    rc = storage_record_move(1, 226, 1, "X", 5, 5);
    TEST_ASSERT(rc == STORAGE_ERR_INVALID, "seq == 226 returns STORAGE_ERR_INVALID");

    /* Invalid stone checks */
    rc = storage_record_move(1, 5, 1, "Z", 5, 5);
    TEST_ASSERT(rc == STORAGE_ERR_INVALID, "Invalid stone 'Z' returns STORAGE_ERR_INVALID");

    rc = storage_record_move(1, 5, 1, "", 5, 5);
    TEST_ASSERT(rc == STORAGE_ERR_INVALID, "Empty stone string returns STORAGE_ERR_INVALID");

    rc = storage_record_move(1, 5, 1, NULL, 5, 5);
    TEST_ASSERT(rc == STORAGE_ERR_INVALID, "NULL stone pointer returns STORAGE_ERR_INVALID");

    /* Non-existent game_id (FK failure) */
    rc = storage_record_move(9999, 1, 1, "X", 5, 5);
    TEST_ASSERT(rc == STORAGE_ERR_NOTFOUND, "Move on nonexistent game_id returns STORAGE_ERR_NOTFOUND");

    /* Non-existent user_id (FK failure) */
    rc = storage_record_move(1, 5, 9999, "X", 5, 5);
    TEST_ASSERT(rc == STORAGE_ERR_NOTFOUND, "Move with nonexistent user_id returns STORAGE_ERR_NOTFOUND");
}

/* ========================================================================= */
/* Test 7: Game Settlement — Win (+3/+0)                                     */
/* ========================================================================= */
static void test_storage_settle_game_win(void) {
    /* Create game 2 between player 1 and player 2 */
    uint32_t gid2 = 0;
    int rc = storage_create_game(1, 2, &gid2);
    TEST_ASSERT(rc == STORAGE_OK, "Create game 2 succeeds");

    /* Settle game 2: player 1 wins against player 2 */
    rc = storage_settle_game(gid2, "WIN_X", "FIVE_IN_A_ROW", 1, 2, false);
    TEST_ASSERT(rc == STORAGE_OK, "storage_settle_game(WIN_X) returns STORAGE_OK");

    /* Verify stats: player 1 gets +3 points, +1 win */
    int p1 = 0, w1 = 0, d1 = 0, l1 = 0;
    storage_get_user_stats(1, &p1, &w1, &d1, &l1);
    TEST_ASSERT(p1 == 3, "Winner player 1 points == 3");
    TEST_ASSERT(w1 == 1, "Winner player 1 wins == 1");
    TEST_ASSERT(d1 == 0, "Winner player 1 draws == 0");
    TEST_ASSERT(l1 == 0, "Winner player 1 losses == 0");

    /* Verify stats: player 2 gets +0 points, +1 loss */
    int p2 = 0, w2 = 0, d2 = 0, l2 = 0;
    storage_get_user_stats(2, &p2, &w2, &d2, &l2);
    TEST_ASSERT(p2 == 0, "Loser player 2 points == 0");
    TEST_ASSERT(w2 == 0, "Loser player 2 wins == 0");
    TEST_ASSERT(d2 == 0, "Loser player 2 draws == 0");
    TEST_ASSERT(l2 == 1, "Loser player 2 losses == 1");
}

/* ========================================================================= */
/* Test 8: Game Settlement — Draw (+1/+1)                                    */
/* ========================================================================= */
static void test_storage_settle_game_draw(void) {
    /* Create game 3 between player 1 and player 2 */
    uint32_t gid3 = 0;
    int rc = storage_create_game(1, 2, &gid3);
    TEST_ASSERT(rc == STORAGE_OK, "Create game 3 succeeds");

    /* Settle game 3: draw */
    rc = storage_settle_game(gid3, "DRAW", "BOARD_FULL", 0, 0, true);
    TEST_ASSERT(rc == STORAGE_OK, "storage_settle_game(DRAW) returns STORAGE_OK");

    /* Verify stats: player 1 gets +1 point, +1 draw (total points: 3+1 = 4) */
    int p1 = 0, w1 = 0, d1 = 0, l1 = 0;
    storage_get_user_stats(1, &p1, &w1, &d1, &l1);
    TEST_ASSERT(p1 == 4, "Player 1 points after draw == 4");
    TEST_ASSERT(w1 == 1, "Player 1 wins after draw == 1");
    TEST_ASSERT(d1 == 1, "Player 1 draws after draw == 1");
    TEST_ASSERT(l1 == 0, "Player 1 losses after draw == 0");

    /* Verify stats: player 2 gets +1 point, +1 draw (total points: 0+1 = 1) */
    int p2 = 0, w2 = 0, d2 = 0, l2 = 0;
    storage_get_user_stats(2, &p2, &w2, &d2, &l2);
    TEST_ASSERT(p2 == 1, "Player 2 points after draw == 1");
    TEST_ASSERT(w2 == 0, "Player 2 wins after draw == 0");
    TEST_ASSERT(d2 == 1, "Player 2 draws after draw == 1");
    TEST_ASSERT(l2 == 1, "Player 2 losses after draw == 1");
}

/* ========================================================================= */
/* Test 9: Double Settlement & Re-settle Rejection (Idempotency)             */
/* ========================================================================= */
static void test_storage_settle_game_double_settle(void) {
    /* Attempt to re-settle game 2 (already FINISHED) */
    int rc = storage_settle_game(2, "WIN_X", "FIVE_IN_A_ROW", 1, 2, false);
    TEST_ASSERT(rc == STORAGE_ERR_INVALID, "Double settlement on game 2 returns STORAGE_ERR_INVALID");

    /* Attempt to re-settle game 3 (already FINISHED) */
    rc = storage_settle_game(3, "DRAW", "BOARD_FULL", 0, 0, true);
    TEST_ASSERT(rc == STORAGE_ERR_INVALID, "Double settlement on game 3 returns STORAGE_ERR_INVALID");

    /* Verify stats are unchanged */
    int p1 = 0, w1 = 0, d1 = 0, l1 = 0;
    storage_get_user_stats(1, &p1, &w1, &d1, &l1);
    TEST_ASSERT(p1 == 4 && w1 == 1 && d1 == 1 && l1 == 0, "Player 1 stats unchanged after rejected re-settle");

    int p2 = 0, w2 = 0, d2 = 0, l2 = 0;
    storage_get_user_stats(2, &p2, &w2, &d2, &l2);
    TEST_ASSERT(p2 == 1 && w2 == 0 && d2 == 1 && l2 == 1, "Player 2 stats unchanged after rejected re-settle");
}

/* ========================================================================= */
/* Test 10: Settle Validation & Participant Mismatch                         */
/* ========================================================================= */
static void test_storage_settle_game_validation(void) {
    /* Register user 3 */
    uint32_t uid3 = 0;
    int rc = storage_register_user("david", "salt3", "hash3", &uid3);
    TEST_ASSERT(rc == STORAGE_OK, "Register 'david' succeeds");

    /* Create game 4 between player 1 and player 2 */
    uint32_t gid4 = 0;
    rc = storage_create_game(1, 2, &gid4);
    TEST_ASSERT(rc == STORAGE_OK, "Create game 4 succeeds");

    /* Settle with user 3 as winner (not a participant) */
    rc = storage_settle_game(gid4, "WIN_X", "RESIGN", uid3, 2, false);
    TEST_ASSERT(rc == STORAGE_ERR_INVALID, "Settle with non-participant winner returns STORAGE_ERR_INVALID");

    /* Settle with user 3 as loser (not a participant) */
    rc = storage_settle_game(gid4, "WIN_X", "RESIGN", 1, uid3, false);
    TEST_ASSERT(rc == STORAGE_ERR_INVALID, "Settle with non-participant loser returns STORAGE_ERR_INVALID");

    /* Non-existent game ID */
    rc = storage_settle_game(9999, "WIN_X", "RESIGN", 1, 2, false);
    TEST_ASSERT(rc == STORAGE_ERR_NOTFOUND, "Settle nonexistent game_id returns STORAGE_ERR_NOTFOUND");

    /* NULL outcome / reason */
    rc = storage_settle_game(gid4, NULL, "RESIGN", 1, 2, false);
    TEST_ASSERT(rc == STORAGE_ERR_INVALID, "NULL outcome returns STORAGE_ERR_INVALID");

    rc = storage_settle_game(gid4, "WIN_X", NULL, 1, 2, false);
    TEST_ASSERT(rc == STORAGE_ERR_INVALID, "NULL reason returns STORAGE_ERR_INVALID");

    rc = storage_settle_game(gid4, "", "RESIGN", 1, 2, false);
    TEST_ASSERT(rc == STORAGE_ERR_INVALID, "Empty outcome returns STORAGE_ERR_INVALID");

    /* Win mode with winner_id == 0 or winner == loser */
    rc = storage_settle_game(gid4, "WIN_X", "RESIGN", 0, 2, false);
    TEST_ASSERT(rc == STORAGE_ERR_INVALID, "Win mode with winner_id == 0 returns STORAGE_ERR_INVALID");

    rc = storage_settle_game(gid4, "WIN_X", "RESIGN", 1, 1, false);
    TEST_ASSERT(rc == STORAGE_ERR_INVALID, "Win mode with winner == loser returns STORAGE_ERR_INVALID");
}

/* ========================================================================= */
/* Test 11: Crash Recovery of Ongoing Games                                  */
/* ========================================================================= */
static void test_storage_recover_crashed_games(void) {
    /* At this point, game 1 and game 4 are ONGOING */
    /* Create game 5 between player 2 and player 3 (also ONGOING) */
    uint32_t gid5 = 0;
    int rc = storage_create_game(2, 4, &gid5);
    TEST_ASSERT(rc == STORAGE_OK, "Create game 5 succeeds");

    /* Recover crashed games */
    int recovered = storage_recover_crashed_games();
    /* Games 1, 4, and 5 were ONGOING -> 3 recovered */
    TEST_ASSERT(recovered == 3, "storage_recover_crashed_games recovered 3 ongoing games");

    /* Re-running recovery should find 0 ongoing games */
    recovered = storage_recover_crashed_games();
    TEST_ASSERT(recovered == 0, "Subsequent recovery finds 0 ongoing games");

    /* Settling an interrupted game should be rejected */
    rc = storage_settle_game(gid5, "WIN_X", "RESIGN", 2, 4, false);
    TEST_ASSERT(rc == STORAGE_ERR_INVALID, "Settling an INTERRUPTED game returns STORAGE_ERR_INVALID");
}

/* ========================================================================= */
/* Test 12: SQL Injection Defense via Prepared Statements                    */
/* ========================================================================= */
static void test_storage_sql_injection_defense(void) {
    /* Register users with SQL injection payloads */
    const char *payload1 = "admin' OR '1'='1";
    const char *payload2 = "'); DROP TABLE users; --";
    uint32_t uid_inj1 = 0;
    uint32_t uid_inj2 = 0;

    int rc = storage_register_user(payload1, "salt_inj", "hash_inj", &uid_inj1);
    TEST_ASSERT(rc == STORAGE_OK, "Register username with quotes succeeds as literal string");

    rc = storage_register_user(payload2, "salt_inj2", "hash_inj2", &uid_inj2);
    TEST_ASSERT(rc == STORAGE_OK, "Register username with DROP TABLE payload succeeds as literal string");

    /* Lookup user with payload */
    char salt[64] = {0};
    char hash[128] = {0};
    uint32_t found_uid = 0;
    rc = storage_get_user_auth(payload1, &found_uid, salt, sizeof(salt), hash, sizeof(hash));
    TEST_ASSERT(rc == STORAGE_OK, "Auth lookup with SQL payload succeeds");
    TEST_ASSERT(found_uid == uid_inj1, "Found correct UID for SQL payload user");

    /* Verify table users still exists and normal user is intact */
    int p = 0, w = 0, d = 0, l = 0;
    rc = storage_get_user_stats(1, &p, &w, &d, &l);
    TEST_ASSERT(rc == STORAGE_OK, "Table users intact: user 1 stats query succeeds");
}

/* ========================================================================= */
/* Test 13: Disk-File Database Persistence                                   */
/* ========================================================================= */
static void test_storage_file_db_persistence(void) {
    const char *temp_db = "tests/test_storage_persist.db";
    unlink(temp_db);
    unlink("tests/test_storage_persist.db-wal");
    unlink("tests/test_storage_persist.db-shm");

    /* Open disk DB */
    int rc = storage_init(temp_db);
    TEST_ASSERT(rc == STORAGE_OK, "storage_init(file) returns STORAGE_OK");

    uint32_t persist_uid = 0;
    rc = storage_register_user("persist_guy", "salt_p", "hash_p", &persist_uid);
    TEST_ASSERT(rc == STORAGE_OK, "Register user in file DB succeeds");

    /* Close and reopen */
    storage_close();

    rc = storage_init(temp_db);
    TEST_ASSERT(rc == STORAGE_OK, "Re-opening file DB returns STORAGE_OK");

    char salt[64] = {0};
    char hash[128] = {0};
    uint32_t verify_uid = 0;
    rc = storage_get_user_auth("persist_guy", &verify_uid, salt, sizeof(salt), hash, sizeof(hash));
    TEST_ASSERT(rc == STORAGE_OK, "Query user after reopening file DB succeeds");
    TEST_ASSERT(verify_uid == persist_uid, "Verified UID matches persisted UID");
    TEST_ASSERT(strcmp(salt, "salt_p") == 0, "Persisted salt matches");
    TEST_ASSERT(strcmp(hash, "hash_p") == 0, "Persisted hash matches");

    storage_close();
    unlink(temp_db);
    unlink("tests/test_storage_persist.db-wal");
    unlink("tests/test_storage_persist.db-shm");
}

/* ========================================================================= */
/* Main Test Runner                                                          */
/* ========================================================================= */
int main(void) {
    printf("=========================================\n");
    printf("     RUNNING STORAGE UNIT TESTS (M2)     \n");
    printf("=========================================\n");

    test_storage_init_and_close();
    test_storage_register_user();
    test_storage_get_user_auth();
    test_storage_get_user_stats();
    test_storage_create_game();
    test_storage_record_move();
    test_storage_settle_game_win();
    test_storage_settle_game_draw();
    test_storage_settle_game_double_settle();
    test_storage_settle_game_validation();
    test_storage_recover_crashed_games();
    test_storage_sql_injection_defense();
    test_storage_file_db_persistence();

    printf("=========================================\n");
    printf("   RESULTS: %d / %d tests passed\n", g_tests_passed, g_tests_run);
    printf("=========================================\n");

    return (g_tests_passed == g_tests_run) ? 0 : 1;
}
