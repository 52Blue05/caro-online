#ifndef GAME_H
#define GAME_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Caro (Gomoku) Board Constants */
#define CARO_BOARD_SIZE      15
#define CARO_WIN_STREAK      5
#define CARO_MAX_GAMES       32

/* Cell states */
typedef enum {
    CELL_EMPTY = 0,
    CELL_X     = 1,
    CELL_O     = 2
} caro_cell_t;

/* Game status */
typedef enum {
    GAME_STATUS_WAITING     = 0,
    GAME_STATUS_IN_PROGRESS = 1,
    GAME_STATUS_FINISHED    = 2,
    GAME_STATUS_ABORTED     = 3
} game_status_t;

/* Game result */
typedef enum {
    GAME_RESULT_NONE  = 0,
    GAME_RESULT_X_WON = 1,
    GAME_RESULT_O_WON = 2,
    GAME_RESULT_DRAW  = 3
} game_result_t;

/* Single move record */
typedef struct {
    uint32_t    seq;
    caro_cell_t player;
    int         row;
    int         col;
    uint32_t    timestamp;
} caro_move_t;

/* Game session state */
typedef struct {
    uint32_t        game_id;
    game_status_t   status;
    game_result_t   result;

    int             player_x_fd;    /* Socket FD of player playing X (goes first) */
    int             player_o_fd;    /* Socket FD of player playing O */
    uint32_t        player_x_id;    /* User/Client ID playing X */
    uint32_t        player_o_id;    /* User/Client ID playing O */
    char            player_x_name[32];
    char            player_o_name[32];
    caro_cell_t     current_turn;   /* CELL_X or CELL_O */

    caro_cell_t     board[CARO_BOARD_SIZE][CARO_BOARD_SIZE];
    uint32_t        move_count;
    uint32_t        expected_seq;   /* Incremented with each valid move, starts at 1 */

    int             last_row;
    int             last_col;
} game_t;

/**
 * @brief Initialize a new game session between two players.
 * 
 * Sets player FDs/IDs, resets board, sets current_turn to CELL_X,
 * sets status to GAME_STATUS_IN_PROGRESS, expected_seq to 1.
 */
void game_init(game_t *game, uint32_t game_id,
               int player_x_fd, int player_o_fd,
               uint32_t player_x_id, uint32_t player_o_id);

/**
 * @brief Reset board to all empty cells.
 */
void game_reset_board(game_t *game);

/**
 * @brief Validate whether a move (row, col) is within bounds [0..14] and unoccupied.
 */
bool game_is_valid_move(const game_t *game, int row, int col);

/**
 * @brief Place a move on the board, update turn and check game status.
 * 
 * @param game Pointer to game session
 * @param player Player making the move (CELL_X or CELL_O)
 * @param row Row index (0..14)
 * @param col Column index (0..14)
 * @return true if move was applied successfully, false otherwise.
 */
bool game_make_move(game_t *game, caro_cell_t player, int row, int col);

/**
 * @brief Check if placing a piece at (row, col) results in a win (>= 5 consecutive).
 * 
 * Evaluates 4 directions: horizontal, vertical, main diagonal, anti-diagonal.
 * Overline rule (>= 6) is considered a win per project specification.
 */
bool game_check_win(const game_t *game, int row, int col);

/**
 * @brief Check if board is full (all 225 cells occupied without a winner -> Draw).
 */
bool game_is_board_full(const game_t *game);

/**
 * @brief Get the cell value at (row, col). Returns CELL_EMPTY if out of bounds.
 */
caro_cell_t game_get_cell(const game_t *game, int row, int col);

#ifdef __cplusplus
}
#endif

#endif /* GAME_H */
