#include <string.h>
#include "game.h"

void game_reset_board(game_t *game) {
    if (!game) return;
    for (int r = 0; r < CARO_BOARD_SIZE; ++r) {
        for (int c = 0; c < CARO_BOARD_SIZE; ++c) {
            game->board[r][c] = CELL_EMPTY;
        }
    }
}

void game_init(game_t *game, uint32_t game_id,
               int player_x_fd, int player_o_fd,
               uint32_t player_x_id, uint32_t player_o_id) {
    if (!game) return;
    memset(game, 0, sizeof(*game));

    game->game_id = game_id;
    game->status = GAME_STATUS_IN_PROGRESS;
    game->result = GAME_RESULT_NONE;

    game->player_x_fd = player_x_fd;
    game->player_o_fd = player_o_fd;
    game->player_x_id = player_x_id;
    game->player_o_id = player_o_id;

    game->current_turn = CELL_X;
    game->move_count = 0;
    game->expected_seq = 1;
    game->last_row = -1;
    game->last_col = -1;

    game_reset_board(game);
}

bool game_is_valid_move(const game_t *game, int row, int col) {
    if (!game) return false;
    if (game->status != GAME_STATUS_IN_PROGRESS) return false;
    if (row < 0 || row >= CARO_BOARD_SIZE) return false;
    if (col < 0 || col >= CARO_BOARD_SIZE) return false;
    return (game->board[row][col] == CELL_EMPTY);
}

caro_cell_t game_get_cell(const game_t *game, int row, int col) {
    if (!game) return CELL_EMPTY;
    if (row < 0 || row >= CARO_BOARD_SIZE || col < 0 || col >= CARO_BOARD_SIZE) {
        return CELL_EMPTY;
    }
    return game->board[row][col];
}

bool game_check_win(const game_t *game, int row, int col) {
    if (!game) return false;
    if (row < 0 || row >= CARO_BOARD_SIZE || col < 0 || col >= CARO_BOARD_SIZE) {
        return false;
    }

    caro_cell_t target = game->board[row][col];
    if (target == CELL_EMPTY) return false;

    /* 4 directions: {dr, dc} */
    static const int dirs[4][2] = {
        { 0,  1 }, /* Horizontal (-) */
        { 1,  0 }, /* Vertical (|) */
        { 1,  1 }, /* Main diagonal (\) */
        { -1, 1 }  /* Anti-diagonal (/) */
    };

    for (int d = 0; d < 4; ++d) {
        int dr = dirs[d][0];
        int dc = dirs[d][1];
        int count = 1; /* Count current placed piece */

        /* Count forward */
        int r = row + dr;
        int c = col + dc;
        while (r >= 0 && r < CARO_BOARD_SIZE && c >= 0 && c < CARO_BOARD_SIZE &&
               game->board[r][c] == target) {
            count++;
            r += dr;
            c += dc;
        }

        /* Count backward */
        r = row - dr;
        c = col - dc;
        while (r >= 0 && r < CARO_BOARD_SIZE && c >= 0 && c < CARO_BOARD_SIZE &&
               game->board[r][c] == target) {
            count++;
            r -= dr;
            c -= dc;
        }

        /* Win condition: >= 5 consecutive pieces */
        if (count >= CARO_WIN_STREAK) {
            return true;
        }
    }

    return false;
}

bool game_is_board_full(const game_t *game) {
    if (!game) return false;
    for (int r = 0; r < CARO_BOARD_SIZE; ++r) {
        for (int c = 0; c < CARO_BOARD_SIZE; ++c) {
            if (game->board[r][c] == CELL_EMPTY) {
                return false;
            }
        }
    }
    return true;
}

bool game_make_move(game_t *game, caro_cell_t player, int row, int col) {
    if (!game) return false;
    if (player != game->current_turn) return false;
    if (!game_is_valid_move(game, row, col)) return false;

    game->board[row][col] = player;
    game->last_row = row;
    game->last_col = col;
    game->move_count++;
    game->expected_seq++;

    if (game_check_win(game, row, col)) {
        game->status = GAME_STATUS_FINISHED;
        game->result = (player == CELL_X) ? GAME_RESULT_X_WON : GAME_RESULT_O_WON;
        return true;
    }

    if (game_is_board_full(game)) {
        game->status = GAME_STATUS_FINISHED;
        game->result = GAME_RESULT_DRAW;
        return true;
    }

    /* Switch turn */
    game->current_turn = (player == CELL_X) ? CELL_O : CELL_X;
    return true;
}
