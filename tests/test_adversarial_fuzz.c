#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "game.h"
#include "protocol.h"

static int g_fuzz_passed = 0;
static int g_fuzz_failed = 0;

#define FUZZ_ASSERT(cond, msg) do { \
    if (!(cond)) { \
        g_fuzz_failed++; \
        printf("[-] FUZZ FAIL [%s:%d]: %s\n", __func__, __LINE__, msg); \
    } else { \
        g_fuzz_passed++; \
    } \
} while(0)

/* Simple deterministic LCG random generator */
static uint32_t lcg_state = 123456789;
static uint32_t lcg_rand(void) {
    lcg_state = lcg_state * 1664525 + 1013904223;
    return lcg_state;
}

/* ========================================================================= */
/* 1. Protocol Frame Unpacker Fuzzing (10,000 iterations)                    */
/* ========================================================================= */
static void fuzz_protocol_unpack(void) {
    printf("[*] Starting fuzz_protocol_unpack (10,000 iterations)...\n");
    uint8_t random_bytes[1024];
    char out_buf[2048];
    size_t out_len = 0;
    size_t consumed = 0;

    for (int iter = 0; iter < 10000; ++iter) {
        size_t len = (size_t)(lcg_rand() % sizeof(random_bytes));
        for (size_t i = 0; i < len; ++i) {
            random_bytes[i] = (uint8_t)(lcg_rand() & 0xFF);
        }

        /* Call unpacker with fuzz input; must never crash or read OOB */
        int res = protocol_unpack_frame(random_bytes, len, out_buf, sizeof(out_buf), &out_len, &consumed);
        FUZZ_ASSERT(res == PROTO_OK ||
                    res == PROTO_NEED_MORE_DATA ||
                    res == PROTO_ERR_INVALID_FRAME ||
                    res == PROTO_ERR_PAYLOAD_TOO_LARGE ||
                    res == PROTO_ERR_BUFFER_OVERFLOW ||
                    res == PROTO_ERR_INVALID_PARAM,
                    "protocol_unpack_frame returned valid status enum");

        if (res == PROTO_OK) {
            FUZZ_ASSERT(consumed <= len, "Consumed bytes <= input length");
            FUZZ_ASSERT(out_len < sizeof(out_buf), "Extracted payload within output buffer bound");
            FUZZ_ASSERT(out_buf[out_len - 1] == '\n', "Valid frame ends with newline");
            FUZZ_ASSERT(out_buf[out_len] == '\0', "Valid frame is null terminated");
        } else if (res == PROTO_NEED_MORE_DATA) {
            FUZZ_ASSERT(consumed == 0, "Consumed is 0 on NEED_MORE_DATA");
        }
    }
    printf("[+] fuzz_protocol_unpack completed.\n");
}

/* ========================================================================= */
/* 2. Protocol Message Parser Fuzzing (10,000 iterations)                    */
/* ========================================================================= */
static void fuzz_protocol_parse_msg(void) {
    printf("[*] Starting fuzz_protocol_parse_msg (10,000 iterations)...\n");
    char random_str[1024];
    protocol_msg_t msg;

    const char charset[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789\t\n\r -_+.:";
    size_t charset_len = strlen(charset);

    for (int iter = 0; iter < 10000; ++iter) {
        size_t len = (size_t)(lcg_rand() % (sizeof(random_str) - 1));
        for (size_t i = 0; i < len; ++i) {
            random_str[i] = charset[lcg_rand() % charset_len];
        }
        random_str[len] = '\0';

        /* Call parser with fuzz input; must never crash */
        int res = protocol_parse_msg(random_str, len, &msg);
        FUZZ_ASSERT(res == PROTO_OK ||
                    res == PROTO_ERR_INVALID_PARAM ||
                    res == PROTO_ERR_INVALID_FRAME,
                    "protocol_parse_msg returned valid status enum");

        if (res == PROTO_OK) {
            FUZZ_ASSERT(msg.token_count >= 1 && msg.token_count <= PROTO_MAX_TOKENS,
                        "token_count within valid range");
            if (msg.cmd == CMD_MOVE) {
                FUZZ_ASSERT(msg.move_row >= 0 && msg.move_row <= 14, "move_row in range");
                FUZZ_ASSERT(msg.move_col >= 0 && msg.move_col <= 14, "move_col in range");
            }
        }
    }
    printf("[+] fuzz_protocol_parse_msg completed.\n");
}

/* ========================================================================= */
/* 3. Random Caro Games Simulation (1,000 full matches)                     */
/* ========================================================================= */
static void simulate_random_caro_games(void) {
    printf("[*] Starting simulate_random_caro_games (1,000 matches)...\n");

    int x_wins = 0;
    int o_wins = 0;
    int draws = 0;

    for (int game_idx = 0; game_idx < 1000; ++game_idx) {
        game_t g;
        game_init(&g, (uint32_t)game_idx, 10, 11, 100, 200);

        int moves_this_game = 0;
        while (g.status == GAME_STATUS_IN_PROGRESS && moves_this_game < 250) {
            caro_cell_t current = g.current_turn;
            FUZZ_ASSERT(current == CELL_X || current == CELL_O, "current_turn is valid");

            /* Pick random coordinate: mostly [0..14], occasionally out-of-bounds */
            int r, c;
            int r_type = (int)(lcg_rand() % 20);
            if (r_type == 0) {
                r = -1 - (int)(lcg_rand() % 10);
                c = (int)(lcg_rand() % 15);
            } else if (r_type == 1) {
                r = (int)(lcg_rand() % 15);
                c = 15 + (int)(lcg_rand() % 10);
            } else {
                r = (int)(lcg_rand() % 15);
                c = (int)(lcg_rand() % 15);
            }

            bool valid = game_is_valid_move(&g, r, c);
            bool applied = game_make_move(&g, current, r, c);
            FUZZ_ASSERT(valid == applied, "game_is_valid_move matches game_make_move result");

            if (applied) {
                moves_this_game++;
                FUZZ_ASSERT(g.board[r][c] == current, "Cell set to current player");
                FUZZ_ASSERT(g.last_row == r && g.last_col == c, "last_row/col updated");
                FUZZ_ASSERT(g.move_count == (uint32_t)moves_this_game, "move_count matches");
                FUZZ_ASSERT(g.expected_seq == (uint32_t)(moves_this_game + 1), "expected_seq matches");

                if (g.status == GAME_STATUS_FINISHED) {
                    if (g.result == GAME_RESULT_X_WON) {
                        FUZZ_ASSERT(current == CELL_X, "X won on X's turn");
                        FUZZ_ASSERT(game_check_win(&g, r, c), "Winning condition confirmed by check_win");
                        x_wins++;
                    } else if (g.result == GAME_RESULT_O_WON) {
                        FUZZ_ASSERT(current == CELL_O, "O won on O's turn");
                        FUZZ_ASSERT(game_check_win(&g, r, c), "Winning condition confirmed by check_win");
                        o_wins++;
                    } else if (g.result == GAME_RESULT_DRAW) {
                        FUZZ_ASSERT(game_is_board_full(&g), "Board is full on DRAW");
                        FUZZ_ASSERT(g.move_count == 225, "Draw occurs exactly at 225 moves");
                        draws++;
                    }
                }
            } else {
                /* When move is rejected, state must NOT change */
                FUZZ_ASSERT(g.current_turn == current, "Turn unchanged after rejected move");
                FUZZ_ASSERT(g.status == GAME_STATUS_IN_PROGRESS, "Status remains IN_PROGRESS");
            }
        }

        /* Post-game invariant check */
        FUZZ_ASSERT(g.status == GAME_STATUS_FINISHED, "Game completed and reached FINISHED state");
        FUZZ_ASSERT(!game_is_valid_move(&g, 7, 7), "No valid moves once game finished");
        FUZZ_ASSERT(!game_make_move(&g, CELL_X, 7, 7), "X rejected once finished");
        FUZZ_ASSERT(!game_make_move(&g, CELL_O, 7, 7), "O rejected once finished");
    }

    printf("[+] simulate_random_caro_games completed: %d X wins, %d O wins, %d draws.\n",
           x_wins, o_wins, draws);
}

/* ========================================================================= */
/* MAIN RUNNER                                                               */
/* ========================================================================= */
int main(void) {
    printf("===============================================================\n");
    printf("   CHALLENGER 2: FUZZING & INVARIANT STRESS SUITE (G2)\n");
    printf("===============================================================\n");

    fuzz_protocol_unpack();
    fuzz_protocol_parse_msg();
    simulate_random_caro_games();

    printf("===============================================================\n");
    printf("   FUZZ SUMMARY: %d checks passed | %d checks failed\n",
           g_fuzz_passed, g_fuzz_failed);
    printf("===============================================================\n");

    return (g_fuzz_failed == 0) ? 0 : 1;
}
