#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "game.h"

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
/* Test 1: Game Initialization, Board Reset, and NULL Safety                 */
/* ========================================================================= */
static void test_game_init_and_null_safety(void) {
    game_t g;
    memset(&g, 0xFF, sizeof(g));

    game_init(&g, 42, 10, 11, 1001, 1002);

    TEST_ASSERT(g.game_id == 42, "game_id initialized correctly");
    TEST_ASSERT(g.status == GAME_STATUS_IN_PROGRESS, "status initialized to IN_PROGRESS");
    TEST_ASSERT(g.result == GAME_RESULT_NONE, "result initialized to NONE");
    TEST_ASSERT(g.player_x_fd == 10, "player_x_fd initialized");
    TEST_ASSERT(g.player_o_fd == 11, "player_o_fd initialized");
    TEST_ASSERT(g.player_x_id == 1001, "player_x_id initialized");
    TEST_ASSERT(g.player_o_id == 1002, "player_o_id initialized");
    TEST_ASSERT(g.current_turn == CELL_X, "First turn is CELL_X");
    TEST_ASSERT(g.move_count == 0, "move_count starts at 0");
    TEST_ASSERT(g.expected_seq == 1, "expected_seq starts at 1");
    TEST_ASSERT(g.last_row == -1, "last_row starts at -1");
    TEST_ASSERT(g.last_col == -1, "last_col starts at -1");

    /* Verify all 225 cells are empty */
    bool all_empty = true;
    for (int r = 0; r < CARO_BOARD_SIZE; ++r) {
        for (int c = 0; c < CARO_BOARD_SIZE; ++c) {
            if (g.board[r][c] != CELL_EMPTY || game_get_cell(&g, r, c) != CELL_EMPTY) {
                all_empty = false;
            }
        }
    }
    TEST_ASSERT(all_empty, "All 225 cells initialized to CELL_EMPTY");

    /* NULL Pointer Safety */
    game_init(NULL, 1, 0, 0, 0, 0);
    game_reset_board(NULL);
    TEST_ASSERT(!game_is_valid_move(NULL, 0, 0), "game_is_valid_move(NULL) returns false");
    TEST_ASSERT(game_get_cell(NULL, 0, 0) == CELL_EMPTY, "game_get_cell(NULL) returns CELL_EMPTY");
    TEST_ASSERT(!game_check_win(NULL, 0, 0), "game_check_win(NULL) returns false");
    TEST_ASSERT(!game_is_board_full(NULL), "game_is_board_full(NULL) returns false");
    TEST_ASSERT(!game_make_move(NULL, CELL_X, 0, 0), "game_make_move(NULL) returns false");
}

/* ========================================================================= */
/* Test 2: Board Boundaries and Coordinate Validation                        */
/* ========================================================================= */
static void test_board_boundaries(void) {
    game_t g;
    game_init(&g, 1, 10, 11, 100, 200);

    /* Valid 0-indexed coordinates [0..14] */
    TEST_ASSERT(game_is_valid_move(&g, 0, 0), "Corner (0, 0) is valid");
    TEST_ASSERT(game_is_valid_move(&g, 0, 14), "Corner (0, 14) is valid");
    TEST_ASSERT(game_is_valid_move(&g, 14, 0), "Corner (14, 0) is valid");
    TEST_ASSERT(game_is_valid_move(&g, 14, 14), "Corner (14, 14) is valid");
    TEST_ASSERT(game_is_valid_move(&g, 7, 7), "Center (7, 7) is valid");

    /* Out of bounds: negative coordinates */
    TEST_ASSERT(!game_is_valid_move(&g, -1, 0), "Reject row = -1");
    TEST_ASSERT(!game_is_valid_move(&g, -10, 0), "Reject row = -10");
    TEST_ASSERT(!game_is_valid_move(&g, 0, -1), "Reject col = -1");
    TEST_ASSERT(!game_is_valid_move(&g, 0, -10), "Reject col = -10");
    TEST_ASSERT(!game_is_valid_move(&g, -1, -1), "Reject both row and col = -1");

    /* Out of bounds: coordinates >= 15 */
    TEST_ASSERT(!game_is_valid_move(&g, 15, 0), "Reject row = 15");
    TEST_ASSERT(!game_is_valid_move(&g, 100, 0), "Reject row = 100");
    TEST_ASSERT(!game_is_valid_move(&g, 0, 15), "Reject col = 15");
    TEST_ASSERT(!game_is_valid_move(&g, 0, 100), "Reject col = 100");
    TEST_ASSERT(!game_is_valid_move(&g, 15, 15), "Reject both row and col = 15");
    TEST_ASSERT(!game_is_valid_move(&g, 999, 999), "Reject large out of bounds");

    /* game_get_cell out-of-bounds behavior */
    TEST_ASSERT(game_get_cell(&g, -1, 0) == CELL_EMPTY, "game_get_cell(-1, 0) returns CELL_EMPTY");
    TEST_ASSERT(game_get_cell(&g, 0, 15) == CELL_EMPTY, "game_get_cell(0, 15) returns CELL_EMPTY");
    TEST_ASSERT(game_get_cell(&g, 15, 15) == CELL_EMPTY, "game_get_cell(15, 15) returns CELL_EMPTY");
    TEST_ASSERT(game_get_cell(&g, -5, -5) == CELL_EMPTY, "game_get_cell(-5, -5) returns CELL_EMPTY");

    /* game_check_win out-of-bounds safety */
    TEST_ASSERT(!game_check_win(&g, -1, 0), "game_check_win(-1, 0) returns false");
    TEST_ASSERT(!game_check_win(&g, 0, 15), "game_check_win(0, 15) returns false");
    TEST_ASSERT(!game_check_win(&g, 15, 15), "game_check_win(15, 15) returns false");
}

/* ========================================================================= */
/* Test 3: Move Placement, Turn Enforcement, and Occupied Cell Rejection     */
/* ========================================================================= */
static void test_move_placement_and_occupied_cells(void) {
    game_t g;
    game_init(&g, 2, 10, 11, 101, 102);

    /* O cannot move when current_turn is X */
    TEST_ASSERT(!game_make_move(&g, CELL_O, 5, 5), "CELL_O cannot move on CELL_X turn");
    TEST_ASSERT(g.board[5][5] == CELL_EMPTY, "Cell (5, 5) remains empty after invalid turn move");
    TEST_ASSERT(g.current_turn == CELL_X, "Turn remains CELL_X");
    TEST_ASSERT(g.expected_seq == 1, "expected_seq remains 1");

    /* Valid move by X */
    bool ok = game_make_move(&g, CELL_X, 5, 5);
    TEST_ASSERT(ok, "CELL_X valid move succeeds");
    TEST_ASSERT(game_get_cell(&g, 5, 5) == CELL_X, "Cell (5, 5) contains CELL_X");
    TEST_ASSERT(g.last_row == 5, "last_row updated to 5");
    TEST_ASSERT(g.last_col == 5, "last_col updated to 5");
    TEST_ASSERT(g.move_count == 1, "move_count incremented to 1");
    TEST_ASSERT(g.expected_seq == 2, "expected_seq incremented to 2");
    TEST_ASSERT(g.current_turn == CELL_O, "current_turn switched to CELL_O");

    /* Occupied cell rejection */
    TEST_ASSERT(!game_is_valid_move(&g, 5, 5), "Occupied cell (5, 5) is invalid for new move");
    TEST_ASSERT(!game_make_move(&g, CELL_O, 5, 5), "CELL_O cannot play on occupied cell (5, 5)");
    TEST_ASSERT(g.current_turn == CELL_O, "Turn remains CELL_O after rejected occupied move");
    TEST_ASSERT(g.move_count == 1, "move_count untouched after rejected move");
    TEST_ASSERT(g.expected_seq == 2, "expected_seq untouched after rejected move");

    /* Valid move by O */
    ok = game_make_move(&g, CELL_O, 5, 6);
    TEST_ASSERT(ok, "CELL_O valid move succeeds");
    TEST_ASSERT(game_get_cell(&g, 5, 6) == CELL_O, "Cell (5, 6) contains CELL_O");
    TEST_ASSERT(g.current_turn == CELL_X, "current_turn switched back to CELL_X");
    TEST_ASSERT(g.move_count == 2, "move_count incremented to 2");
    TEST_ASSERT(g.expected_seq == 3, "expected_seq incremented to 3");

    /* X cannot play on O's cell */
    TEST_ASSERT(!game_is_valid_move(&g, 5, 6), "Occupied cell (5, 6) is invalid for X");
    TEST_ASSERT(!game_make_move(&g, CELL_X, 5, 6), "CELL_X cannot play on occupied cell (5, 6)");
}

/* ========================================================================= */
/* Test 4: 4-Direction Win Detection (Horizontal, Vertical, Diagonals)       */
/* ========================================================================= */
static void test_4_direction_wins(void) {
    /* Direction 1: Horizontal Win (Streak = 5) for X */
    {
        game_t g;
        game_init(&g, 10, 10, 11, 1, 2);
        /* Moves:
           X: (3, 4), O: (0, 0)
           X: (3, 5), O: (0, 1)
           X: (3, 6), O: (0, 2)
           X: (3, 7), O: (0, 3)
           X: (3, 8) -> WIN
        */
        game_make_move(&g, CELL_X, 3, 4); game_make_move(&g, CELL_O, 0, 0);
        game_make_move(&g, CELL_X, 3, 5); game_make_move(&g, CELL_O, 0, 1);
        game_make_move(&g, CELL_X, 3, 6); game_make_move(&g, CELL_O, 0, 2);
        game_make_move(&g, CELL_X, 3, 7); game_make_move(&g, CELL_O, 0, 3);
        TEST_ASSERT(g.status == GAME_STATUS_IN_PROGRESS, "Horizontal 4-in-a-row is not yet win");

        bool win = game_make_move(&g, CELL_X, 3, 8);
        TEST_ASSERT(win, "Horizontal 5th piece move succeeds");
        TEST_ASSERT(g.status == GAME_STATUS_FINISHED, "Game status is FINISHED after horizontal win");
        TEST_ASSERT(g.result == GAME_RESULT_X_WON, "Result is X_WON for horizontal 5-in-a-row");
    }

    /* Direction 1b: Horizontal Win by filling in the middle */
    {
        game_t g;
        game_init(&g, 11, 10, 11, 1, 2);
        /* Place X at (6, 2), (6, 3), (6, 5), (6, 6) then fill middle (6, 4) */
        game_make_move(&g, CELL_X, 6, 2); game_make_move(&g, CELL_O, 0, 0);
        game_make_move(&g, CELL_X, 6, 3); game_make_move(&g, CELL_O, 0, 1);
        game_make_move(&g, CELL_X, 6, 5); game_make_move(&g, CELL_O, 0, 2);
        game_make_move(&g, CELL_X, 6, 6); game_make_move(&g, CELL_O, 0, 3);
        TEST_ASSERT(g.status == GAME_STATUS_IN_PROGRESS, "Split 4 pieces is not win");

        game_make_move(&g, CELL_X, 6, 4);
        TEST_ASSERT(g.status == GAME_STATUS_FINISHED, "Filling middle completes 5-in-a-row win");
        TEST_ASSERT(g.result == GAME_RESULT_X_WON, "Result is X_WON for middle-filled horizontal win");
    }

    /* Direction 2: Vertical Win (Streak = 5) for O */
    {
        game_t g;
        game_init(&g, 12, 10, 11, 1, 2);
        /* Moves:
           X: (0, 0), O: (4, 9)
           X: (1, 1), O: (5, 9)
           X: (0, 2), O: (6, 9)
           X: (1, 3), O: (7, 9)
           X: (0, 4), O: (8, 9) -> WIN
        */
        game_make_move(&g, CELL_X, 0, 0); game_make_move(&g, CELL_O, 4, 9);
        game_make_move(&g, CELL_X, 1, 1); game_make_move(&g, CELL_O, 5, 9);
        game_make_move(&g, CELL_X, 0, 2); game_make_move(&g, CELL_O, 6, 9);
        game_make_move(&g, CELL_X, 1, 3); game_make_move(&g, CELL_O, 7, 9);
        TEST_ASSERT(g.status == GAME_STATUS_IN_PROGRESS, "Vertical 4-in-a-row is not yet win");

        game_make_move(&g, CELL_X, 0, 4);
        bool o_won = game_make_move(&g, CELL_O, 8, 9);
        TEST_ASSERT(o_won, "Vertical 5th piece move succeeds");
        TEST_ASSERT(g.status == GAME_STATUS_FINISHED, "Game status is FINISHED after vertical win");
        TEST_ASSERT(g.result == GAME_RESULT_O_WON, "Result is O_WON for vertical 5-in-a-row");
    }

    /* Direction 3: Main Diagonal Win (\) for X */
    {
        game_t g;
        game_init(&g, 13, 10, 11, 1, 2);
        /* Moves:
           X: (1, 1), O: (0, 10)
           X: (2, 2), O: (0, 11)
           X: (3, 3), O: (0, 12)
           X: (4, 4), O: (0, 13)
           X: (5, 5) -> WIN
        */
        game_make_move(&g, CELL_X, 1, 1); game_make_move(&g, CELL_O, 0, 10);
        game_make_move(&g, CELL_X, 2, 2); game_make_move(&g, CELL_O, 0, 11);
        game_make_move(&g, CELL_X, 3, 3); game_make_move(&g, CELL_O, 0, 12);
        game_make_move(&g, CELL_X, 4, 4); game_make_move(&g, CELL_O, 0, 13);
        TEST_ASSERT(g.status == GAME_STATUS_IN_PROGRESS, "Main diagonal 4 pieces is not win");

        game_make_move(&g, CELL_X, 5, 5);
        TEST_ASSERT(g.status == GAME_STATUS_FINISHED, "Game status is FINISHED after main diagonal win");
        TEST_ASSERT(g.result == GAME_RESULT_X_WON, "Result is X_WON for main diagonal win");
    }

    /* Direction 4: Anti-Diagonal Win (/) for O */
    {
        game_t g;
        game_init(&g, 14, 10, 11, 1, 2);
        /* Moves:
           X: (0, 0), O: (8, 2)
           X: (1, 1), O: (7, 3)
           X: (0, 2), O: (6, 4)
           X: (1, 3), O: (5, 5)
           X: (0, 4), O: (4, 6) -> WIN
        */
        game_make_move(&g, CELL_X, 0, 0); game_make_move(&g, CELL_O, 8, 2);
        game_make_move(&g, CELL_X, 1, 1); game_make_move(&g, CELL_O, 7, 3);
        game_make_move(&g, CELL_X, 0, 2); game_make_move(&g, CELL_O, 6, 4);
        game_make_move(&g, CELL_X, 1, 3); game_make_move(&g, CELL_O, 5, 5);
        TEST_ASSERT(g.status == GAME_STATUS_IN_PROGRESS, "Anti-diagonal 4 pieces is not win");

        game_make_move(&g, CELL_X, 0, 4);
        bool o_won = game_make_move(&g, CELL_O, 4, 6);
        TEST_ASSERT(o_won, "Anti-diagonal 5th piece move succeeds");
        TEST_ASSERT(g.status == GAME_STATUS_FINISHED, "Game status is FINISHED after anti-diagonal win");
        TEST_ASSERT(g.result == GAME_RESULT_O_WON, "Result is O_WON for anti-diagonal win");
    }
}

/* ========================================================================= */
/* Test 5: Overline Win Condition (Streak >= 6 pieces)                       */
/* ========================================================================= */
static void test_overline_win_condition(void) {
    /* 5a: Horizontal overline with 6 consecutive pieces */
    {
        game_t g;
        game_init(&g, 20, 10, 11, 1, 2);
        for (int c = 2; c <= 7; ++c) {
            g.board[4][c] = CELL_X;
        }
        TEST_ASSERT(game_check_win(&g, 4, 2), "Horizontal overline (6 pieces) at head is win");
        TEST_ASSERT(game_check_win(&g, 4, 5), "Horizontal overline (6 pieces) at middle is win");
        TEST_ASSERT(game_check_win(&g, 4, 7), "Horizontal overline (6 pieces) at tail is win");
    }

    /* 5b: Vertical overline with 6 consecutive pieces */
    {
        game_t g;
        game_init(&g, 21, 10, 11, 1, 2);
        for (int r = 3; r <= 8; ++r) {
            g.board[r][5] = CELL_O;
        }
        TEST_ASSERT(game_check_win(&g, 3, 5), "Vertical overline (6 pieces) at head is win");
        TEST_ASSERT(game_check_win(&g, 6, 5), "Vertical overline (6 pieces) at middle is win");
        TEST_ASSERT(game_check_win(&g, 8, 5), "Vertical overline (6 pieces) at tail is win");
    }

    /* 5c: Main diagonal overline with 6 consecutive pieces */
    {
        game_t g;
        game_init(&g, 22, 10, 11, 1, 2);
        for (int i = 0; i < 6; ++i) {
            g.board[3 + i][2 + i] = CELL_X;
        }
        TEST_ASSERT(game_check_win(&g, 3, 2), "Main diagonal overline (6 pieces) at head is win");
        TEST_ASSERT(game_check_win(&g, 5, 4), "Main diagonal overline (6 pieces) at middle is win");
        TEST_ASSERT(game_check_win(&g, 8, 7), "Main diagonal overline (6 pieces) at tail is win");
    }

    /* 5d: Anti-diagonal overline with 6 consecutive pieces */
    {
        game_t g;
        game_init(&g, 23, 10, 11, 1, 2);
        for (int i = 0; i < 6; ++i) {
            g.board[9 - i][2 + i] = CELL_O;
        }
        TEST_ASSERT(game_check_win(&g, 9, 2), "Anti-diagonal overline (6 pieces) at head is win");
        TEST_ASSERT(game_check_win(&g, 7, 4), "Anti-diagonal overline (6 pieces) at middle is win");
        TEST_ASSERT(game_check_win(&g, 4, 7), "Anti-diagonal overline (6 pieces) at tail is win");
    }

    /* 5e: Long overline with 7 consecutive pieces */
    {
        game_t g;
        game_init(&g, 24, 10, 11, 1, 2);
        for (int c = 1; c <= 7; ++c) {
            g.board[8][c] = CELL_X;
        }
        TEST_ASSERT(game_check_win(&g, 8, 4), "Overline with 7 consecutive pieces is win");
    }

    /* 5f: Overline blocked at both ends by opponent (freestyle Gomoku: still a win) */
    {
        game_t g;
        game_init(&g, 25, 10, 11, 1, 2);
        g.board[5][1] = CELL_O; /* Block left */
        for (int c = 2; c <= 7; ++c) {
            g.board[5][c] = CELL_X; /* 6 X pieces */
        }
        g.board[5][8] = CELL_O; /* Block right */
        TEST_ASSERT(game_check_win(&g, 5, 4), "Overline blocked at both ends is recognized as WIN");
    }
}

/* ========================================================================= */
/* Test 6: Four Corners Win Scenarios (0,0), (0,14), (14,0), (14,14)         */
/* ========================================================================= */
static void test_four_corners_wins(void) {
    /* Corner 1: Top-Left (0, 0) */
    {
        /* Horizontal at row 0 */
        game_t g;
        game_init(&g, 30, 10, 11, 1, 2);
        for (int c = 0; c < 5; ++c) g.board[0][c] = CELL_X;
        TEST_ASSERT(game_check_win(&g, 0, 0), "Corner (0,0) horizontal win verified");

        /* Vertical at col 0 */
        game_reset_board(&g);
        for (int r = 0; r < 5; ++r) g.board[r][0] = CELL_X;
        TEST_ASSERT(game_check_win(&g, 0, 0), "Corner (0,0) vertical win verified");

        /* Main diagonal from (0,0) */
        game_reset_board(&g);
        for (int i = 0; i < 5; ++i) g.board[i][i] = CELL_X;
        TEST_ASSERT(game_check_win(&g, 0, 0), "Corner (0,0) main diagonal win verified");
    }

    /* Corner 2: Top-Right (0, 14) */
    {
        /* Horizontal ending at (0, 14) */
        game_t g;
        game_init(&g, 31, 10, 11, 1, 2);
        for (int c = 10; c <= 14; ++c) g.board[0][c] = CELL_O;
        TEST_ASSERT(game_check_win(&g, 0, 14), "Corner (0,14) horizontal win verified");

        /* Vertical starting at (0, 14) */
        game_reset_board(&g);
        for (int r = 0; r < 5; ++r) g.board[r][14] = CELL_O;
        TEST_ASSERT(game_check_win(&g, 0, 14), "Corner (0,14) vertical win verified");

        /* Anti-diagonal starting at (0, 14) */
        game_reset_board(&g);
        for (int i = 0; i < 5; ++i) g.board[i][14 - i] = CELL_O;
        TEST_ASSERT(game_check_win(&g, 0, 14), "Corner (0,14) anti-diagonal win verified");
    }

    /* Corner 3: Bottom-Left (14, 0) */
    {
        /* Horizontal starting at (14, 0) */
        game_t g;
        game_init(&g, 32, 10, 11, 1, 2);
        for (int c = 0; c < 5; ++c) g.board[14][c] = CELL_X;
        TEST_ASSERT(game_check_win(&g, 14, 0), "Corner (14,0) horizontal win verified");

        /* Vertical ending at (14, 0) */
        game_reset_board(&g);
        for (int r = 10; r <= 14; ++r) g.board[r][0] = CELL_X;
        TEST_ASSERT(game_check_win(&g, 14, 0), "Corner (14,0) vertical win verified");

        /* Anti-diagonal ending at (14, 0) */
        game_reset_board(&g);
        for (int i = 0; i < 5; ++i) g.board[14 - i][i] = CELL_X;
        TEST_ASSERT(game_check_win(&g, 14, 0), "Corner (14,0) anti-diagonal win verified");
    }

    /* Corner 4: Bottom-Right (14, 14) */
    {
        /* Horizontal ending at (14, 14) */
        game_t g;
        game_init(&g, 33, 10, 11, 1, 2);
        for (int c = 10; c <= 14; ++c) g.board[14][c] = CELL_O;
        TEST_ASSERT(game_check_win(&g, 14, 14), "Corner (14,14) horizontal win verified");

        /* Vertical ending at (14, 14) */
        game_reset_board(&g);
        for (int r = 10; r <= 14; ++r) g.board[r][14] = CELL_O;
        TEST_ASSERT(game_check_win(&g, 14, 14), "Corner (14,14) vertical win verified");

        /* Main diagonal ending at (14, 14) */
        game_reset_board(&g);
        for (int i = 0; i < 5; ++i) g.board[14 - i][14 - i] = CELL_O;
        TEST_ASSERT(game_check_win(&g, 14, 14), "Corner (14,14) main diagonal win verified");
    }
}

/* ========================================================================= */
/* Test 7: Full Board 225-Cell Draw Condition                                */
/* ========================================================================= */
static void test_full_board_225_draw(void) {
    game_t g;
    game_init(&g, 40, 10, 11, 100, 200);

    /* Construct a non-winning draw tiling on 15x15 board.
     * Formula: ((c + (r / 2)) % 4 < 2) ^ ((r % 2) == 1)
     * This creates 2x2 shifted blocks with max streak of 2 in rows, cols,
     * and diagonals, guaranteeing no 5-in-a-row anywhere on the 15x15 board.
     * Total pieces: exactly 113 for X and 112 for O (113 + 112 = 225).
     */
    typedef struct {
        int r;
        int c;
    } coord_t;

    coord_t x_moves[120];
    coord_t o_moves[120];
    int x_count = 0;
    int o_count = 0;

    for (int r = 0; r < CARO_BOARD_SIZE; ++r) {
        for (int c = 0; c < CARO_BOARD_SIZE; ++c) {
            bool is_x = (((c + (r / 2)) % 4 < 2) ^ ((r % 2) == 1));
            if (is_x) {
                x_moves[x_count].r = r;
                x_moves[x_count].c = c;
                x_count++;
            } else {
                o_moves[o_count].r = r;
                o_moves[o_count].c = c;
                o_count++;
            }
        }
    }

    TEST_ASSERT(x_count == 113, "Draw pattern has exactly 113 X cells");
    TEST_ASSERT(o_count == 112, "Draw pattern has exactly 112 O cells");
    TEST_ASSERT(x_count + o_count == 225, "Draw pattern covers all 225 cells");

    /* Play out all 225 moves through game_make_move() alternately */
    bool all_moves_succeeded = true;
    for (int i = 0; i < o_count; ++i) {
        /* X move */
        if (!game_make_move(&g, CELL_X, x_moves[i].r, x_moves[i].c)) {
            all_moves_succeeded = false;
            break;
        }
        if (g.status != GAME_STATUS_IN_PROGRESS) {
            all_moves_succeeded = false;
            break;
        }

        /* O move */
        if (!game_make_move(&g, CELL_O, o_moves[i].r, o_moves[i].c)) {
            all_moves_succeeded = false;
            break;
        }
        if (g.status != GAME_STATUS_IN_PROGRESS) {
            all_moves_succeeded = false;
            break;
        }
    }
    TEST_ASSERT(all_moves_succeeded, "First 224 moves placed without premature finish");
    TEST_ASSERT(!game_is_board_full(&g), "Board is not full before move 225");

    /* Play 225th move (the final move by X) */
    bool final_move_ok = game_make_move(&g, CELL_X, x_moves[x_count - 1].r, x_moves[x_count - 1].c);
    TEST_ASSERT(final_move_ok, "225th move placed successfully");
    TEST_ASSERT(game_is_board_full(&g), "game_is_board_full() returns true after 225 moves");
    TEST_ASSERT(g.status == GAME_STATUS_FINISHED, "Game status is FINISHED after filling board");
    TEST_ASSERT(g.result == GAME_RESULT_DRAW, "Game result is GAME_RESULT_DRAW");
    TEST_ASSERT(g.move_count == 225, "Total move_count is 225");
    TEST_ASSERT(g.expected_seq == 226, "expected_seq is 226");
}

/* ========================================================================= */
/* Test 8: Post-Game Move Rejection                                          */
/* ========================================================================= */
static void test_post_game_move_rejection(void) {
    /* 8a: After a game is won */
    {
        game_t g;
        game_init(&g, 50, 10, 11, 1, 2);
        /* Make a quick horizontal win for X */
        for (int c = 0; c < 4; ++c) {
            game_make_move(&g, CELL_X, 0, c);
            game_make_move(&g, CELL_O, 1, c);
        }
        game_make_move(&g, CELL_X, 0, 4); /* X wins */
        TEST_ASSERT(g.status == GAME_STATUS_FINISHED, "Game is finished (X won)");

        /* Pick an unoccupied cell */
        TEST_ASSERT(g.board[10][10] == CELL_EMPTY, "Cell (10, 10) is unoccupied");
        TEST_ASSERT(!game_is_valid_move(&g, 10, 10), "game_is_valid_move rejects moves on finished game");
        TEST_ASSERT(!game_make_move(&g, CELL_O, 10, 10), "game_make_move rejects O move after game finished");
        TEST_ASSERT(!game_make_move(&g, CELL_X, 10, 10), "game_make_move rejects X move after game finished");
        TEST_ASSERT(g.status == GAME_STATUS_FINISHED, "Game status remains FINISHED");
        TEST_ASSERT(g.result == GAME_RESULT_X_WON, "Game result remains X_WON");
    }

    /* 8b: After game status is set to ABORTED */
    {
        game_t g;
        game_init(&g, 51, 10, 11, 1, 2);
        g.status = GAME_STATUS_ABORTED;
        TEST_ASSERT(!game_is_valid_move(&g, 7, 7), "game_is_valid_move rejects moves on ABORTED game");
        TEST_ASSERT(!game_make_move(&g, CELL_X, 7, 7), "game_make_move rejects move on ABORTED game");
    }

    /* 8c: When game status is WAITING */
    {
        game_t g;
        game_init(&g, 52, 10, 11, 1, 2);
        g.status = GAME_STATUS_WAITING;
        TEST_ASSERT(!game_is_valid_move(&g, 7, 7), "game_is_valid_move rejects moves on WAITING game");
        TEST_ASSERT(!game_make_move(&g, CELL_X, 7, 7), "game_make_move rejects move on WAITING game");
    }
}

/* ========================================================================= */
/* Test 9: Rule Independence Verification (No Socket/Poll/SQL in Rules)      */
/* ========================================================================= */
static void test_rule_independence(void) {
    /* Verify game source files do not contain forbidden networking or database calls */
    const char *files_to_check[] = {
        "src/server/game.c",
        "include/game.h"
    };

    const char *forbidden_patterns[] = {
        "sys/socket.h",
        "netinet/in.h",
        "arpa/inet.h",
        "poll.h",
        "sqlite3.h",
        "winsock2.h",
        "ws2tcpip.h",
        "socket(",
        "bind(",
        "listen(",
        "accept(",
        "connect(",
        "send(",
        "recv(",
        "poll(",
        "sqlite3_"
    };

    size_t num_files = sizeof(files_to_check) / sizeof(files_to_check[0]);
    size_t num_patterns = sizeof(forbidden_patterns) / sizeof(forbidden_patterns[0]);

    for (size_t f = 0; f < num_files; ++f) {
        FILE *fp = fopen(files_to_check[f], "r");
        TEST_ASSERT(fp != NULL, "Opened game module file for independence check");
        if (!fp) continue;

        char line[512];
        bool has_forbidden = false;
        char matched_pattern[64] = {0};

        while (fgets(line, sizeof(line), fp)) {
            for (size_t p = 0; p < num_patterns; ++p) {
                if (strstr(line, forbidden_patterns[p]) != NULL) {
                    has_forbidden = true;
                    strncpy(matched_pattern, forbidden_patterns[p], sizeof(matched_pattern) - 1);
                    break;
                }
            }
            if (has_forbidden) break;
        }
        fclose(fp);

        TEST_ASSERT(!has_forbidden, "Game module file contains 0 socket/poll/SQL calls");
    }
}

/* ========================================================================= */
/* Main Test Runner                                                          */
/* ========================================================================= */
int main(void) {
    printf("=========================================\n");
    printf("     RUNNING CARO GAME UNIT TESTS (M1)   \n");
    printf("=========================================\n");

    test_game_init_and_null_safety();
    test_board_boundaries();
    test_move_placement_and_occupied_cells();
    test_4_direction_wins();
    test_overline_win_condition();
    test_four_corners_wins();
    test_full_board_225_draw();
    test_post_game_move_rejection();
    test_rule_independence();

    printf("=========================================\n");
    printf("   RESULTS: %d / %d tests passed\n", g_tests_passed, g_tests_run);
    printf("=========================================\n");

    return (g_tests_passed == g_tests_run) ? 0 : 1;
}
