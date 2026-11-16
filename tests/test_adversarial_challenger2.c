#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <limits.h>
#include "game.h"
#include "protocol.h"

static int g_tests_run = 0;
static int g_tests_passed = 0;
static int g_tests_failed = 0;

#define ADV_ASSERT(cond, msg) do { \
    g_tests_run++; \
    if (!(cond)) { \
        g_tests_failed++; \
        printf("[-] FAIL [%s:%d]: %s\n", __func__, __LINE__, msg); \
    } else { \
        g_tests_passed++; \
        printf("[+] PASS [%s]: %s\n", __func__, msg); \
    } \
} while(0)

/* ========================================================================= */
/* SECTION 1: Extreme Boundary & Out-of-Bounds Stress Testing               */
/* ========================================================================= */
static void stress_board_boundaries_and_extremes(void) {
    game_t g;
    game_init(&g, 100, 10, 11, 1, 2);

    /* Extreme negative values */
    int extreme_negs[] = { -1, -2, -14, -15, -16, -100, -999, -32768, INT_MIN, INT_MIN + 1 };
    size_t num_negs = sizeof(extreme_negs) / sizeof(extreme_negs[0]);
    for (size_t i = 0; i < num_negs; ++i) {
        ADV_ASSERT(!game_is_valid_move(&g, extreme_negs[i], 0), "Reject negative row");
        ADV_ASSERT(!game_is_valid_move(&g, 0, extreme_negs[i]), "Reject negative col");
        ADV_ASSERT(!game_is_valid_move(&g, extreme_negs[i], extreme_negs[i]), "Reject both negative");
        ADV_ASSERT(!game_make_move(&g, CELL_X, extreme_negs[i], 0), "Reject make_move negative row");
        ADV_ASSERT(!game_make_move(&g, CELL_X, 0, extreme_negs[i]), "Reject make_move negative col");
        ADV_ASSERT(game_get_cell(&g, extreme_negs[i], 0) == CELL_EMPTY, "game_get_cell neg row returns CELL_EMPTY");
        ADV_ASSERT(game_get_cell(&g, 0, extreme_negs[i]) == CELL_EMPTY, "game_get_cell neg col returns CELL_EMPTY");
        ADV_ASSERT(!game_check_win(&g, extreme_negs[i], 0), "game_check_win neg row returns false");
    }

    /* Extreme positive values */
    int extreme_pos[] = { 15, 16, 20, 100, 255, 1000, 32767, 65535, INT_MAX, INT_MAX - 1 };
    size_t num_pos = sizeof(extreme_pos) / sizeof(extreme_pos[0]);
    for (size_t i = 0; i < num_pos; ++i) {
        ADV_ASSERT(!game_is_valid_move(&g, extreme_pos[i], 0), "Reject out-of-bounds pos row");
        ADV_ASSERT(!game_is_valid_move(&g, 0, extreme_pos[i]), "Reject out-of-bounds pos col");
        ADV_ASSERT(!game_is_valid_move(&g, extreme_pos[i], extreme_pos[i]), "Reject both pos OOB");
        ADV_ASSERT(!game_make_move(&g, CELL_X, extreme_pos[i], 0), "Reject make_move pos row");
        ADV_ASSERT(!game_make_move(&g, CELL_X, 0, extreme_pos[i]), "Reject make_move pos col");
        ADV_ASSERT(game_get_cell(&g, extreme_pos[i], 0) == CELL_EMPTY, "game_get_cell pos row returns CELL_EMPTY");
        ADV_ASSERT(game_get_cell(&g, 0, extreme_pos[i]) == CELL_EMPTY, "game_get_cell pos col returns CELL_EMPTY");
        ADV_ASSERT(!game_check_win(&g, extreme_pos[i], 0), "game_check_win pos row returns false");
    }

    /* Valid corners */
    ADV_ASSERT(game_is_valid_move(&g, 0, 0), "Valid corner (0, 0)");
    ADV_ASSERT(game_is_valid_move(&g, 0, 14), "Valid corner (0, 14)");
    ADV_ASSERT(game_is_valid_move(&g, 14, 0), "Valid corner (14, 0)");
    ADV_ASSERT(game_is_valid_move(&g, 14, 14), "Valid corner (14, 14)");
}

/* ========================================================================= */
/* SECTION 2: Occupied Cells & Turn State Invariants                         */
/* ========================================================================= */
static void stress_occupied_and_turn_invariants(void) {
    game_t g;
    game_init(&g, 101, 10, 11, 1, 2);

    /* O cannot move first */
    ADV_ASSERT(!game_make_move(&g, CELL_O, 7, 7), "Player O cannot move first");
    ADV_ASSERT(g.board[7][7] == CELL_EMPTY, "Board cell untouched");
    ADV_ASSERT(g.current_turn == CELL_X, "Turn remains CELL_X");

    /* Invalid player enums */
    ADV_ASSERT(!game_make_move(&g, CELL_EMPTY, 7, 7), "CELL_EMPTY cannot make move");
    ADV_ASSERT(!game_make_move(&g, (caro_cell_t)3, 7, 7), "Invalid player enum 3 cannot make move");
    ADV_ASSERT(!game_make_move(&g, (caro_cell_t)-1, 7, 7), "Negative player enum cannot make move");
    ADV_ASSERT(!game_make_move(&g, (caro_cell_t)999, 7, 7), "Player enum 999 cannot make move");

    /* X makes valid move at (7, 7) */
    ADV_ASSERT(game_make_move(&g, CELL_X, 7, 7), "X moves to (7, 7)");
    ADV_ASSERT(g.board[7][7] == CELL_X, "Cell (7, 7) has CELL_X");
    ADV_ASSERT(g.current_turn == CELL_O, "Turn switched to CELL_O");
    ADV_ASSERT(g.expected_seq == 2, "expected_seq is 2");

    /* Try placing on (7, 7) with O */
    ADV_ASSERT(!game_is_valid_move(&g, 7, 7), "Occupied cell (7, 7) is invalid");
    ADV_ASSERT(!game_make_move(&g, CELL_O, 7, 7), "O cannot play on (7, 7)");
    ADV_ASSERT(g.board[7][7] == CELL_X, "Cell (7, 7) still CELL_X");
    ADV_ASSERT(g.current_turn == CELL_O, "Turn remains CELL_O after rejected move");
    ADV_ASSERT(g.expected_seq == 2, "expected_seq remains 2");

    /* Try placing on (7, 7) with X (wrong turn and occupied) */
    ADV_ASSERT(!game_make_move(&g, CELL_X, 7, 7), "X cannot play on (7, 7)");

    /* O plays (7, 8) */
    ADV_ASSERT(game_make_move(&g, CELL_O, 7, 8), "O moves to (7, 8)");
    ADV_ASSERT(g.board[7][8] == CELL_O, "Cell (7, 8) has CELL_O");
    ADV_ASSERT(g.current_turn == CELL_X, "Turn switched to CELL_X");
    ADV_ASSERT(g.expected_seq == 3, "expected_seq is 3");

    /* X tries playing on (7, 8) (occupied by O) */
    ADV_ASSERT(!game_make_move(&g, CELL_X, 7, 8), "X cannot play on (7, 8) occupied by O");
    ADV_ASSERT(g.board[7][8] == CELL_O, "Cell (7, 8) remains CELL_O");
}

/* ========================================================================= */
/* SECTION 3: Win Logic: 4 Directions, Overline (6, 7, 8, 15), Blocked Ends  */
/* ========================================================================= */
static void stress_win_logic_and_overlines(void) {
    /* 1. Horizontal: 4 pieces (no win), 5 pieces (win), 6 pieces (win), 7 pieces (win) */
    {
        game_t g;
        game_init(&g, 201, 10, 11, 1, 2);
        for (int c = 0; c < 4; ++c) {
            g.board[2][c] = CELL_X;
        }
        ADV_ASSERT(!game_check_win(&g, 2, 0), "Horizontal 4 pieces at (2,0) is not win");
        ADV_ASSERT(!game_check_win(&g, 2, 3), "Horizontal 4 pieces at (2,3) is not win");

        /* 5 pieces */
        g.board[2][4] = CELL_X;
        ADV_ASSERT(game_check_win(&g, 2, 0), "Horizontal 5 pieces head is win");
        ADV_ASSERT(game_check_win(&g, 2, 2), "Horizontal 5 pieces center is win");
        ADV_ASSERT(game_check_win(&g, 2, 4), "Horizontal 5 pieces tail is win");

        /* 6 pieces (overline) */
        g.board[2][5] = CELL_X;
        ADV_ASSERT(game_check_win(&g, 2, 0), "Horizontal 6 pieces head is win");
        ADV_ASSERT(game_check_win(&g, 2, 3), "Horizontal 6 pieces center is win");
        ADV_ASSERT(game_check_win(&g, 2, 5), "Horizontal 6 pieces tail is win");

        /* 7 pieces (long overline) */
        g.board[2][6] = CELL_X;
        ADV_ASSERT(game_check_win(&g, 2, 0), "Horizontal 7 pieces head is win");
        ADV_ASSERT(game_check_win(&g, 2, 4), "Horizontal 7 pieces center is win");
        ADV_ASSERT(game_check_win(&g, 2, 6), "Horizontal 7 pieces tail is win");

        /* 8 pieces */
        g.board[2][7] = CELL_X;
        ADV_ASSERT(game_check_win(&g, 2, 7), "Horizontal 8 pieces tail is win");

        /* Full line of 15 pieces */
        for (int c = 8; c < 15; ++c) {
            g.board[2][c] = CELL_X;
        }
        ADV_ASSERT(game_check_win(&g, 2, 0), "Horizontal 15 pieces head is win");
        ADV_ASSERT(game_check_win(&g, 2, 7), "Horizontal 15 pieces middle is win");
        ADV_ASSERT(game_check_win(&g, 2, 14), "Horizontal 15 pieces end is win");
    }

    /* 2. Vertical: 4 pieces (no win), 5 pieces (win), 6 pieces (win), 7 pieces (win) */
    {
        game_t g;
        game_init(&g, 202, 10, 11, 1, 2);
        for (int r = 0; r < 4; ++r) {
            g.board[r][5] = CELL_O;
        }
        ADV_ASSERT(!game_check_win(&g, 0, 5), "Vertical 4 pieces is not win");

        /* 5 pieces */
        g.board[4][5] = CELL_O;
        ADV_ASSERT(game_check_win(&g, 0, 5), "Vertical 5 pieces head is win");
        ADV_ASSERT(game_check_win(&g, 2, 5), "Vertical 5 pieces center is win");
        ADV_ASSERT(game_check_win(&g, 4, 5), "Vertical 5 pieces tail is win");

        /* 6 pieces */
        g.board[5][5] = CELL_O;
        ADV_ASSERT(game_check_win(&g, 0, 5), "Vertical 6 pieces head is win");
        ADV_ASSERT(game_check_win(&g, 5, 5), "Vertical 6 pieces tail is win");

        /* 7 pieces */
        g.board[6][5] = CELL_O;
        ADV_ASSERT(game_check_win(&g, 6, 5), "Vertical 7 pieces tail is win");
    }

    /* 3. Main Diagonal: 4 pieces (no win), 5 pieces (win), 6 pieces (win), 7 pieces (win) */
    {
        game_t g;
        game_init(&g, 203, 10, 11, 1, 2);
        for (int i = 0; i < 4; ++i) {
            g.board[1 + i][1 + i] = CELL_X;
        }
        ADV_ASSERT(!game_check_win(&g, 1, 1), "Main diagonal 4 pieces is not win");

        /* 5 pieces */
        g.board[5][5] = CELL_X;
        ADV_ASSERT(game_check_win(&g, 1, 1), "Main diagonal 5 pieces head is win");
        ADV_ASSERT(game_check_win(&g, 3, 3), "Main diagonal 5 pieces center is win");
        ADV_ASSERT(game_check_win(&g, 5, 5), "Main diagonal 5 pieces tail is win");

        /* 6 pieces */
        g.board[6][6] = CELL_X;
        ADV_ASSERT(game_check_win(&g, 6, 6), "Main diagonal 6 pieces tail is win");

        /* 7 pieces */
        g.board[7][7] = CELL_X;
        ADV_ASSERT(game_check_win(&g, 4, 4), "Main diagonal 7 pieces center is win");
    }

    /* 4. Anti-Diagonal: 4 pieces (no win), 5 pieces (win), 6 pieces (win), 7 pieces (win) */
    {
        game_t g;
        game_init(&g, 204, 10, 11, 1, 2);
        for (int i = 0; i < 4; ++i) {
            g.board[10 - i][2 + i] = CELL_O;
        }
        ADV_ASSERT(!game_check_win(&g, 10, 2), "Anti-diagonal 4 pieces is not win");

        /* 5 pieces */
        g.board[6][6] = CELL_O;
        ADV_ASSERT(game_check_win(&g, 10, 2), "Anti-diagonal 5 pieces head is win");
        ADV_ASSERT(game_check_win(&g, 8, 4), "Anti-diagonal 5 pieces center is win");
        ADV_ASSERT(game_check_win(&g, 6, 6), "Anti-diagonal 5 pieces tail is win");

        /* 6 pieces */
        g.board[5][7] = CELL_O;
        ADV_ASSERT(game_check_win(&g, 5, 7), "Anti-diagonal 6 pieces tail is win");

        /* 7 pieces */
        g.board[4][8] = CELL_O;
        ADV_ASSERT(game_check_win(&g, 4, 8), "Anti-diagonal 7 pieces tail is win");
    }

    /* 5. Overline blocked at both ends */
    {
        /* Horizontal blocked at both ends: O X X X X X X O */
        game_t g;
        game_init(&g, 205, 10, 11, 1, 2);
        g.board[6][1] = CELL_O; /* Left blocker */
        for (int c = 2; c <= 7; ++c) {
            g.board[6][c] = CELL_X; /* 6 consecutive X */
        }
        g.board[6][8] = CELL_O; /* Right blocker */
        ADV_ASSERT(game_check_win(&g, 6, 2), "Horizontal 6 pieces blocked at both ends is WIN");
        ADV_ASSERT(game_check_win(&g, 6, 5), "Horizontal 6 pieces center blocked at both ends is WIN");
        ADV_ASSERT(game_check_win(&g, 6, 7), "Horizontal 6 pieces tail blocked at both ends is WIN");

        /* Vertical blocked at both ends: O X X X X X X O */
        game_reset_board(&g);
        g.board[2][5] = CELL_O; /* Top blocker */
        for (int r = 3; r <= 8; ++r) {
            g.board[r][5] = CELL_X; /* 6 consecutive X */
        }
        g.board[9][5] = CELL_O; /* Bottom blocker */
        ADV_ASSERT(game_check_win(&g, 3, 5), "Vertical 6 pieces blocked at both ends is WIN");
        ADV_ASSERT(game_check_win(&g, 6, 5), "Vertical 6 pieces center blocked at both ends is WIN");
        ADV_ASSERT(game_check_win(&g, 8, 5), "Vertical 6 pieces tail blocked at both ends is WIN");

        /* Main diagonal blocked at both ends */
        game_reset_board(&g);
        g.board[1][1] = CELL_O;
        for (int i = 2; i <= 7; ++i) {
            g.board[i][i] = CELL_X;
        }
        g.board[8][8] = CELL_O;
        ADV_ASSERT(game_check_win(&g, 2, 2), "Main diagonal 6 pieces blocked at both ends is WIN");
        ADV_ASSERT(game_check_win(&g, 5, 5), "Main diagonal 6 pieces center blocked at both ends is WIN");
        ADV_ASSERT(game_check_win(&g, 7, 7), "Main diagonal 6 pieces tail blocked at both ends is WIN");

        /* Anti-diagonal blocked at both ends */
        game_reset_board(&g);
        g.board[9][1] = CELL_X; /* blocker */
        for (int i = 0; i < 6; ++i) {
            g.board[8 - i][2 + i] = CELL_O;
        }
        g.board[2][8] = CELL_X; /* blocker */
        ADV_ASSERT(game_check_win(&g, 8, 2), "Anti-diagonal 6 pieces blocked at both ends is WIN");
        ADV_ASSERT(game_check_win(&g, 5, 5), "Anti-diagonal 6 pieces center blocked at both ends is WIN");
        ADV_ASSERT(game_check_win(&g, 3, 7), "Anti-diagonal 6 pieces tail blocked at both ends is WIN");
    }
}

/* ========================================================================= */
/* SECTION 4: Draw Logic & Win Priority on 225th Move                        */
/* ========================================================================= */
static void stress_draw_and_225th_move_priority(void) {
    /* Test 4a: Move 225 completes a winning line -> Must be WIN, not DRAW! */
    {
        game_t g;
        game_init(&g, 301, 10, 11, 1, 2);

        /* Fill 224 cells manually such that:
         * Cell (14, 14) is empty and is the 225th cell.
         * Row 14, cols 10, 11, 12, 13 are CELL_X.
         * So when CELL_X plays (14, 14), it completes 5 in a row horizontally!
         */
        for (int r = 0; r < 14; ++r) {
            for (int c = 0; c < 15; ++c) {
                /* Checkerboard to avoid premature win */
                g.board[r][c] = ((r + c) % 2 == 0) ? CELL_X : CELL_O;
            }
        }
        for (int c = 0; c < 10; ++c) {
            g.board[14][c] = (c % 2 == 0) ? CELL_X : CELL_O;
        }
        /* Put 4 X's at cols 10, 11, 12, 13 */
        g.board[14][10] = CELL_X;
        g.board[14][11] = CELL_X;
        g.board[14][12] = CELL_X;
        g.board[14][13] = CELL_X;
        g.board[14][14] = CELL_EMPTY; /* 225th cell */

        g.move_count = 224;
        g.current_turn = CELL_X;
        g.status = GAME_STATUS_IN_PROGRESS;

        ADV_ASSERT(!game_is_board_full(&g), "Board not full at move 224");

        /* Move 225 by X at (14, 14) */
        bool move_ok = game_make_move(&g, CELL_X, 14, 14);
        ADV_ASSERT(move_ok, "225th move successfully placed");
        ADV_ASSERT(g.status == GAME_STATUS_FINISHED, "Game is FINISHED");
        ADV_ASSERT(g.result == GAME_RESULT_X_WON, "225th move resulting in 5-in-a-row is X_WON (WIN > DRAW)");
        ADV_ASSERT(g.result != GAME_RESULT_DRAW, "Result is NOT DRAW when winning line formed on 225th cell");
    }

    /* Test 4b: Post-finish moves strictly rejected */
    {
        game_t g;
        game_init(&g, 302, 10, 11, 1, 2);
        g.status = GAME_STATUS_FINISHED;
        g.result = GAME_RESULT_DRAW;

        ADV_ASSERT(!game_is_valid_move(&g, 0, 0), "Post-draw valid_move rejected");
        ADV_ASSERT(!game_make_move(&g, CELL_X, 0, 0), "Post-draw make_move X rejected");
        ADV_ASSERT(!game_make_move(&g, CELL_O, 0, 0), "Post-draw make_move O rejected");

        g.status = GAME_STATUS_ABORTED;
        ADV_ASSERT(!game_is_valid_move(&g, 0, 0), "Aborted valid_move rejected");
        ADV_ASSERT(!game_make_move(&g, CELL_X, 0, 0), "Aborted make_move X rejected");

        g.status = GAME_STATUS_WAITING;
        ADV_ASSERT(!game_is_valid_move(&g, 0, 0), "Waiting valid_move rejected");
        ADV_ASSERT(!game_make_move(&g, CELL_X, 0, 0), "Waiting make_move X rejected");
    }
}

/* ========================================================================= */
/* SECTION 5: Parser Edge Cases & Extreme Number Parsing                     */
/* ========================================================================= */
static void stress_protocol_parser(void) {
    protocol_msg_t msg;

    /* 1. Missing tokens across all commands */
    ADV_ASSERT(protocol_parse_msg("MOVE\n", 0, &msg) == PROTO_ERR_INVALID_PARAM, "MOVE 0 args rejected");
    ADV_ASSERT(protocol_parse_msg("MOVE\t1\n", 0, &msg) == PROTO_ERR_INVALID_PARAM, "MOVE 1 arg rejected");
    ADV_ASSERT(protocol_parse_msg("MOVE\t1\t2\n", 0, &msg) == PROTO_ERR_INVALID_PARAM, "MOVE 2 args rejected");
    ADV_ASSERT(protocol_parse_msg("MOVE\t1\t2\t3\n", 0, &msg) == PROTO_ERR_INVALID_PARAM, "MOVE 3 args rejected");
    ADV_ASSERT(protocol_parse_msg("MOVE_ACK\t1\t2\t3\n", 0, &msg) == PROTO_ERR_INVALID_PARAM, "MOVE_ACK 3 args rejected");
    ADV_ASSERT(protocol_parse_msg("MOVE_BROADCAST\t1\t2\t3\n", 0, &msg) == PROTO_ERR_INVALID_PARAM, "MOVE_BROADCAST 3 args rejected");
    ADV_ASSERT(protocol_parse_msg("LOGIN\n", 0, &msg) == PROTO_ERR_INVALID_PARAM, "LOGIN 0 args rejected");
    ADV_ASSERT(protocol_parse_msg("LOGIN\t1\n", 0, &msg) == PROTO_ERR_INVALID_PARAM, "LOGIN 1 arg rejected");
    ADV_ASSERT(protocol_parse_msg("LOGIN\t1\talice\n", 0, &msg) == PROTO_ERR_INVALID_PARAM, "LOGIN 2 args rejected");
    ADV_ASSERT(protocol_parse_msg("REGISTER\n", 0, &msg) == PROTO_ERR_INVALID_PARAM, "REGISTER 0 args rejected");
    ADV_ASSERT(protocol_parse_msg("REGISTER\t1\n", 0, &msg) == PROTO_ERR_INVALID_PARAM, "REGISTER 1 arg rejected");
    ADV_ASSERT(protocol_parse_msg("REGISTER\t1\talice\n", 0, &msg) == PROTO_ERR_INVALID_PARAM, "REGISTER 2 args rejected");
    ADV_ASSERT(protocol_parse_msg("CHALLENGE\n", 0, &msg) == PROTO_ERR_INVALID_PARAM, "CHALLENGE 0 args rejected");
    ADV_ASSERT(protocol_parse_msg("CHALLENGE\t1\n", 0, &msg) == PROTO_ERR_INVALID_PARAM, "CHALLENGE 1 arg rejected");
    ADV_ASSERT(protocol_parse_msg("ERR\n", 0, &msg) == PROTO_ERR_INVALID_PARAM, "ERR 0 args rejected");
    ADV_ASSERT(protocol_parse_msg("ERR\t100\n", 0, &msg) == PROTO_ERR_INVALID_PARAM, "ERR 1 arg rejected");

    /* Empty and whitespace inputs */
    ADV_ASSERT(protocol_parse_msg("", 0, &msg) == PROTO_ERR_INVALID_FRAME, "Empty payload rejected");
    ADV_ASSERT(protocol_parse_msg("\n", 0, &msg) == PROTO_ERR_INVALID_FRAME, "Newline only rejected");
    ADV_ASSERT(protocol_parse_msg("\r\n", 0, &msg) == PROTO_ERR_INVALID_FRAME, "CRLF only rejected");
    ADV_ASSERT(protocol_parse_msg("\r", 0, &msg) == PROTO_ERR_INVALID_FRAME, "CR only rejected");

    /* 2. Numeric overflow & extreme numbers */
    /* 2^32 = 4294967296 (1 over UINT32_MAX) */
    ADV_ASSERT(protocol_parse_msg("MOVE\t4294967296\t1\t0\t0\n", 0, &msg) == PROTO_ERR_INVALID_PARAM, "game_id 2^32 rejected");
    ADV_ASSERT(protocol_parse_msg("MOVE\t1\t4294967296\t0\t0\n", 0, &msg) == PROTO_ERR_INVALID_PARAM, "seq 2^32 rejected");
    ADV_ASSERT(protocol_parse_msg("MOVE\t1\t1\t4294967296\t0\n", 0, &msg) == PROTO_ERR_INVALID_PARAM, "row 2^32 rejected");
    ADV_ASSERT(protocol_parse_msg("MOVE\t1\t1\t0\t4294967296\n", 0, &msg) == PROTO_ERR_INVALID_PARAM, "col 2^32 rejected");
    ADV_ASSERT(protocol_parse_msg("LOGIN\t4294967296\tu\tp\n", 0, &msg) == PROTO_ERR_INVALID_PARAM, "LOGIN req_id 2^32 rejected");
    ADV_ASSERT(protocol_parse_msg("REGISTER\t4294967296\tu\tp\n", 0, &msg) == PROTO_ERR_INVALID_PARAM, "REGISTER req_id 2^32 rejected");
    ADV_ASSERT(protocol_parse_msg("CHALLENGE\t4294967296\tu\n", 0, &msg) == PROTO_ERR_INVALID_PARAM, "CHALLENGE req_id 2^32 rejected");

    /* 64-bit max / extreme overflow */
    ADV_ASSERT(protocol_parse_msg("MOVE\t18446744073709551615\t1\t0\t0\n", 0, &msg) == PROTO_ERR_INVALID_PARAM, "ULLONG_MAX rejected");
    ADV_ASSERT(protocol_parse_msg("MOVE\t18446744073709551616\t1\t0\t0\n", 0, &msg) == PROTO_ERR_INVALID_PARAM, "ULLONG_MAX + 1 rejected");
    ADV_ASSERT(protocol_parse_msg("MOVE\t9999999999999999999999999999999999999999\t1\t0\t0\n", 0, &msg) == PROTO_ERR_INVALID_PARAM, "Massive digit string rejected");

    /* 3. Negative numbers */
    ADV_ASSERT(protocol_parse_msg("MOVE\t-1\t1\t0\t0\n", 0, &msg) == PROTO_ERR_INVALID_PARAM, "game_id -1 rejected");
    ADV_ASSERT(protocol_parse_msg("MOVE\t1\t-1\t0\t0\n", 0, &msg) == PROTO_ERR_INVALID_PARAM, "seq -1 rejected");
    ADV_ASSERT(protocol_parse_msg("MOVE\t1\t1\t-1\t0\n", 0, &msg) == PROTO_ERR_INVALID_PARAM, "row -1 rejected");
    ADV_ASSERT(protocol_parse_msg("MOVE\t1\t1\t0\t-1\n", 0, &msg) == PROTO_ERR_INVALID_PARAM, "col -1 rejected");
    ADV_ASSERT(protocol_parse_msg("MOVE\t-2147483648\t1\t0\t0\n", 0, &msg) == PROTO_ERR_INVALID_PARAM, "INT_MIN game_id rejected");
    ADV_ASSERT(protocol_parse_msg("LOGIN\t-1\tu\tp\n", 0, &msg) == PROTO_ERR_INVALID_PARAM, "LOGIN req_id -1 rejected");
    ADV_ASSERT(protocol_parse_msg("ERR\t-402\tMsg\n", 0, &msg) == PROTO_ERR_INVALID_PARAM, "ERR code -402 rejected");
    ADV_ASSERT(protocol_parse_msg("ERR\t-1\t402\tMsg\n", 0, &msg) == PROTO_ERR_INVALID_PARAM, "ERR req_id -1 rejected");

    /* 4. Non-digits, hex, floating point, signs */
    ADV_ASSERT(protocol_parse_msg("MOVE\t+1\t1\t0\t0\n", 0, &msg) == PROTO_ERR_INVALID_PARAM, "Leading plus rejected");
    ADV_ASSERT(protocol_parse_msg("MOVE\t 1\t1\t0\t0\n", 0, &msg) == PROTO_ERR_INVALID_PARAM, "Leading space rejected");
    ADV_ASSERT(protocol_parse_msg("MOVE\t1 \t1\t0\t0\n", 0, &msg) == PROTO_ERR_INVALID_PARAM, "Trailing space rejected");
    ADV_ASSERT(protocol_parse_msg("MOVE\t0x10\t1\t0\t0\n", 0, &msg) == PROTO_ERR_INVALID_PARAM, "Hex 0x10 rejected");
    ADV_ASSERT(protocol_parse_msg("MOVE\t1.5\t1\t0\t0\n", 0, &msg) == PROTO_ERR_INVALID_PARAM, "Float 1.5 rejected");
    ADV_ASSERT(protocol_parse_msg("MOVE\tNaN\t1\t0\t0\n", 0, &msg) == PROTO_ERR_INVALID_PARAM, "NaN rejected");
    ADV_ASSERT(protocol_parse_msg("MOVE\tInf\t1\t0\t0\n", 0, &msg) == PROTO_ERR_INVALID_PARAM, "Inf rejected");

    /* 5. Valid exact boundary UINT32_MAX */
    ADV_ASSERT(protocol_parse_msg("MOVE\t4294967295\t4294967295\t0\t0\n", 0, &msg) == PROTO_OK, "UINT32_MAX valid parsed");
    ADV_ASSERT(msg.game_id == 4294967295U, "game_id equals UINT32_MAX");
    ADV_ASSERT(msg.seq == 4294967295U, "seq equals UINT32_MAX");

    /* 6. Board coordinate limits in parser */
    ADV_ASSERT(protocol_parse_msg("MOVE\t1\t1\t14\t14\n", 0, &msg) == PROTO_OK, "Row 14, Col 14 valid");
    ADV_ASSERT(msg.move_row == 14 && msg.move_col == 14, "Parsed row=14, col=14");
    ADV_ASSERT(protocol_parse_msg("MOVE\t1\t1\t15\t0\n", 0, &msg) == PROTO_ERR_INVALID_PARAM, "Row 15 rejected in parser");
    ADV_ASSERT(protocol_parse_msg("MOVE\t1\t1\t0\t15\n", 0, &msg) == PROTO_ERR_INVALID_PARAM, "Col 15 rejected in parser");
    ADV_ASSERT(protocol_parse_msg("MOVE\t1\t1\t100\t14\n", 0, &msg) == PROTO_ERR_INVALID_PARAM, "Row 100 rejected in parser");

    /* 7. ERR command variations */
    ADV_ASSERT(protocol_parse_msg("ERR\t402\tInvalid move\n", 0, &msg) == PROTO_OK, "3-token ERR succeeds");
    ADV_ASSERT(msg.request_id == 0, "3-token ERR has req_id 0");
    ADV_ASSERT(protocol_parse_msg("ERR\t1001\t402\tInvalid move\n", 0, &msg) == PROTO_OK, "4-token ERR succeeds");
    ADV_ASSERT(msg.request_id == 1001, "4-token ERR has req_id 1001");
}

/* ========================================================================= */
/* SECTION 6: Binary Frame Framing Edge Cases                                */
/* ========================================================================= */
static void stress_binary_framing(void) {
    uint8_t wire[8192];
    char out_buf[8192];
    size_t out_len = 0;
    size_t consumed = 0;

    /* 1. Header with length = 0 */
    uint32_t zero_len = htonl(0);
    memcpy(wire, &zero_len, 4);
    int res = protocol_unpack_frame(wire, 4, out_buf, sizeof(out_buf), &out_len, &consumed);
    ADV_ASSERT(res == PROTO_ERR_INVALID_FRAME, "0-byte payload header returns PROTO_ERR_INVALID_FRAME");

    /* 2. Header with length = 4096 (max allowed payload) */
    uint32_t max_len = htonl(PROTO_MAX_PAYLOAD_SIZE);
    memcpy(wire, &max_len, 4);
    res = protocol_unpack_frame(wire, 4, out_buf, sizeof(out_buf), &out_len, &consumed);
    ADV_ASSERT(res == PROTO_NEED_MORE_DATA, "4096-byte header returns PROTO_NEED_MORE_DATA when body incomplete");

    /* 3. Header with length = 4097 (exceeds max payload) */
    uint32_t over_len = htonl(PROTO_MAX_PAYLOAD_SIZE + 1);
    memcpy(wire, &over_len, 4);
    res = protocol_unpack_frame(wire, 4, out_buf, sizeof(out_buf), &out_len, &consumed);
    ADV_ASSERT(res == PROTO_ERR_PAYLOAD_TOO_LARGE, "4097-byte header returns PROTO_ERR_PAYLOAD_TOO_LARGE");

    /* 4. Extreme payload headers */
    uint32_t huge_lens[] = { 65536, 1000000, 0x7FFFFFFF, 0xFFFFFFFF };
    for (int i = 0; i < 4; ++i) {
        uint32_t h = htonl(huge_lens[i]);
        memcpy(wire, &h, 4);
        res = protocol_unpack_frame(wire, 4, out_buf, sizeof(out_buf), &out_len, &consumed);
        ADV_ASSERT(res == PROTO_ERR_PAYLOAD_TOO_LARGE, "Gigantic header length rejected immediately");
    }

    /* 5. Incomplete headers: 0, 1, 2, 3 bytes */
    for (size_t l = 0; l < 4; ++l) {
        res = protocol_unpack_frame(wire, l, out_buf, sizeof(out_buf), &out_len, &consumed);
        ADV_ASSERT(res == PROTO_NEED_MORE_DATA, "Header length < 4 returns PROTO_NEED_MORE_DATA");
        ADV_ASSERT(consumed == 0, "0 bytes consumed for partial header");
    }

    /* 6. Missing newline in payload */
    uint32_t valid_len = htonl(5);
    memcpy(wire, &valid_len, 4);
    memcpy(wire + 4, "HELLO", 5); /* No newline */
    res = protocol_unpack_frame(wire, 9, out_buf, sizeof(out_buf), &out_len, &consumed);
    ADV_ASSERT(res == PROTO_ERR_INVALID_FRAME, "Missing trailing newline returns PROTO_ERR_INVALID_FRAME");

    /* 7. Output buffer too small for payload */
    uint32_t normal_len = htonl(10);
    memcpy(wire, &normal_len, 4);
    memcpy(wire + 4, "123456789\n", 10);
    res = protocol_unpack_frame(wire, 14, out_buf, 5, &out_len, &consumed);
    ADV_ASSERT(res == PROTO_ERR_BUFFER_OVERFLOW, "Small out buffer returns PROTO_ERR_BUFFER_OVERFLOW");

    /* 8. NULL pointer protection */
    ADV_ASSERT(protocol_unpack_frame(NULL, 10, out_buf, sizeof(out_buf), &out_len, &consumed) == PROTO_ERR_INVALID_PARAM,
               "protocol_unpack_frame(NULL in_buf) safe");
    ADV_ASSERT(protocol_unpack_frame(wire, 10, NULL, sizeof(out_buf), &out_len, &consumed) == PROTO_ERR_INVALID_PARAM,
               "protocol_unpack_frame(NULL out_buf) safe");
    ADV_ASSERT(protocol_unpack_frame(wire, 10, out_buf, sizeof(out_buf), NULL, &consumed) == PROTO_ERR_INVALID_PARAM,
               "protocol_unpack_frame(NULL out_len) safe");
    ADV_ASSERT(protocol_unpack_frame(wire, 10, out_buf, sizeof(out_buf), &out_len, NULL) == PROTO_ERR_INVALID_PARAM,
               "protocol_unpack_frame(NULL consumed) safe");
}

/* ========================================================================= */
/* MAIN RUNNER                                                               */
/* ========================================================================= */
int main(void) {
    printf("===============================================================\n");
    printf("   CHALLENGER 2: ADVERSARIAL STRESS TEST SUITE (G2)\n");
    printf("===============================================================\n");

    stress_board_boundaries_and_extremes();
    stress_occupied_and_turn_invariants();
    stress_win_logic_and_overlines();
    stress_draw_and_225th_move_priority();
    stress_protocol_parser();
    stress_binary_framing();

    printf("===============================================================\n");
    printf("   SUMMARY: %d tests run | %d passed | %d failed\n",
           g_tests_run, g_tests_passed, g_tests_failed);
    printf("===============================================================\n");

    return (g_tests_failed == 0) ? 0 : 1;
}
