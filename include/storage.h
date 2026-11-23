#ifndef CARO_STORAGE_H
#define CARO_STORAGE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Return Status Codes */
#define STORAGE_OK           0
#define STORAGE_ERR_GENERIC -1
#define STORAGE_ERR_NOTFOUND -2
#define STORAGE_ERR_EXISTS   -3
#define STORAGE_ERR_BUSY     -4
#define STORAGE_ERR_INVALID  -5

/* Lifecycle & Migration */
int storage_init(const char *db_path);
void storage_close(void);
int storage_recover_crashed_games(void);

/* User & Auth Operations */
int storage_register_user(const char *username, const char *salt_hex, const char *hash_hex, uint32_t *out_user_id);
int storage_get_user_auth(const char *username, uint32_t *out_user_id, char *out_salt_hex, size_t salt_len, char *out_hash_hex, size_t hash_len);
int storage_get_user_stats(uint32_t user_id, int *points, int *wins, int *draws, int *losses);

/* Gameplay Operations */
int storage_create_game(uint32_t player_x_id, uint32_t player_o_id, uint32_t *out_game_id);
int storage_record_move(uint32_t game_id, uint32_t seq, uint32_t user_id, const char *stone, int row, int col);
int storage_settle_game(uint32_t game_id, const char *outcome, const char *reason, uint32_t winner_id, uint32_t loser_id, bool is_draw);

#endif /* CARO_STORAGE_H */
