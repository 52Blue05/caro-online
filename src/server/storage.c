#include "storage.h"
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>

/* Global database handle and serialization mutex */
static sqlite3 *g_db = NULL;
static pthread_mutex_t g_storage_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Embedded DDL for self-contained database initialization */
static const char *SCHEMA_DDL =
    "PRAGMA foreign_keys = ON;\n"
    "CREATE TABLE IF NOT EXISTS users (\n"
    "    id            INTEGER PRIMARY KEY AUTOINCREMENT,\n"
    "    username      TEXT UNIQUE NOT NULL COLLATE NOCASE,\n"
    "    salt          TEXT NOT NULL,\n"
    "    password_hash TEXT NOT NULL,\n"
    "    points        INTEGER NOT NULL DEFAULT 0 CHECK (points >= 0),\n"
    "    wins          INTEGER NOT NULL DEFAULT 0 CHECK (wins >= 0),\n"
    "    draws         INTEGER NOT NULL DEFAULT 0 CHECK (draws >= 0),\n"
    "    losses        INTEGER NOT NULL DEFAULT 0 CHECK (losses >= 0),\n"
    "    created_at    DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP\n"
    ");\n"
    "CREATE TABLE IF NOT EXISTS games (\n"
    "    id            INTEGER PRIMARY KEY AUTOINCREMENT,\n"
    "    player_x_id   INTEGER NOT NULL REFERENCES users(id),\n"
    "    player_o_id   INTEGER NOT NULL REFERENCES users(id),\n"
    "    status        TEXT NOT NULL DEFAULT 'ONGOING' CHECK (status IN ('ONGOING', 'FINISHED', 'INTERRUPTED')),\n"
    "    outcome       TEXT,\n"
    "    reason        TEXT,\n"
    "    winner_id     INTEGER REFERENCES users(id),\n"
    "    created_at    DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,\n"
    "    settled_at    DATETIME,\n"
    "    CHECK (player_x_id <> player_o_id)\n"
    ");\n"
    "CREATE TABLE IF NOT EXISTS moves (\n"
    "    id            INTEGER PRIMARY KEY AUTOINCREMENT,\n"
    "    game_id       INTEGER NOT NULL REFERENCES games(id) ON DELETE CASCADE,\n"
    "    seq           INTEGER NOT NULL CHECK (seq BETWEEN 1 AND 225),\n"
    "    user_id       INTEGER NOT NULL REFERENCES users(id),\n"
    "    stone         TEXT NOT NULL CHECK (stone IN ('X', 'O')),\n"
    "    row           INTEGER NOT NULL CHECK (row BETWEEN 0 AND 14),\n"
    "    col           INTEGER NOT NULL CHECK (col BETWEEN 0 AND 14),\n"
    "    created_at    DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,\n"
    "    UNIQUE (game_id, seq),\n"
    "    UNIQUE (game_id, row, col)\n"
    ");\n"
    "CREATE INDEX IF NOT EXISTS idx_users_username ON users(username);\n"
    "CREATE INDEX IF NOT EXISTS idx_users_ranking  ON users(points DESC, wins DESC, username ASC);\n"
    "CREATE INDEX IF NOT EXISTS idx_games_players  ON games(player_x_id, player_o_id);\n"
    "CREATE INDEX IF NOT EXISTS idx_games_status   ON games(status);\n"
    "CREATE INDEX IF NOT EXISTS idx_moves_game_seq  ON moves(game_id, seq);\n";

/* ========================================================================= */
/* Lifecycle & Migration                                                     */
/* ========================================================================= */

static void storage_close_locked(void) {
    if (g_db) {
        sqlite3_close(g_db);
        g_db = NULL;
    }
}

int storage_init(const char *db_path) {
    if (!db_path) {
        return STORAGE_ERR_INVALID;
    }

    pthread_mutex_lock(&g_storage_mutex);
    if (g_db) {
        storage_close_locked();
    }

    int rc = sqlite3_open_v2(db_path, &g_db,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                             NULL);
    if (rc != SQLITE_OK || !g_db) {
        storage_close_locked();
        pthread_mutex_unlock(&g_storage_mutex);
        return STORAGE_ERR_GENERIC;
    }

    /* Set busy timeout to 5000 ms to avoid immediate lock failures */
    sqlite3_busy_timeout(g_db, 5000);

    /* Enforce foreign keys on this connection */
    rc = sqlite3_exec(g_db, "PRAGMA foreign_keys = ON;", NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        storage_close_locked();
        pthread_mutex_unlock(&g_storage_mutex);
        return STORAGE_ERR_GENERIC;
    }

    /* Enable WAL mode for disk files if not in-memory */
    if (strcmp(db_path, ":memory:") != 0) {
        sqlite3_exec(g_db, "PRAGMA journal_mode = WAL;", NULL, NULL, NULL);
    }

    /* Execute embedded DDL */
    char *err_msg = NULL;
    rc = sqlite3_exec(g_db, SCHEMA_DDL, NULL, NULL, &err_msg);
    if (rc != SQLITE_OK) {
        if (err_msg) {
            sqlite3_free(err_msg);
        }
        storage_close_locked();
        pthread_mutex_unlock(&g_storage_mutex);
        return STORAGE_ERR_GENERIC;
    }

    pthread_mutex_unlock(&g_storage_mutex);
    return STORAGE_OK;
}

void storage_close(void) {
    pthread_mutex_lock(&g_storage_mutex);
    storage_close_locked();
    pthread_mutex_unlock(&g_storage_mutex);
}

int storage_recover_crashed_games(void) {
    pthread_mutex_lock(&g_storage_mutex);
    if (!g_db) {
        pthread_mutex_unlock(&g_storage_mutex);
        return STORAGE_ERR_GENERIC;
    }

    const char *sql =
        "UPDATE games "
        "SET status = 'INTERRUPTED', "
        "    reason = 'SERVER_CRASH', "
        "    settled_at = CURRENT_TIMESTAMP "
        "WHERE status = 'ONGOING';";

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        pthread_mutex_unlock(&g_storage_mutex);
        return STORAGE_ERR_GENERIC;
    }

    rc = sqlite3_step(stmt);
    int changes = sqlite3_changes(g_db);
    sqlite3_finalize(stmt);

    pthread_mutex_unlock(&g_storage_mutex);
    if (rc != SQLITE_DONE) {
        return STORAGE_ERR_GENERIC;
    }

    return changes;
}

/* ========================================================================= */
/* User & Auth Operations                                                    */
/* ========================================================================= */

int storage_register_user(const char *username, const char *salt_hex,
                          const char *hash_hex, uint32_t *out_user_id) {
    if (!username || username[0] == '\0' ||
        !salt_hex || salt_hex[0] == '\0' ||
        !hash_hex || hash_hex[0] == '\0') {
        return STORAGE_ERR_INVALID;
    }

    pthread_mutex_lock(&g_storage_mutex);
    if (!g_db) {
        pthread_mutex_unlock(&g_storage_mutex);
        return STORAGE_ERR_GENERIC;
    }

    const char *sql =
        "INSERT INTO users (username, salt, password_hash) VALUES (?, ?, ?);";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        pthread_mutex_unlock(&g_storage_mutex);
        return STORAGE_ERR_GENERIC;
    }

    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, salt_hex, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, hash_hex, -1, SQLITE_STATIC);

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_DONE) {
        sqlite3_int64 rowid = sqlite3_last_insert_rowid(g_db);
        if (out_user_id) {
            *out_user_id = (uint32_t)rowid;
        }
        sqlite3_finalize(stmt);
        pthread_mutex_unlock(&g_storage_mutex);
        return STORAGE_OK;
    }

    int ext = sqlite3_extended_errcode(g_db);
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&g_storage_mutex);

    if ((ext & 0xFF) == SQLITE_CONSTRAINT) {
        return STORAGE_ERR_EXISTS;
    }
    if (rc == SQLITE_BUSY || rc == SQLITE_LOCKED) {
        return STORAGE_ERR_BUSY;
    }
    return STORAGE_ERR_GENERIC;
}

int storage_get_user_auth(const char *username, uint32_t *out_user_id,
                          char *out_salt_hex, size_t salt_len,
                          char *out_hash_hex, size_t hash_len) {
    if (!username || username[0] == '\0' || !out_salt_hex || !out_hash_hex) {
        return STORAGE_ERR_INVALID;
    }

    /* Require buffer space for 32 hex + null (33) and 64 hex + null (65) */
    if (salt_len < 33 || hash_len < 65) {
        return STORAGE_ERR_INVALID;
    }

    pthread_mutex_lock(&g_storage_mutex);
    if (!g_db) {
        pthread_mutex_unlock(&g_storage_mutex);
        return STORAGE_ERR_GENERIC;
    }

    const char *sql =
        "SELECT id, salt, password_hash FROM users WHERE username = ?;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        pthread_mutex_unlock(&g_storage_mutex);
        return STORAGE_ERR_GENERIC;
    }

    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        if (out_user_id) {
            *out_user_id = (uint32_t)sqlite3_column_int64(stmt, 0);
        }
        const unsigned char *s = sqlite3_column_text(stmt, 1);
        const unsigned char *h = sqlite3_column_text(stmt, 2);

        snprintf(out_salt_hex, salt_len, "%s", s ? (const char *)s : "");
        snprintf(out_hash_hex, hash_len, "%s", h ? (const char *)h : "");

        sqlite3_finalize(stmt);
        pthread_mutex_unlock(&g_storage_mutex);
        return STORAGE_OK;
    }

    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&g_storage_mutex);

    if (rc == SQLITE_DONE) {
        return STORAGE_ERR_NOTFOUND;
    }
    if (rc == SQLITE_BUSY || rc == SQLITE_LOCKED) {
        return STORAGE_ERR_BUSY;
    }
    return STORAGE_ERR_GENERIC;
}

int storage_get_user_stats(uint32_t user_id, int *points, int *wins,
                           int *draws, int *losses) {
    if (user_id == 0) {
        return STORAGE_ERR_INVALID;
    }

    pthread_mutex_lock(&g_storage_mutex);
    if (!g_db) {
        pthread_mutex_unlock(&g_storage_mutex);
        return STORAGE_ERR_GENERIC;
    }

    const char *sql =
        "SELECT points, wins, draws, losses FROM users WHERE id = ?;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        pthread_mutex_unlock(&g_storage_mutex);
        return STORAGE_ERR_GENERIC;
    }

    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)user_id);

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        if (points) *points = sqlite3_column_int(stmt, 0);
        if (wins)   *wins   = sqlite3_column_int(stmt, 1);
        if (draws)  *draws  = sqlite3_column_int(stmt, 2);
        if (losses) *losses = sqlite3_column_int(stmt, 3);
        sqlite3_finalize(stmt);
        pthread_mutex_unlock(&g_storage_mutex);
        return STORAGE_OK;
    }

    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&g_storage_mutex);

    if (rc == SQLITE_DONE) {
        return STORAGE_ERR_NOTFOUND;
    }
    if (rc == SQLITE_BUSY || rc == SQLITE_LOCKED) {
        return STORAGE_ERR_BUSY;
    }
    return STORAGE_ERR_GENERIC;
}

/* ========================================================================= */
/* Gameplay Operations                                                       */
/* ========================================================================= */

int storage_create_game(uint32_t player_x_id, uint32_t player_o_id,
                       uint32_t *out_game_id) {
    if (player_x_id == 0 || player_o_id == 0 ||
        player_x_id == player_o_id || !out_game_id) {
        return STORAGE_ERR_INVALID;
    }

    pthread_mutex_lock(&g_storage_mutex);
    if (!g_db) {
        pthread_mutex_unlock(&g_storage_mutex);
        return STORAGE_ERR_GENERIC;
    }

    const char *sql =
        "INSERT INTO games (player_x_id, player_o_id, status) VALUES (?, ?, 'ONGOING');";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        pthread_mutex_unlock(&g_storage_mutex);
        return STORAGE_ERR_GENERIC;
    }

    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)player_x_id);
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)player_o_id);

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_DONE) {
        *out_game_id = (uint32_t)sqlite3_last_insert_rowid(g_db);
        sqlite3_finalize(stmt);
        pthread_mutex_unlock(&g_storage_mutex);
        return STORAGE_OK;
    }

    int ext = sqlite3_extended_errcode(g_db);
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&g_storage_mutex);

    if (ext == SQLITE_CONSTRAINT_FOREIGNKEY) {
        return STORAGE_ERR_NOTFOUND;
    }
    if (ext == SQLITE_CONSTRAINT_CHECK) {
        return STORAGE_ERR_INVALID;
    }
    if (rc == SQLITE_BUSY || rc == SQLITE_LOCKED) {
        return STORAGE_ERR_BUSY;
    }
    return STORAGE_ERR_GENERIC;
}

int storage_record_move(uint32_t game_id, uint32_t seq, uint32_t user_id,
                       const char *stone, int row, int col) {
    if (game_id == 0 || user_id == 0) {
        return STORAGE_ERR_INVALID;
    }
    if (seq < 1 || seq > 225) {
        return STORAGE_ERR_INVALID;
    }
    if (!stone || (strcmp(stone, "X") != 0 && strcmp(stone, "O") != 0)) {
        return STORAGE_ERR_INVALID;
    }
    if (row < 0 || row > 14 || col < 0 || col > 14) {
        return STORAGE_ERR_INVALID;
    }

    pthread_mutex_lock(&g_storage_mutex);
    if (!g_db) {
        pthread_mutex_unlock(&g_storage_mutex);
        return STORAGE_ERR_GENERIC;
    }

    const char *sql =
        "INSERT INTO moves (game_id, seq, user_id, stone, row, col) VALUES (?, ?, ?, ?, ?, ?);";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        pthread_mutex_unlock(&g_storage_mutex);
        return STORAGE_ERR_GENERIC;
    }

    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)game_id);
    sqlite3_bind_int(stmt, 2, (int)seq);
    sqlite3_bind_int64(stmt, 3, (sqlite3_int64)user_id);
    sqlite3_bind_text(stmt, 4, stone, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 5, row);
    sqlite3_bind_int(stmt, 6, col);

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_DONE) {
        sqlite3_finalize(stmt);
        pthread_mutex_unlock(&g_storage_mutex);
        return STORAGE_OK;
    }

    int ext = sqlite3_extended_errcode(g_db);
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&g_storage_mutex);

    if (ext == SQLITE_CONSTRAINT_UNIQUE || ext == SQLITE_CONSTRAINT_PRIMARYKEY) {
        return STORAGE_ERR_EXISTS;
    }
    if (ext == SQLITE_CONSTRAINT_FOREIGNKEY) {
        return STORAGE_ERR_NOTFOUND;
    }
    if (ext == SQLITE_CONSTRAINT_CHECK) {
        return STORAGE_ERR_INVALID;
    }
    if (rc == SQLITE_BUSY || rc == SQLITE_LOCKED) {
        return STORAGE_ERR_BUSY;
    }
    return STORAGE_ERR_GENERIC;
}

int storage_settle_game(uint32_t game_id, const char *outcome,
                       const char *reason, uint32_t winner_id,
                       uint32_t loser_id, bool is_draw) {
    if (game_id == 0 || !outcome || outcome[0] == '\0' ||
        !reason || reason[0] == '\0') {
        return STORAGE_ERR_INVALID;
    }
    if (!is_draw) {
        if (winner_id == 0 || loser_id == 0 || winner_id == loser_id) {
            return STORAGE_ERR_INVALID;
        }
    }

    pthread_mutex_lock(&g_storage_mutex);
    if (!g_db) {
        pthread_mutex_unlock(&g_storage_mutex);
        return STORAGE_ERR_GENERIC;
    }

    /* Atomic transaction */
    int rc = sqlite3_exec(g_db, "BEGIN IMMEDIATE;", NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        pthread_mutex_unlock(&g_storage_mutex);
        if (rc == SQLITE_BUSY || rc == SQLITE_LOCKED) {
            return STORAGE_ERR_BUSY;
        }
        return STORAGE_ERR_GENERIC;
    }

    /* Step 1: Verify game existence, status, and participants */
    const char *check_sql =
        "SELECT status, player_x_id, player_o_id FROM games WHERE id = ?;";
    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(g_db, check_sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        sqlite3_exec(g_db, "ROLLBACK;", NULL, NULL, NULL);
        pthread_mutex_unlock(&g_storage_mutex);
        return STORAGE_ERR_GENERIC;
    }

    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)game_id);
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        sqlite3_exec(g_db, "ROLLBACK;", NULL, NULL, NULL);
        pthread_mutex_unlock(&g_storage_mutex);
        return (rc == SQLITE_DONE) ? STORAGE_ERR_NOTFOUND : STORAGE_ERR_GENERIC;
    }

    const unsigned char *status_str = sqlite3_column_text(stmt, 0);
    uint32_t px = (uint32_t)sqlite3_column_int64(stmt, 1);
    uint32_t po = (uint32_t)sqlite3_column_int64(stmt, 2);

    /* Must be ONGOING - reject double settlement */
    if (!status_str || strcmp((const char *)status_str, "ONGOING") != 0) {
        sqlite3_finalize(stmt);
        sqlite3_exec(g_db, "ROLLBACK;", NULL, NULL, NULL);
        pthread_mutex_unlock(&g_storage_mutex);
        return STORAGE_ERR_INVALID;
    }

    /* If not draw, participants must match */
    if (!is_draw) {
        if (!((winner_id == px && loser_id == po) ||
              (winner_id == po && loser_id == px))) {
            sqlite3_finalize(stmt);
            sqlite3_exec(g_db, "ROLLBACK;", NULL, NULL, NULL);
            pthread_mutex_unlock(&g_storage_mutex);
            return STORAGE_ERR_INVALID;
        }
    }

    sqlite3_finalize(stmt);
    stmt = NULL;

    /* Step 2: Update games record */
    const char *update_game_sql =
        "UPDATE games "
        "SET status = 'FINISHED', "
        "    outcome = ?, "
        "    reason = ?, "
        "    winner_id = ?, "
        "    settled_at = CURRENT_TIMESTAMP "
        "WHERE id = ?;";
    rc = sqlite3_prepare_v2(g_db, update_game_sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        sqlite3_exec(g_db, "ROLLBACK;", NULL, NULL, NULL);
        pthread_mutex_unlock(&g_storage_mutex);
        return STORAGE_ERR_GENERIC;
    }

    sqlite3_bind_text(stmt, 1, outcome, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, reason, -1, SQLITE_STATIC);
    if (is_draw || winner_id == 0) {
        sqlite3_bind_null(stmt, 3);
    } else {
        sqlite3_bind_int64(stmt, 3, (sqlite3_int64)winner_id);
    }
    sqlite3_bind_int64(stmt, 4, (sqlite3_int64)game_id);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    stmt = NULL;

    if (rc != SQLITE_DONE) {
        sqlite3_exec(g_db, "ROLLBACK;", NULL, NULL, NULL);
        pthread_mutex_unlock(&g_storage_mutex);
        return STORAGE_ERR_GENERIC;
    }

    /* Step 3: Update user stats */
    if (is_draw) {
        /* Both players get +1 point, +1 draw */
        const char *draw_sql =
            "UPDATE users SET points = points + 1, draws = draws + 1 WHERE id IN (?, ?);";
        rc = sqlite3_prepare_v2(g_db, draw_sql, -1, &stmt, NULL);
        if (rc != SQLITE_OK) {
            sqlite3_exec(g_db, "ROLLBACK;", NULL, NULL, NULL);
            pthread_mutex_unlock(&g_storage_mutex);
            return STORAGE_ERR_GENERIC;
        }

        sqlite3_bind_int64(stmt, 1, (sqlite3_int64)px);
        sqlite3_bind_int64(stmt, 2, (sqlite3_int64)po);

        rc = sqlite3_step(stmt);
        int ch = sqlite3_changes(g_db);
        sqlite3_finalize(stmt);
        stmt = NULL;

        if (rc != SQLITE_DONE || ch != 2) {
            sqlite3_exec(g_db, "ROLLBACK;", NULL, NULL, NULL);
            pthread_mutex_unlock(&g_storage_mutex);
            return STORAGE_ERR_GENERIC;
        }
    } else {
        /* Winner gets +3 points, +1 win */
        const char *win_sql =
            "UPDATE users SET points = points + 3, wins = wins + 1 WHERE id = ?;";
        rc = sqlite3_prepare_v2(g_db, win_sql, -1, &stmt, NULL);
        if (rc != SQLITE_OK) {
            sqlite3_exec(g_db, "ROLLBACK;", NULL, NULL, NULL);
            pthread_mutex_unlock(&g_storage_mutex);
            return STORAGE_ERR_GENERIC;
        }

        sqlite3_bind_int64(stmt, 1, (sqlite3_int64)winner_id);
        rc = sqlite3_step(stmt);
        int ch = sqlite3_changes(g_db);
        sqlite3_finalize(stmt);
        stmt = NULL;

        if (rc != SQLITE_DONE || ch != 1) {
            sqlite3_exec(g_db, "ROLLBACK;", NULL, NULL, NULL);
            pthread_mutex_unlock(&g_storage_mutex);
            return STORAGE_ERR_GENERIC;
        }

        /* Loser gets +1 loss (points unchanged) */
        const char *lose_sql =
            "UPDATE users SET losses = losses + 1 WHERE id = ?;";
        rc = sqlite3_prepare_v2(g_db, lose_sql, -1, &stmt, NULL);
        if (rc != SQLITE_OK) {
            sqlite3_exec(g_db, "ROLLBACK;", NULL, NULL, NULL);
            pthread_mutex_unlock(&g_storage_mutex);
            return STORAGE_ERR_GENERIC;
        }

        sqlite3_bind_int64(stmt, 1, (sqlite3_int64)loser_id);
        rc = sqlite3_step(stmt);
        ch = sqlite3_changes(g_db);
        sqlite3_finalize(stmt);
        stmt = NULL;

        if (rc != SQLITE_DONE || ch != 1) {
            sqlite3_exec(g_db, "ROLLBACK;", NULL, NULL, NULL);
            pthread_mutex_unlock(&g_storage_mutex);
            return STORAGE_ERR_GENERIC;
        }
    }

    /* Step 4: Commit transaction */
    rc = sqlite3_exec(g_db, "COMMIT;", NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        sqlite3_exec(g_db, "ROLLBACK;", NULL, NULL, NULL);
        pthread_mutex_unlock(&g_storage_mutex);
        if (rc == SQLITE_BUSY || rc == SQLITE_LOCKED) {
            return STORAGE_ERR_BUSY;
        }
        return STORAGE_ERR_GENERIC;
    }

    pthread_mutex_unlock(&g_storage_mutex);
    return STORAGE_OK;
}
