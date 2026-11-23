#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <ctype.h>
#include <signal.h>

#if defined(_WIN32)
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <io.h>
    #include <conio.h>
    typedef int socklen_t;
    #define socket_close closesocket
    typedef WSAPOLLFD client_pollfd_t;
    #define client_poll(fds, nfds, timeout) WSAPoll((fds), (nfds), (timeout))
#else
    #include <unistd.h>
    #include <fcntl.h>
    #include <errno.h>
    #include <arpa/inet.h>
    #include <sys/socket.h>
    #include <poll.h>
    #define socket_close close
    typedef struct pollfd client_pollfd_t;
    #define client_poll(fds, nfds, timeout) poll((fds), (nfds), (timeout))
#endif

#include "protocol.h"
#include "game.h"

/* ------------------------------------------------------------------------- */
/* Client State Machine & Context Definitions                                */
/* ------------------------------------------------------------------------- */

typedef enum {
    CLI_STATE_UNAUTHENTICATED = 0,
    CLI_STATE_IN_LOBBY        = 1,
    CLI_STATE_IN_INVITE       = 2,
    CLI_STATE_IN_GAME         = 3
} client_cli_state_t;

typedef struct {
    int                 sock_fd;
    client_cli_state_t  state;
    uint32_t            req_id_counter;

    /* Authentication & Session */
    bool                logged_in;
    uint32_t            user_id;
    char                username[32];
    int                 points;
    int                 wins;
    int                 draws;
    int                 losses;

    /* Pending Invitations */
    uint32_t            pending_invite_id;
    char                pending_user[32];
    bool                is_challenger;      /* true if sent challenge, false if received */

    /* Active Game State */
    uint32_t            current_game_id;
    caro_cell_t         my_stone;           /* CELL_X or CELL_O */
    caro_cell_t         opp_stone;          /* CELL_O or CELL_X */
    char                opponent_name[32];
    caro_cell_t         current_turn;       /* CELL_X or CELL_O */
    uint32_t            expected_seq;       /* 1-indexed, starts at 1 */
    caro_cell_t         board[CARO_BOARD_SIZE][CARO_BOARD_SIZE];
    int                 last_row;
    int                 last_col;
    uint32_t            last_seq;
    caro_cell_t         last_stone;
    bool                draw_offered_by_me;
    bool                draw_offered_to_me;

    /* Correlating Pending Requests */
    proto_cmd_t         last_pending_cmd;
    uint32_t            last_req_id;
    char                pending_username[32];
} client_ctx_t;

/* Volatile flag for graceful signal handling */
static volatile sig_atomic_t g_stop_requested = 0;

static void handle_signal(int sig) {
    (void)sig;
    g_stop_requested = 1;
}

/* Trim leading and trailing whitespace characters */
static char *trim_whitespace(char *str) {
    if (!str) return NULL;
    while (*str && isspace((unsigned char)*str)) {
        str++;
    }
    if (*str == '\0') return str;

    char *end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) {
        *end = '\0';
        end--;
    }
    return str;
}

/* Initialize socket subsystem (Winsock on Windows) */
static int client_net_init(void) {
#if defined(_WIN32)
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "[-] WSAStartup failed\n");
        return -1;
    }
#endif
    return 0;
}

static void client_net_cleanup(void) {
#if defined(_WIN32)
    WSACleanup();
#endif
}

/* Establish TCP connection to server */
static int client_connect_tcp(const char *host, int port) {
    int sock_fd = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        perror("[-] Failed to create socket");
        return -1;
    }

    struct sockaddr_in srv_addr;
    memset(&srv_addr, 0, sizeof(srv_addr));
    srv_addr.sin_family = AF_INET;
    srv_addr.sin_port = htons((uint16_t)port);

    if (inet_pton(AF_INET, host, &srv_addr.sin_addr) <= 0) {
        fprintf(stderr, "[-] Invalid IP address: %s\n", host);
        socket_close(sock_fd);
        return -1;
    }

    if (connect(sock_fd, (struct sockaddr *)&srv_addr, sizeof(srv_addr)) < 0) {
        perror("[-] Connection failed");
        socket_close(sock_fd);
        return -1;
    }

    return sock_fd;
}

/* Send a complete framed payload to the socket */
static int client_send_msg(int sock_fd, const char *payload) {
    if (sock_fd < 0 || !payload) return -1;

    uint8_t frame_buf[PROTO_MAX_FRAME_BUFFER];
    size_t frame_len = 0;
    size_t payload_len = strlen(payload);

    int pack_res = protocol_pack_frame(payload, payload_len, frame_buf, sizeof(frame_buf), &frame_len);
    if (pack_res != PROTO_OK) {
        fprintf(stderr, "[-] Failed to pack frame: %s\n", protocol_err_str((protocol_err_t)pack_res));
        return -1;
    }

    size_t total_sent = 0;
    while (total_sent < frame_len) {
#if defined(_WIN32)
        int sent = send((SOCKET)sock_fd, (const char *)(frame_buf + total_sent),
                        (int)(frame_len - total_sent), 0);
#else
#if defined(MSG_NOSIGNAL)
        int flags = MSG_NOSIGNAL;
#else
        int flags = 0;
#endif
        ssize_t sent = send(sock_fd, frame_buf + total_sent, frame_len - total_sent, flags);
#endif
        if (sent < 0) {
#if defined(_WIN32)
            int err = WSAGetLastError();
            if (err == WSAEINTR) continue;
#else
            if (errno == EINTR) continue;
#endif
            perror("[-] Failed to send frame to server");
            return -1;
        }
        total_sent += (size_t)sent;
    }

    return 0;
}

/* ------------------------------------------------------------------------- */
/* Board State Management & ASCII Rendering                                 */
/* ------------------------------------------------------------------------- */

static void client_ctx_init(client_ctx_t *ctx, int sock_fd) {
    if (!ctx) return;
    memset(ctx, 0, sizeof(*ctx));
    ctx->sock_fd = sock_fd;
    ctx->state = CLI_STATE_UNAUTHENTICATED;
    ctx->req_id_counter = 1000;
    ctx->last_row = -1;
    ctx->last_col = -1;
    ctx->last_stone = CELL_EMPTY;
    ctx->last_seq = 0;
    for (int r = 0; r < CARO_BOARD_SIZE; ++r) {
        for (int c = 0; c < CARO_BOARD_SIZE; ++c) {
            ctx->board[r][c] = CELL_EMPTY;
        }
    }
}

static void client_board_reset(client_ctx_t *ctx) {
    if (!ctx) return;
    for (int r = 0; r < CARO_BOARD_SIZE; ++r) {
        for (int c = 0; c < CARO_BOARD_SIZE; ++c) {
            ctx->board[r][c] = CELL_EMPTY;
        }
    }
    ctx->last_row = -1;
    ctx->last_col = -1;
    ctx->last_stone = CELL_EMPTY;
    ctx->last_seq = 0;
}

static int client_board_apply_move(client_ctx_t *ctx, uint32_t seq, caro_cell_t stone, int row, int col) {
    if (!ctx) return -1;
    if (row < 0 || row >= CARO_BOARD_SIZE || col < 0 || col >= CARO_BOARD_SIZE) {
        return -1;
    }
    ctx->board[row][col] = stone;
    ctx->last_row = row;
    ctx->last_col = col;
    ctx->last_stone = stone;
    ctx->last_seq = seq;
    return 0;
}

/**
 * @brief Render the 15x15 Gomoku board with 2-digit column headers, row headers,
 * cell intersections, last-move indicator, and status/turn banners.
 */
static void client_board_render(const client_ctx_t *ctx) {
    if (!ctx) return;
    if (ctx->state != CLI_STATE_IN_GAME && ctx->current_game_id == 0) {
        printf("[-] Not currently in an active game.\n");
        return;
    }

    char my_stone_char = (ctx->my_stone == CELL_X) ? 'X' : 'O';
    char opp_stone_char = (ctx->opp_stone == CELL_X) ? 'X' : 'O';

    printf("\n======================================================================\n");
    printf("  MATCH #%u | You: %s [%c]  vs  %s [%c]\n",
           ctx->current_game_id,
           (ctx->username[0] != '\0') ? ctx->username : "Player",
           my_stone_char,
           (ctx->opponent_name[0] != '\0') ? ctx->opponent_name : "Opponent",
           opp_stone_char);
    printf("======================================================================\n");

    /* Formatted grid with 2-digit column headers (00 01 02 ... 14) */
    printf("     00 01 02 03 04 05 06 07 08 09 10 11 12 13 14\n");
    printf("   +---------------------------------------------+\n");

    /* Row headers (00 | ... to 14 | ...) */
    for (int r = 0; r < CARO_BOARD_SIZE; ++r) {
        printf("%02d |", r);
        for (int c = 0; c < CARO_BOARD_SIZE; ++c) {
            bool is_last = (r == ctx->last_row && c == ctx->last_col);
            caro_cell_t cell = ctx->board[r][c];

            if (is_last) {
                if (cell == CELL_X) {
                    printf("[X]");
                } else if (cell == CELL_O) {
                    printf("[O]");
                } else {
                    printf(" . ");
                }
            } else {
                if (cell == CELL_X) {
                    printf(" X ");
                } else if (cell == CELL_O) {
                    printf(" O ");
                } else {
                    printf(" . ");
                }
            }
        }
        printf("|\n");
    }
    printf("   +---------------------------------------------+\n");

    /* Status banner underneath board */
    if (ctx->last_row >= 0 && ctx->last_col >= 0) {
        char st_c = (ctx->last_stone == CELL_X) ? 'X' : (ctx->last_stone == CELL_O ? 'O' : '?');
        printf("  Last move: [%c] at (row %d, col %d) [move #%u]\n",
               st_c, ctx->last_row, ctx->last_col, ctx->last_seq);
    } else {
        printf("  Last move: (none - starting board)\n");
    }

    /* Turn indicator */
    if (ctx->current_turn == ctx->my_stone) {
        printf("  >> YOUR TURN [ %c ] <<  (Enter 'move <row> <col>', e.g. 'move 7 7')\n", my_stone_char);
    } else {
        printf("  >> OPPONENT'S TURN [ %c ] <<  (Waiting for %s to move...)\n",
               opp_stone_char, ctx->opponent_name);
    }
    printf("  [Commands: move <row> <col> | resign | draw | board | help]\n");
    printf("======================================================================\n");
    fflush(stdout);
}

/**
 * @brief Parse user coordinate input. Supports numeric pairs "7 7" / "7,7"
 * and Gomoku coordinates like "H8", "h8", "A1", "O15".
 */
static bool client_parse_coordinates(const char *input, int *out_row, int *out_col) {
    if (!input || !out_row || !out_col) return false;

    while (*input && isspace((unsigned char)*input)) input++;
    if (*input == '\0') return false;

    /* 1. Try Gomoku coordinate like 'H8', 'h8', 'A1', 'O15' */
    if (isalpha((unsigned char)input[0])) {
        char col_char = (char)toupper((unsigned char)input[0]);
        if (col_char < 'A' || col_char > 'O') {
            return false;
        }
        int col = col_char - 'A';
        char *endptr = NULL;
        long row_num = strtol(input + 1, &endptr, 10);
        if (endptr == input + 1) {
            return false;
        }
        while (*endptr && isspace((unsigned char)*endptr)) endptr++;
        if (*endptr != '\0') return false;

        if (row_num < 1 || row_num > 15) {
            return false;
        }
        int row = (int)row_num - 1;

        *out_row = row;
        *out_col = col;
        return true;
    }

    /* 2. Try numeric pair 'row col' or 'row,col' */
    int r = -1, c = -1;
    char sep = '\0';
    int n = 0;
    if (sscanf(input, "%d%c%d%n", &r, &sep, &c, &n) == 3 && (sep == ' ' || sep == ',' || sep == '\t')) {
        const char *p = input + n;
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == '\0' && r >= 0 && r < CARO_BOARD_SIZE && c >= 0 && c < CARO_BOARD_SIZE) {
            *out_row = r;
            *out_col = c;
            return true;
        }
    } else {
        n = 0;
        if (sscanf(input, "%d %d%n", &r, &c, &n) == 2 || sscanf(input, "%d , %d%n", &r, &c, &n) == 2) {
            const char *p = input + n;
            while (*p && isspace((unsigned char)*p)) p++;
            if (*p == '\0' && r >= 0 && r < CARO_BOARD_SIZE && c >= 0 && c < CARO_BOARD_SIZE) {
                *out_row = r;
                *out_col = c;
                return true;
            }
        }
    }

    return false;
}

/* ------------------------------------------------------------------------- */
/* Error Formatting & Game Outcome Presentation                              */
/* ------------------------------------------------------------------------- */

static void client_display_error(int code, const char *err_msg) {
    const char *advice = "";
    switch (code) {
        case 100: advice = "Communication framing error. Please retry."; break;
        case 101: advice = "Unknown command. Type 'help' to see available commands."; break;
        case 102: advice = "Invalid parameter. Please check coordinates or command syntax."; break;
        case 200: advice = "You must log in first! Use 'login <user> <pass>'."; break;
        case 201: advice = "Authentication failed: invalid username or password."; break;
        case 202: advice = "Username already exists. Please choose another username."; break;
        case 203: advice = "Account is already active on another connection."; break;
        case 300: advice = "Target user not found. Type 'online' to see available players."; break;
        case 301: advice = "Player is busy (already in match or handling another invite)."; break;
        case 302: advice = "Challenge expired (30-second response timeout)."; break;
        case 303: advice = "Invalid challenge invitation ID."; break;
        case 400: advice = "You are not currently in this match."; break;
        case 401: advice = "Not your turn! Please wait for your opponent to move."; break;
        case 402: advice = "Illegal move: cell occupied or coordinates out of bounds [0..14]."; break;
        case 403: advice = "Sequence number mismatch; synchronizing state..."; break;
        case 404: advice = "Match not found or already concluded."; break;
        case 500: advice = "Internal server error. Please try again."; break;
        case 501: advice = "Database operation error."; break;
        default:  advice = ""; break;
    }

    printf("\n[-] SERVER ERROR %d: %s\n", code, (err_msg && *err_msg) ? err_msg : "Error");
    if (advice[0] != '\0') {
        printf("    -> %s\n", advice);
    }
    fflush(stdout);
}

static void client_display_game_over(const client_ctx_t *ctx, const char *outcome,
                                     const char *winner_name, const char *reason) {
    printf("\n**********************************************************************\n");
    if (outcome && strcmp(outcome, "DRAW") == 0) {
        printf("*                       *** MATCH DRAW ***                           *\n");
        printf("*  Match #%-6u ended in a DRAW                                       *\n", ctx->current_game_id);
        printf("*  Reason: %-58s*\n", reason ? reason : "Draw agreement or full board");
    } else if (ctx->logged_in && winner_name && strcmp(winner_name, ctx->username) == 0) {
        printf("*                        *** YOU WON! ***                            *\n");
        printf("*  Congratulations, %-15s! You won Match #%-6u             *\n", ctx->username, ctx->current_game_id);
        printf("*  Reason: %-58s*\n", reason ? reason : "Victory condition met");
    } else {
        printf("*                        *** YOU LOST ***                            *\n");
        printf("*  Winner: %-20s  Match #%-6u                        *\n",
               winner_name ? winner_name : "Opponent", ctx->current_game_id);
        printf("*  Reason: %-58s*\n", reason ? reason : "Match concluded");
        printf("*  Better luck next time!                                            *\n");
    }
    printf("**********************************************************************\n");
    printf("[*] Returned to lobby. Type 'online' to view players, or 'challenge <user>' to play again.\n\n");
    fflush(stdout);
}

static void client_display_online_list(const protocol_msg_t *msg) {
    int count = 0;
    if (msg->token_count > 2) {
        count = atoi(msg->tokens[2]);
    }
    printf("\n======================================================\n");
    printf("              ONLINE USERS (Total: %d)                \n", count);
    printf("======================================================\n");
    printf("  %-24s | %-15s\n", "Username", "Status");
    printf("------------------------------------------------------\n");

    if (msg->token_count <= 3 || count == 0) {
        printf("  (No other users online)\n");
    } else {
        for (int i = 3; i < msg->token_count; ++i) {
            char entry[64];
            strncpy(entry, msg->tokens[i], sizeof(entry) - 1);
            entry[sizeof(entry) - 1] = '\0';

            char *colon = strchr(entry, ':');
            if (colon) {
                *colon = '\0';
                const char *uname = entry;
                const char *st = colon + 1;
                printf("  %-24s | %-15s\n", uname, st);
            } else {
                printf("  %-24s |\n", entry);
            }
        }
    }
    printf("======================================================\n\n");
    fflush(stdout);
}

static void client_print_prompt(const client_ctx_t *ctx) {
    if (!ctx) {
        printf("caro> ");
    } else {
        switch (ctx->state) {
            case CLI_STATE_UNAUTHENTICATED:
                printf("caro (guest)> ");
                break;
            case CLI_STATE_IN_LOBBY:
                printf("caro [%s@lobby]> ", ctx->username[0] ? ctx->username : "guest");
                break;
            case CLI_STATE_IN_INVITE:
                printf("caro [%s@invite]> ", ctx->username[0] ? ctx->username : "guest");
                break;
            case CLI_STATE_IN_GAME: {
                char stone = (ctx->my_stone == CELL_X) ? 'X' : 'O';
                if (ctx->current_turn == ctx->my_stone) {
                    printf("caro [%s(%c) TURN #%u]> ", ctx->username, stone, ctx->expected_seq);
                } else {
                    printf("caro [%s(%c) waiting...]> ", ctx->username, stone);
                }
                break;
            }
            default:
                printf("caro> ");
                break;
        }
    }
    fflush(stdout);
}

static void client_print_help(const client_ctx_t *ctx) {
    printf("=================== CARO ONLINE CLIENT COMMANDS ===================\n");

    if (!ctx || ctx->state == CLI_STATE_UNAUTHENTICATED) {
        printf("  Authentication Commands:\n");
        printf("    register <username> <password> : Create a new Caro account\n");
        printf("    login <username> <password>    : Log into your account\n");
    }

    if (ctx && (ctx->state == CLI_STATE_IN_LOBBY || ctx->state == CLI_STATE_IN_INVITE)) {
        printf("  Lobby Commands:\n");
        printf("    online                         : List active online users and statuses\n");
        printf("    challenge <username>           : Challenge a player to a match\n");
    }

    if (ctx && ctx->state == CLI_STATE_IN_INVITE) {
        printf("  Invitation Commands:\n");
        printf("    accept [invite_id]             : Accept pending challenge invitation\n");
        printf("    decline [invite_id]            : Decline pending challenge invitation\n");
    }

    if (ctx && ctx->state == CLI_STATE_IN_GAME) {
        printf("  In-Game Match Commands:\n");
        printf("    move <row> <col>               : Place stone (e.g. 'move 7 7' or '7 7')\n");
        printf("    board                          : Redraw current 15x15 board\n");
        printf("    draw / offer_draw              : Offer a draw to opponent\n");
        printf("    accept_draw / draw accept      : Accept opponent's draw offer\n");
        printf("    decline_draw / draw decline    : Decline opponent's draw offer\n");
        printf("    resign                         : Forfeit current match\n");
    }

    printf("  General Commands:\n");
    printf("    help / ?                       : Show this context-sensitive help\n");
    printf("    ping <msg>                     : Ping the server to test connection\n");
    printf("    quit / exit                    : Disconnect and exit\n");
    printf("======================================================================\n");
    fflush(stdout);
}

/* ------------------------------------------------------------------------- */
/* Server Message Demultiplexer & State Sync                                 */
/* ------------------------------------------------------------------------- */

static void client_display_server_message(client_ctx_t *ctx, const char *payload, size_t len) {
    protocol_msg_t msg;
    int parse_res = protocol_parse_msg(payload, len, &msg);
    if (parse_res != PROTO_OK) {
        printf("\n[SERVER RAW] %.*s\n", (int)len, payload);
        return;
    }

    switch (msg.cmd) {
        case CMD_PONG: {
            const char *echo_str = (msg.token_count > 1) ? msg.tokens[1] : "";
            printf("\n[SERVER] PONG: %s\n", echo_str);
            break;
        }

        case CMD_ERR: {
            int code = 0;
            const char *err_text = "";
            if (msg.token_count >= 4) {
                code = atoi(msg.tokens[2]);
                err_text = msg.tokens[3];
            } else if (msg.token_count == 3) {
                code = atoi(msg.tokens[1]);
                err_text = msg.tokens[2];
            } else if (msg.token_count == 2) {
                code = atoi(msg.tokens[1]);
            }

            client_display_error(code, err_text);

            /* Reset invite state on invite error / timeout */
            if (ctx->state == CLI_STATE_IN_INVITE) {
                if (code == ERR_INVITE_EXPIRED || code == ERR_INVITE_INVALID ||
                    code == ERR_USER_BUSY || code == ERR_USER_NOT_FOUND) {
                    ctx->state = ctx->logged_in ? CLI_STATE_IN_LOBBY : CLI_STATE_UNAUTHENTICATED;
                    ctx->pending_invite_id = 0;
                    ctx->pending_user[0] = '\0';
                    ctx->is_challenger = false;
                }
            }
            if (ctx->last_pending_cmd != CMD_UNKNOWN) {
                ctx->last_pending_cmd = CMD_UNKNOWN;
            }
            break;
        }

        case CMD_OK: {
            const char *req_id_str = (msg.token_count > 1) ? msg.tokens[1] : "0";
            uint32_t req_id = (uint32_t)strtoul(req_id_str, NULL, 10);

            /* 1. Login OK: OK\t<req_id>\t<user_id>\t<points>\t<wins>\t<draws>\t<losses>\n */
            if (ctx->last_pending_cmd == CMD_LOGIN && msg.token_count >= 6) {
                ctx->logged_in = true;
                ctx->state = CLI_STATE_IN_LOBBY;
                ctx->user_id = (uint32_t)strtoul(msg.tokens[2], NULL, 10);
                if (ctx->pending_username[0] != '\0') {
                    strncpy(ctx->username, ctx->pending_username, sizeof(ctx->username) - 1);
                    ctx->username[sizeof(ctx->username) - 1] = '\0';
                    ctx->pending_username[0] = '\0';
                }
                ctx->points = atoi(msg.tokens[3]);
                ctx->wins = atoi(msg.tokens[4]);
                ctx->draws = atoi(msg.tokens[5]);
                ctx->losses = (msg.token_count > 6) ? atoi(msg.tokens[6]) : 0;
                ctx->last_pending_cmd = CMD_UNKNOWN;

                printf("\n======================================================================\n");
                printf("  [+] LOGIN SUCCESSFUL! Welcome, %s (UID: %u)\n", ctx->username, ctx->user_id);
                printf("  [STATS] Points: %d | Wins: %d | Draws: %d | Losses: %d\n",
                       ctx->points, ctx->wins, ctx->draws, ctx->losses);
                printf("  Type 'online' to view players, or 'challenge <user>' to play Caro!\n");
                printf("======================================================================\n\n");
            }
            /* 2. Register OK: OK\t<req_id>\t<user_id>\t<username>\n */
            else if (ctx->last_pending_cmd == CMD_REGISTER && msg.token_count >= 4) {
                uint32_t uid = (uint32_t)strtoul(msg.tokens[2], NULL, 10);
                const char *uname = msg.tokens[3];
                ctx->last_pending_cmd = CMD_UNKNOWN;

                printf("\n======================================================================\n");
                printf("  [+] REGISTRATION SUCCESSFUL!\n");
                printf("  Account '%s' created with User ID #%u.\n", uname, uid);
                printf("  You can now log in using: login %s <password>\n", uname);
                printf("======================================================================\n\n");
            }
            /* 3. Challenge OK (sent to challenger upon dispatching invite) */
            else if (ctx->last_pending_cmd == CMD_CHALLENGE) {
                ctx->state = CLI_STATE_IN_INVITE;
                ctx->is_challenger = true;
                ctx->last_pending_cmd = CMD_UNKNOWN;

                printf("\n[*] Challenge invitation dispatched to '%s'. Waiting for response (30s timeout)...\n",
                       ctx->pending_user);
            }
            /* 4. Generic OK */
            else {
                printf("\n[SERVER] OK (request_id=%u)\n", req_id);
            }
            break;
        }

        case CMD_CHALLENGE_REQ: {
            if (msg.token_count >= 4) {
                uint32_t inv_id = (uint32_t)strtoul(msg.tokens[1], NULL, 10);
                const char *challenger_uid = msg.tokens[2];
                const char *challenger_name = msg.tokens[3];

                ctx->pending_invite_id = inv_id;
                strncpy(ctx->pending_user, challenger_name, sizeof(ctx->pending_user) - 1);
                ctx->pending_user[sizeof(ctx->pending_user) - 1] = '\0';
                ctx->is_challenger = false;
                if (ctx->state != CLI_STATE_IN_GAME) {
                    ctx->state = CLI_STATE_IN_INVITE;
                }

                printf("\n======================================================================\n");
                printf("  [!] INCOMING CHALLENGE: Player '%s' (UID #%s) has challenged you!\n",
                       challenger_name, challenger_uid);
                printf("      Invite ID: %u (Expires in 30 seconds)\n", inv_id);
                printf("      -> Type 'accept' (or 'accept %u') to accept and play\n", inv_id);
                printf("      -> Type 'decline' (or 'decline %u') to decline\n", inv_id);
                printf("======================================================================\n\n");
            }
            break;
        }

        case CMD_GAME_START: {
            if (msg.token_count >= 4) {
                ctx->current_game_id = msg.game_id;
                char stone_char = msg.tokens[2][0];
                if (stone_char == 'X' || stone_char == 'x') {
                    ctx->my_stone = CELL_X;
                    ctx->opp_stone = CELL_O;
                } else {
                    ctx->my_stone = CELL_O;
                    ctx->opp_stone = CELL_X;
                }
                strncpy(ctx->opponent_name, msg.tokens[3], sizeof(ctx->opponent_name) - 1);
                ctx->opponent_name[sizeof(ctx->opponent_name) - 1] = '\0';

                client_board_reset(ctx);
                ctx->expected_seq = 1;
                ctx->current_turn = CELL_X;
                ctx->state = CLI_STATE_IN_GAME;
                ctx->pending_invite_id = 0;
                ctx->pending_user[0] = '\0';
                ctx->is_challenger = false;
                ctx->draw_offered_by_me = false;
                ctx->draw_offered_to_me = false;

                printf("\n======================================================================\n");
                printf("  [+] MATCH STARTED! Match #%u\n", ctx->current_game_id);
                printf("      Opponent : %s\n", ctx->opponent_name);
                printf("      Your Role: %c (%s)\n",
                       (ctx->my_stone == CELL_X) ? 'X' : 'O',
                       (ctx->my_stone == CELL_X) ? "Plays First" : "Plays Second");
                printf("======================================================================\n");

                client_board_render(ctx);
            }
            break;
        }

        case CMD_MOVE_ACK: {
            if (ctx->state == CLI_STATE_IN_GAME && msg.game_id == ctx->current_game_id) {
                client_board_apply_move(ctx, msg.seq, ctx->my_stone, msg.move_row, msg.move_col);
                ctx->expected_seq = msg.seq + 1;
                ctx->current_turn = ctx->opp_stone;

                client_board_render(ctx);
                printf("[+] Move acknowledged: [%c] placed at (%d, %d). Waiting for %s...\n",
                       (ctx->my_stone == CELL_X) ? 'X' : 'O',
                       msg.move_row, msg.move_col, ctx->opponent_name);
            }
            break;
        }

        case CMD_MOVE_BROADCAST: {
            if (ctx->state == CLI_STATE_IN_GAME && msg.game_id == ctx->current_game_id) {
                client_board_apply_move(ctx, msg.seq, ctx->opp_stone, msg.move_row, msg.move_col);
                ctx->expected_seq = msg.seq + 1;
                ctx->current_turn = ctx->my_stone;

                client_board_render(ctx);
                printf("[*] Opponent %s played [%c] at (%d, %d). YOUR TURN!\n",
                       ctx->opponent_name,
                       (ctx->opp_stone == CELL_X) ? 'X' : 'O',
                       msg.move_row, msg.move_col);
            }
            break;
        }

        case CMD_OFFER_DRAW: {
            if (ctx->state == CLI_STATE_IN_GAME && msg.game_id == ctx->current_game_id) {
                ctx->draw_offered_to_me = true;
                printf("\n======================================================================\n");
                printf("  [!] DRAW OFFER: Opponent '%s' has offered a draw!\n", ctx->opponent_name);
                printf("      -> Type 'accept_draw' (or 'draw accept') to accept the draw\n");
                printf("      -> Type 'decline_draw' (or 'draw decline') to decline\n");
                printf("======================================================================\n\n");
            }
            break;
        }

        case CMD_DRAW_RESP: {
            if (ctx->state == CLI_STATE_IN_GAME) {
                ctx->draw_offered_by_me = false;
                printf("\n[-] Opponent %s declined your draw offer. Match continues!\n\n", ctx->opponent_name);
            }
            break;
        }

        case CMD_GAME_OVER: {
            const char *outcome = (msg.token_count > 2) ? msg.tokens[2] : "OVER";
            const char *winner = (msg.token_count > 3) ? msg.tokens[3] : "NONE";
            const char *reason = (msg.token_count > 4) ? msg.tokens[4] : "CONCLUDED";

            client_display_game_over(ctx, outcome, winner, reason);

            ctx->state = ctx->logged_in ? CLI_STATE_IN_LOBBY : CLI_STATE_UNAUTHENTICATED;
            ctx->current_game_id = 0;
            ctx->draw_offered_by_me = false;
            ctx->draw_offered_to_me = false;
            break;
        }

        case CMD_ONLINE_LIST: {
            client_display_online_list(&msg);
            break;
        }

        default: {
            printf("\n[SERVER] %s\n", msg.raw_payload);
            break;
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Interactive User Input Dispatcher                                         */
/* ------------------------------------------------------------------------- */

static int client_dispatch_user_input(client_ctx_t *ctx, char *line, bool *out_running) {
    char *cmd = trim_whitespace(line);
    if (*cmd == '\0') {
        return 0;
    }

    /* 1. quit / exit */
    if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "exit") == 0) {
        if (ctx->state == CLI_STATE_IN_GAME) {
            printf("[!] Quitting while in match forfeits the game.\n");
        }
        printf("[*] Disconnecting from server and exiting...\n");
        *out_running = false;
        return 0;
    }

    /* 2. help / ? */
    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
        client_print_help(ctx);
        return 0;
    }

    /* 3. ping <msg> */
    if (strncmp(cmd, "ping", 4) == 0 && (cmd[4] == ' ' || cmd[4] == '\t' || cmd[4] == '\0')) {
        char *arg = cmd + 4;
        while (*arg && isspace((unsigned char)*arg)) arg++;
        const char *ping_msg = (*arg != '\0') ? arg : "hello";

        char payload[PROTO_MAX_PAYLOAD_SIZE];
        protocol_create_ping(ping_msg, payload, sizeof(payload));

        if (client_send_msg(ctx->sock_fd, payload) == 0) {
            printf("[*] Sent PING: '%s' -> Waiting for PONG...\n", ping_msg);
        } else {
            fprintf(stderr, "[-] Failed to send PING\n");
        }
        return 0;
    }

    /* 4. register <username> <password> */
    if (strncmp(cmd, "register", 8) == 0 && (cmd[8] == ' ' || cmd[8] == '\t')) {
        if (ctx->logged_in) {
            printf("[!] Already logged in as '%s'. Please quit to create a new account.\n", ctx->username);
            return 0;
        }
        char user[64], pass[64];
        if (sscanf(cmd + 8, "%63s %63s", user, pass) != 2) {
            printf("[!] Usage: register <username> <password>\n");
            return 0;
        }
        uint32_t req_id = ++ctx->req_id_counter;
        ctx->last_req_id = req_id;
        ctx->last_pending_cmd = CMD_REGISTER;
        strncpy(ctx->pending_username, user, sizeof(ctx->pending_username) - 1);
        ctx->pending_username[sizeof(ctx->pending_username) - 1] = '\0';

        char payload[PROTO_MAX_PAYLOAD_SIZE];
        snprintf(payload, sizeof(payload), "REGISTER\t%u\t%s\t%s\n", req_id, user, pass);
        if (client_send_msg(ctx->sock_fd, payload) == 0) {
            printf("[*] Registering account '%s'...\n", user);
        } else {
            fprintf(stderr, "[-] Failed to send registration request.\n");
        }
        return 0;
    }

    /* 5. login <username> <password> */
    if (strncmp(cmd, "login", 5) == 0 && (cmd[5] == ' ' || cmd[5] == '\t')) {
        if (ctx->logged_in) {
            printf("[!] Already logged in as '%s'.\n", ctx->username);
            return 0;
        }
        char user[64], pass[64];
        if (sscanf(cmd + 5, "%63s %63s", user, pass) != 2) {
            printf("[!] Usage: login <username> <password>\n");
            return 0;
        }
        uint32_t req_id = ++ctx->req_id_counter;
        ctx->last_req_id = req_id;
        ctx->last_pending_cmd = CMD_LOGIN;
        strncpy(ctx->pending_username, user, sizeof(ctx->pending_username) - 1);
        ctx->pending_username[sizeof(ctx->pending_username) - 1] = '\0';

        char payload[PROTO_MAX_PAYLOAD_SIZE];
        snprintf(payload, sizeof(payload), "LOGIN\t%u\t%s\t%s\n", req_id, user, pass);
        if (client_send_msg(ctx->sock_fd, payload) == 0) {
            printf("[*] Logging in as '%s'...\n", user);
        } else {
            fprintf(stderr, "[-] Failed to send login request.\n");
        }
        return 0;
    }

    /* 6. online / list */
    if (strcmp(cmd, "online") == 0 || strcmp(cmd, "list") == 0) {
        if (!ctx->logged_in) {
            printf("[!] Please log in first: 'login <username> <password>'.\n");
            return 0;
        }
        uint32_t req_id = ++ctx->req_id_counter;
        char payload[64];
        snprintf(payload, sizeof(payload), "GET_ONLINE\t%u\n", req_id);
        if (client_send_msg(ctx->sock_fd, payload) < 0) {
            fprintf(stderr, "[-] Failed to request online list.\n");
        }
        return 0;
    }

    /* 7. challenge <username> */
    if (strncmp(cmd, "challenge", 9) == 0 && (cmd[9] == ' ' || cmd[9] == '\t')) {
        if (!ctx->logged_in) {
            printf("[!] Please log in first: 'login <username> <password>'.\n");
            return 0;
        }
        if (ctx->state == CLI_STATE_IN_GAME) {
            printf("[!] Cannot challenge while in an active match! Finish or resign first.\n");
            return 0;
        }
        if (ctx->state == CLI_STATE_IN_INVITE) {
            printf("[!] You already have a pending challenge in progress. Please wait.\n");
            return 0;
        }
        char target[64];
        if (sscanf(cmd + 9, "%63s", target) != 1) {
            printf("[!] Usage: challenge <username>\n");
            return 0;
        }
        if (strcmp(target, ctx->username) == 0) {
            printf("[!] You cannot challenge yourself.\n");
            return 0;
        }
        uint32_t req_id = ++ctx->req_id_counter;
        ctx->last_req_id = req_id;
        ctx->last_pending_cmd = CMD_CHALLENGE;
        strncpy(ctx->pending_user, target, sizeof(ctx->pending_user) - 1);
        ctx->pending_user[sizeof(ctx->pending_user) - 1] = '\0';

        char payload[128];
        snprintf(payload, sizeof(payload), "CHALLENGE\t%u\t%s\n", req_id, target);
        if (client_send_msg(ctx->sock_fd, payload) == 0) {
            printf("[*] Sending challenge to '%s'...\n", target);
        } else {
            fprintf(stderr, "[-] Failed to send challenge.\n");
        }
        return 0;
    }

    /* 8. accept [invite_id] */
    if (strncmp(cmd, "accept", 6) == 0 && (cmd[6] == ' ' || cmd[6] == '\t' || cmd[6] == '\0')) {
        /* Distinguish from accept_draw */
        if (strcmp(cmd, "accept_draw") != 0) {
            if (!ctx->logged_in) {
                printf("[!] Please log in first.\n");
                return 0;
            }
            uint32_t inv_id = ctx->pending_invite_id;
            char *arg = cmd + 6;
            while (*arg && isspace((unsigned char)*arg)) arg++;
            if (*arg != '\0') {
                inv_id = (uint32_t)strtoul(arg, NULL, 10);
            }
            if (inv_id == 0) {
                printf("[!] No pending challenge to accept.\n");
                return 0;
            }
            char payload[64];
            snprintf(payload, sizeof(payload), "CHALLENGE_RESP\t%u\t1\n", inv_id);
            if (client_send_msg(ctx->sock_fd, payload) == 0) {
                printf("[*] Accepting challenge (Invite #%u). Starting match...\n", inv_id);
            }
            return 0;
        }
    }

    /* 9. decline [invite_id] */
    if (strncmp(cmd, "decline", 7) == 0 && (cmd[7] == ' ' || cmd[7] == '\t' || cmd[7] == '\0')) {
        /* Distinguish from decline_draw */
        if (strcmp(cmd, "decline_draw") != 0) {
            if (!ctx->logged_in) {
                printf("[!] Please log in first.\n");
                return 0;
            }
            uint32_t inv_id = ctx->pending_invite_id;
            char *arg = cmd + 7;
            while (*arg && isspace((unsigned char)*arg)) arg++;
            if (*arg != '\0') {
                inv_id = (uint32_t)strtoul(arg, NULL, 10);
            }
            if (inv_id == 0) {
                printf("[!] No pending challenge to decline.\n");
                return 0;
            }
            char payload[64];
            snprintf(payload, sizeof(payload), "CHALLENGE_RESP\t%u\t0\n", inv_id);
            if (client_send_msg(ctx->sock_fd, payload) == 0) {
                printf("[*] Declined challenge (Invite #%u).\n", inv_id);
                ctx->pending_invite_id = 0;
                if (ctx->state == CLI_STATE_IN_INVITE) {
                    ctx->state = CLI_STATE_IN_LOBBY;
                }
            }
            return 0;
        }
    }

    /* 10. board */
    if (strcmp(cmd, "board") == 0) {
        if (ctx->state != CLI_STATE_IN_GAME && ctx->current_game_id == 0) {
            printf("[!] Not currently in an active game.\n");
            return 0;
        }
        client_board_render(ctx);
        return 0;
    }

    /* 11. resign */
    if (strcmp(cmd, "resign") == 0) {
        if (ctx->state != CLI_STATE_IN_GAME || ctx->current_game_id == 0) {
            printf("[!] Not currently in an active match.\n");
            return 0;
        }
        char payload[64];
        snprintf(payload, sizeof(payload), "RESIGN\t%u\n", ctx->current_game_id);
        if (client_send_msg(ctx->sock_fd, payload) == 0) {
            printf("[*] Resigning from match #%u...\n", ctx->current_game_id);
        }
        return 0;
    }

    /* 12. draw / offer_draw / draw offer */
    if (strcmp(cmd, "draw") == 0 || strcmp(cmd, "offer_draw") == 0 || strcmp(cmd, "draw offer") == 0) {
        if (ctx->state != CLI_STATE_IN_GAME || ctx->current_game_id == 0) {
            printf("[!] Not currently in an active match.\n");
            return 0;
        }
        if (ctx->draw_offered_by_me) {
            printf("[!] You have already offered a draw. Waiting for opponent response.\n");
            return 0;
        }
        char payload[64];
        snprintf(payload, sizeof(payload), "OFFER_DRAW\t%u\n", ctx->current_game_id);
        if (client_send_msg(ctx->sock_fd, payload) == 0) {
            ctx->draw_offered_by_me = true;
            printf("[*] Draw offer sent to %s. Waiting for response...\n", ctx->opponent_name);
        }
        return 0;
    }

    /* 13. accept_draw / draw accept */
    if (strcmp(cmd, "accept_draw") == 0 || strcmp(cmd, "draw accept") == 0) {
        if (ctx->state != CLI_STATE_IN_GAME || ctx->current_game_id == 0) {
            printf("[!] Not currently in an active match.\n");
            return 0;
        }
        if (!ctx->draw_offered_to_me) {
            printf("[!] No draw offer pending from opponent to accept.\n");
            return 0;
        }
        char payload[64];
        snprintf(payload, sizeof(payload), "DRAW_RESP\t%u\t1\n", ctx->current_game_id);
        if (client_send_msg(ctx->sock_fd, payload) == 0) {
            printf("[*] Accepted draw offer. Concluding match in a draw...\n");
        }
        return 0;
    }

    /* 14. decline_draw / draw decline */
    if (strcmp(cmd, "decline_draw") == 0 || strcmp(cmd, "draw decline") == 0) {
        if (ctx->state != CLI_STATE_IN_GAME || ctx->current_game_id == 0) {
            printf("[!] Not currently in an active match.\n");
            return 0;
        }
        if (!ctx->draw_offered_to_me) {
            printf("[!] No draw offer pending to decline.\n");
            return 0;
        }
        char payload[64];
        snprintf(payload, sizeof(payload), "DRAW_RESP\t%u\t0\n", ctx->current_game_id);
        if (client_send_msg(ctx->sock_fd, payload) == 0) {
            printf("[*] Declined draw offer. Continuing match...\n");
            ctx->draw_offered_to_me = false;
        }
        return 0;
    }

    /* 15. move <row> <col> or direct coordinates */
    bool is_explicit_move = (strncmp(cmd, "move", 4) == 0 && (cmd[4] == ' ' || cmd[4] == '\t'));
    bool is_implicit_move = (ctx->state == CLI_STATE_IN_GAME &&
                            (isdigit((unsigned char)*cmd) || isalpha((unsigned char)*cmd)));

    if (is_explicit_move || is_implicit_move) {
        const char *coord_str = is_explicit_move ? cmd + 4 : cmd;
        while (*coord_str && isspace((unsigned char)*coord_str)) coord_str++;

        if (ctx->state != CLI_STATE_IN_GAME || ctx->current_game_id == 0) {
            printf("[!] You are not currently in a match. Type 'online' or 'challenge <user>' to start.\n");
            return 0;
        }

        if (ctx->current_turn != ctx->my_stone) {
            char opp_st = (ctx->opp_stone == CELL_X) ? 'X' : 'O';
            printf("[!] It is not your turn! Waiting for %s [%c] to play.\n", ctx->opponent_name, opp_st);
            return 0;
        }

        int row = -1, col = -1;
        if (!client_parse_coordinates(coord_str, &row, &col)) {
            int r = -1, c = -1;
            if (sscanf(coord_str, "%d %d", &r, &c) == 2 || sscanf(coord_str, "%d,%d", &r, &c) == 2) {
                printf("[!] Coordinates (%d, %d) out of bounds! Valid range is [0..14].\n", r, c);
            } else {
                printf("[!] Invalid coordinates '%s'. Usage: move <row> <col> (e.g. 'move 7 7' or 'move H8')\n", coord_str);
            }
            return 0;
        }

        if (row < 0 || row >= CARO_BOARD_SIZE || col < 0 || col >= CARO_BOARD_SIZE) {
            printf("[!] Coordinates (%d, %d) out of bounds! Valid range is [0..14].\n", row, col);
            return 0;
        }

        if (ctx->board[row][col] != CELL_EMPTY) {
            printf("[!] Cell (%d, %d) is already occupied! Please choose an empty cell.\n", row, col);
            return 0;
        }

        char payload[64];
        snprintf(payload, sizeof(payload), "MOVE\t%u\t%u\t%d\t%d\n",
                 ctx->current_game_id, ctx->expected_seq, row, col);
        if (client_send_msg(ctx->sock_fd, payload) == 0) {
            printf("[*] Move sent: (%d, %d) [Seq #%u] -> Waiting for server confirmation...\n",
                   row, col, ctx->expected_seq);
        } else {
            fprintf(stderr, "[-] Failed to send move.\n");
        }
        return 0;
    }

    printf("[!] Unknown command: '%s'. Type 'help' for available commands.\n", cmd);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Interactive Event Loop with poll() Multiplexing                           */
/* ------------------------------------------------------------------------- */

static int client_run_interactive_loop(const char *host, int port) {
    printf("======================================================================\n");
    printf("                CARO ONLINE CLI CLIENT (C11 / Linux)                  \n");
    printf("======================================================================\n");
    printf("[*] Connecting to server at %s:%d...\n", host, port);

    int sock_fd = client_connect_tcp(host, port);
    if (sock_fd < 0) {
        return 1;
    }

    client_ctx_t ctx;
    client_ctx_init(&ctx, sock_fd);

    printf("[+] Connected successfully to server!\n");
    printf("[*] Type 'help' for available commands, 'quit' to exit.\n\n");
    client_print_prompt(&ctx);

    uint8_t rx_buf[PROTO_MAX_FRAME_BUFFER];
    size_t rx_len = 0;
    bool running = true;
    bool connection_lost = false;

#if !defined(_WIN32)
    char stdin_buf[2048];
    size_t stdin_len = 0;
#endif

    while (running && !g_stop_requested) {
#if !defined(_WIN32)
        /* Check if stdin_buf already has a complete line from a previous read */
        char *newline_pos = NULL;
        for (size_t i = 0; i < stdin_len; ++i) {
            if (stdin_buf[i] == '\n') {
                newline_pos = stdin_buf + i;
                break;
            }
        }

        if (newline_pos != NULL) {
            size_t line_len = (size_t)(newline_pos - stdin_buf);
            char line[1024];
            size_t copy_len = (line_len < sizeof(line) - 1) ? line_len : sizeof(line) - 1;
            memcpy(line, stdin_buf, copy_len);
            line[copy_len] = '\0';

            size_t consumed = line_len + 1;
            memmove(stdin_buf, stdin_buf + consumed, stdin_len - consumed);
            stdin_len -= consumed;

            client_dispatch_user_input(&ctx, line, &running);
            if (running) {
                client_print_prompt(&ctx);
            }
            continue;
        }

        /* POSIX: poll() monitors both stdin (fd 0) and socket_fd */
        client_pollfd_t pfds[2];
        pfds[0].fd = STDIN_FILENO;
        pfds[0].events = POLLIN;
        pfds[0].revents = 0;

        pfds[1].fd = sock_fd;
        pfds[1].events = POLLIN;
        pfds[1].revents = 0;

        int poll_res = client_poll(pfds, 2, 100);
        if (poll_res < 0) {
            if (errno == EINTR) continue;
            perror("[-] poll error");
            break;
        }

        /* 1. Check data from server */
        if (pfds[1].revents & (POLLIN | POLLHUP | POLLERR)) {
            ssize_t n = recv(sock_fd, rx_buf + rx_len, sizeof(rx_buf) - rx_len, 0);
            if (n > 0) {
                rx_len += (size_t)n;
                bool any_msg_displayed = false;
                while (running) {
                    char payload[PROTO_MAX_PAYLOAD_SIZE + 1];
                    size_t payload_len = 0;
                    size_t consumed = 0;

                    int status = protocol_unpack_frame(rx_buf, rx_len, payload,
                                                       sizeof(payload), &payload_len, &consumed);
                    if (status == PROTO_NEED_MORE_DATA) {
                        break;
                    }
                    if (status != PROTO_OK) {
                        fprintf(stderr, "\n[-] Protocol frame error from server: %d\n", status);
                        running = false;
                        break;
                    }

                    client_display_server_message(&ctx, payload, payload_len);
                    any_msg_displayed = true;

                    memmove(rx_buf, rx_buf + consumed, rx_len - consumed);
                    rx_len -= consumed;
                }

                if (running && any_msg_displayed) {
                    client_print_prompt(&ctx);
                }
            } else if (n == 0) {
                printf("\n[CONNECTION LOST] Server disconnected or connection lost.\n");
                connection_lost = true;
                running = false;
                break;
            } else {
                if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                    perror("\n[-] Socket read error");
                    printf("\n[CONNECTION LOST] Server disconnected or connection lost.\n");
                    connection_lost = true;
                    running = false;
                    break;
                }
            }
        }

        /* 2. Check user input from stdin */
        if (running && (pfds[0].revents & (POLLIN | POLLHUP))) {
            ssize_t n = read(STDIN_FILENO, stdin_buf + stdin_len, sizeof(stdin_buf) - stdin_len - 1);
            if (n > 0) {
                stdin_len += (size_t)n;
                stdin_buf[stdin_len] = '\0';
            } else if (n == 0) {
                if (stdin_len > 0) {
                    char line[1024];
                    size_t copy_len = (stdin_len < sizeof(line) - 1) ? stdin_len : sizeof(line) - 1;
                    memcpy(line, stdin_buf, copy_len);
                    line[copy_len] = '\0';
                    stdin_len = 0;
                    client_dispatch_user_input(&ctx, line, &running);
                }
                printf("\n[*] Stdin EOF detected. Exiting...\n");
                running = false;
                break;
            }
        }
#else
        /* Windows fallback */
        client_pollfd_t pfd;
        pfd.fd = sock_fd;
        pfd.events = POLLIN;
        pfd.revents = 0;

        int poll_res = client_poll(&pfd, 1, 50);
        if (poll_res > 0 && (pfd.revents & (POLLIN | POLLHUP | POLLERR))) {
            int n = recv((SOCKET)sock_fd, (char *)(rx_buf + rx_len),
                         (int)(sizeof(rx_buf) - rx_len), 0);
            if (n > 0) {
                rx_len += (size_t)n;
                bool any_msg_displayed = false;
                while (running) {
                    char payload[PROTO_MAX_PAYLOAD_SIZE + 1];
                    size_t payload_len = 0;
                    size_t consumed = 0;

                    int status = protocol_unpack_frame(rx_buf, rx_len, payload,
                                                       sizeof(payload), &payload_len, &consumed);
                    if (status == PROTO_NEED_MORE_DATA) break;
                    if (status != PROTO_OK) {
                        fprintf(stderr, "\n[-] Frame unpack error: %d\n", status);
                        running = false;
                        break;
                    }

                    client_display_server_message(&ctx, payload, payload_len);
                    any_msg_displayed = true;

                    memmove(rx_buf, rx_buf + consumed, rx_len - consumed);
                    rx_len -= consumed;
                }

                if (running && any_msg_displayed) {
                    client_print_prompt(&ctx);
                }
            } else if (n == 0) {
                printf("\n[CONNECTION LOST] Server disconnected or connection lost.\n");
                connection_lost = true;
                running = false;
                break;
            } else {
                int wsa_err = WSAGetLastError();
                if (wsa_err != WSAEWOULDBLOCK && wsa_err != WSAEINTR) {
                    printf("\n[CONNECTION LOST] Server disconnected or connection lost.\n");
                    connection_lost = true;
                    running = false;
                    break;
                }
            }
        }

        if (_kbhit()) {
            char line[1024];
            if (fgets(line, sizeof(line), stdin) != NULL) {
                client_dispatch_user_input(&ctx, line, &running);
                if (running) {
                    client_print_prompt(&ctx);
                }
            }
        }
#endif
    }

    printf("[*] Closing client socket...\n");
    socket_close(sock_fd);
    if (!connection_lost) {
        printf("[+] Client exited cleanly.\n");
    }
    return connection_lost ? 1 : 0;
}

/* ------------------------------------------------------------------------- */
/* Batch Ping Mode                                                           */
/* ------------------------------------------------------------------------- */

static int client_run_batch_ping(const char *host, int port, const char *ping_msg, int timeout_ms) {
    printf("[*] Connecting to %s:%d (Batch Mode)...\n", host, port);
    int sock_fd = client_connect_tcp(host, port);
    if (sock_fd < 0) {
        return 1;
    }

    char payload[PROTO_MAX_PAYLOAD_SIZE];
    protocol_create_ping(ping_msg, payload, sizeof(payload));

    printf("[*] Sending PING frame (echo='%s')...\n", ping_msg);
    if (client_send_msg(sock_fd, payload) < 0) {
        socket_close(sock_fd);
        return 1;
    }

    uint8_t rx_buf[PROTO_MAX_FRAME_BUFFER];
    size_t rx_len = 0;
    bool pong_received = false;
    int exit_code = 1;

    client_pollfd_t pfd;
    pfd.fd = sock_fd;
    pfd.events = POLLIN;
    pfd.revents = 0;

    int ret = client_poll(&pfd, 1, timeout_ms);
    if (ret <= 0) {
        fprintf(stderr, "[-] Timeout or error waiting for PONG from server\n");
        socket_close(sock_fd);
        return 1;
    }

    if (pfd.revents & POLLIN) {
#if defined(_WIN32)
        int n = recv((SOCKET)sock_fd, (char *)(rx_buf + rx_len),
                     (int)(sizeof(rx_buf) - rx_len), 0);
#else
        ssize_t n = recv(sock_fd, rx_buf + rx_len, sizeof(rx_buf) - rx_len, 0);
#endif
        if (n > 0) {
            rx_len += (size_t)n;

            char extracted[PROTO_MAX_PAYLOAD_SIZE + 1];
            size_t ext_len = 0;
            size_t consumed = 0;

            int unpack_st = protocol_unpack_frame(rx_buf, rx_len, extracted,
                                                  sizeof(extracted), &ext_len, &consumed);
            if (unpack_st == PROTO_OK) {
                protocol_msg_t msg;
                if (protocol_parse_msg(extracted, ext_len, &msg) == PROTO_OK) {
                    if (msg.cmd == CMD_PONG) {
                        const char *echo = (msg.token_count > 1) ? msg.tokens[1] : "";
                        printf("[+] PONG received from server: '%s'\n", echo);
                        if (strcmp(echo, ping_msg) == 0) {
                            printf("[+] Verified: PONG echo matches PING request successfully!\n");
                            pong_received = true;
                            exit_code = 0;
                        } else {
                            fprintf(stderr, "[-] Echo mismatch: expected '%s', got '%s'\n", ping_msg, echo);
                        }
                    } else if (msg.cmd == CMD_ERR) {
                        fprintf(stderr, "[-] Server returned ERR response\n");
                    }
                }
            } else {
                fprintf(stderr, "[-] Failed to unpack frame from server (code %d)\n", unpack_st);
            }
        } else {
            fprintf(stderr, "[-] Connection closed by server\n");
            printf("\n[CONNECTION LOST] Server disconnected or connection lost.\n");
        }
    }

    socket_close(sock_fd);
    return pong_received ? exit_code : 1;
}

/* ------------------------------------------------------------------------- */
/* Expanded Client Unit / Self-Test Suite                                    */
/* ------------------------------------------------------------------------- */

static int client_run_self_test(void) {
    printf("======================================================================\n");
    printf("       CARO CLIENT EXPANDED SELF-TEST SUITE (Feature F21)             \n");
    printf("======================================================================\n");

    /* Test 1: PING packing helper */
    char ping_payload[PROTO_MAX_PAYLOAD_SIZE];
    int res = protocol_create_ping("selftest_123", ping_payload, sizeof(ping_payload));
    if (res != PROTO_OK || strcmp(ping_payload, "PING\tselftest_123\n") != 0) {
        fprintf(stderr, "[-] Test 1 failed: protocol_create_ping\n");
        return 1;
    }
    printf("[+] Test 1 passed: protocol_create_ping produces correct wire string\n");

    /* Test 2: Frame packing */
    uint8_t frame_buf[PROTO_MAX_FRAME_BUFFER];
    size_t frame_len = 0;
    res = protocol_pack_frame(ping_payload, strlen(ping_payload), frame_buf, sizeof(frame_buf), &frame_len);
    if (res != PROTO_OK || frame_len != PROTO_HEADER_SIZE + strlen(ping_payload)) {
        fprintf(stderr, "[-] Test 2 failed: protocol_pack_frame\n");
        return 1;
    }
    printf("[+] Test 2 passed: protocol_pack_frame generates valid 4-byte header\n");

    /* Test 3: Frame unpacking */
    char extracted[PROTO_MAX_PAYLOAD_SIZE + 1];
    size_t ext_len = 0;
    size_t consumed = 0;
    res = protocol_unpack_frame(frame_buf, frame_len, extracted, sizeof(extracted), &ext_len, &consumed);
    if (res != PROTO_OK || consumed != frame_len || strcmp(extracted, ping_payload) != 0) {
        fprintf(stderr, "[-] Test 3 failed: protocol_unpack_frame\n");
        return 1;
    }
    printf("[+] Test 3 passed: protocol_unpack_frame extracts matching payload\n");

    /* Test 4: Parse PONG response */
    const char *pong_wire = "PONG\tselftest_123\n";
    protocol_msg_t msg;
    res = protocol_parse_msg(pong_wire, strlen(pong_wire), &msg);
    if (res != PROTO_OK || msg.cmd != CMD_PONG || msg.token_count < 2 || strcmp(msg.tokens[1], "selftest_123") != 0) {
        fprintf(stderr, "[-] Test 4 failed: protocol_parse_msg on PONG\n");
        return 1;
    }
    printf("[+] Test 4 passed: protocol_parse_msg correctly decodes CMD_PONG\n");

    /* Test 5: Parse 3-token and 4-token ERR response */
    const char *err3_wire = "ERR\t401\tNot your turn\n";
    res = protocol_parse_msg(err3_wire, strlen(err3_wire), &msg);
    if (res != PROTO_OK || msg.cmd != CMD_ERR || msg.token_count != 3 || strcmp(msg.tokens[1], "401") != 0) {
        fprintf(stderr, "[-] Test 5a failed: protocol_parse_msg on 3-token ERR\n");
        return 1;
    }
    const char *err4_wire = "ERR\t1001\t201\tAuth failed\n";
    res = protocol_parse_msg(err4_wire, strlen(err4_wire), &msg);
    if (res != PROTO_OK || msg.cmd != CMD_ERR || msg.token_count != 4 || msg.request_id != 1001 || strcmp(msg.tokens[2], "201") != 0) {
        fprintf(stderr, "[-] Test 5b failed: protocol_parse_msg on 4-token ERR\n");
        return 1;
    }
    printf("[+] Test 5 passed: protocol_parse_msg handles 3-token and 4-token ERR\n");

    /* Test 6: Parse MOVE validates coords and token count */
    const char *move_wire = "MOVE\t1\t5\t7\t8\n";
    res = protocol_parse_msg(move_wire, strlen(move_wire), &msg);
    if (res != PROTO_OK || msg.cmd != CMD_MOVE || msg.move_row != 7 || msg.move_col != 8) {
        fprintf(stderr, "[-] Test 6a failed: protocol_parse_msg on valid MOVE\n");
        return 1;
    }
    const char *bad_move = "MOVE\t1\t5\n";
    res = protocol_parse_msg(bad_move, strlen(bad_move), &msg);
    if (res == PROTO_OK) {
        fprintf(stderr, "[-] Test 6b failed: incomplete MOVE was not rejected\n");
        return 1;
    }
    printf("[+] Test 6 passed: protocol_parse_msg validates MOVE token count and coordinates\n");

    /* Test 7: Coordinate parsing unit tests (Feature F21) */
    int r = -1, c = -1;
    if (!client_parse_coordinates("7 7", &r, &c) || r != 7 || c != 7) {
        fprintf(stderr, "[-] Test 7a failed: client_parse_coordinates '7 7'\n");
        return 1;
    }
    if (!client_parse_coordinates("7,8", &r, &c) || r != 7 || c != 8) {
        fprintf(stderr, "[-] Test 7b failed: client_parse_coordinates '7,8'\n");
        return 1;
    }
    if (!client_parse_coordinates("H8", &r, &c) || r != 7 || c != 7) {
        fprintf(stderr, "[-] Test 7c failed: client_parse_coordinates 'H8'\n");
        return 1;
    }
    if (!client_parse_coordinates("a1", &r, &c) || r != 0 || c != 0) {
        fprintf(stderr, "[-] Test 7d failed: client_parse_coordinates 'a1'\n");
        return 1;
    }
    if (!client_parse_coordinates("O15", &r, &c) || r != 14 || c != 14) {
        fprintf(stderr, "[-] Test 7e failed: client_parse_coordinates 'O15'\n");
        return 1;
    }
    if (client_parse_coordinates("15 0", &r, &c)) {
        fprintf(stderr, "[-] Test 7f failed: client_parse_coordinates '15 0' should fail\n");
        return 1;
    }
    if (client_parse_coordinates("-1 5", &r, &c)) {
        fprintf(stderr, "[-] Test 7g failed: client_parse_coordinates '-1 5' should fail\n");
        return 1;
    }
    if (client_parse_coordinates("P1", &r, &c)) {
        fprintf(stderr, "[-] Test 7h failed: client_parse_coordinates 'P1' should fail\n");
        return 1;
    }
    if (client_parse_coordinates("invalid_coord", &r, &c)) {
        fprintf(stderr, "[-] Test 7i failed: client_parse_coordinates 'invalid_coord' should fail\n");
        return 1;
    }
    if (client_parse_coordinates("A0", &r, &c)) {
        fprintf(stderr, "[-] Test 7j failed: client_parse_coordinates 'A0' should fail (A0 aliasing)\n");
        return 1;
    }
    if (client_parse_coordinates("7 7abc", &r, &c)) {
        fprintf(stderr, "[-] Test 7k failed: client_parse_coordinates '7 7abc' should fail (trailing chars)\n");
        return 1;
    }
    if (client_parse_coordinates("7,8xyz", &r, &c)) {
        fprintf(stderr, "[-] Test 7l failed: client_parse_coordinates '7,8xyz' should fail (trailing chars)\n");
        return 1;
    }
    if (client_parse_coordinates("7 7 9", &r, &c)) {
        fprintf(stderr, "[-] Test 7m failed: client_parse_coordinates '7 7 9' should fail (trailing token)\n");
        return 1;
    }
    printf("[+] Test 7 passed: client_parse_coordinates correctly parses numeric & Gomoku coords\n");

    /* Test 8: Board reset & move application tests (Feature F21) */
    client_ctx_t test_ctx;
    client_ctx_init(&test_ctx, -1);
    if (test_ctx.board[7][7] != CELL_EMPTY || test_ctx.last_row != -1 || test_ctx.last_col != -1) {
        fprintf(stderr, "[-] Test 8a failed: client_ctx_init did not clear board\n");
        return 1;
    }
    if (client_board_apply_move(&test_ctx, 1, CELL_X, 7, 7) != 0 ||
        test_ctx.board[7][7] != CELL_X || test_ctx.last_row != 7 || test_ctx.last_col != 7 ||
        test_ctx.last_stone != CELL_X || test_ctx.last_seq != 1) {
        fprintf(stderr, "[-] Test 8b failed: client_board_apply_move first move\n");
        return 1;
    }
    if (client_board_apply_move(&test_ctx, 2, CELL_O, 7, 8) != 0 ||
        test_ctx.board[7][8] != CELL_O || test_ctx.last_row != 7 || test_ctx.last_col != 8 ||
        test_ctx.last_stone != CELL_O || test_ctx.last_seq != 2) {
        fprintf(stderr, "[-] Test 8c failed: client_board_apply_move second move\n");
        return 1;
    }
    if (client_board_apply_move(&test_ctx, 3, CELL_X, 15, 15) == 0) {
        fprintf(stderr, "[-] Test 8d failed: out of bounds move not rejected\n");
        return 1;
    }
    client_board_reset(&test_ctx);
    if (test_ctx.board[7][7] != CELL_EMPTY || test_ctx.board[7][8] != CELL_EMPTY ||
        test_ctx.last_row != -1 || test_ctx.last_col != -1) {
        fprintf(stderr, "[-] Test 8e failed: client_board_reset did not reset board\n");
        return 1;
    }
    printf("[+] Test 8 passed: client_board_apply_move and client_board_reset maintain board state\n");

    /* Test 9: State machine transitions (Feature F21) */
    test_ctx.state = CLI_STATE_UNAUTHENTICATED;
    test_ctx.logged_in = false;
    if (test_ctx.state != CLI_STATE_UNAUTHENTICATED) {
        fprintf(stderr, "[-] Test 9a failed: state initialization\n");
        return 1;
    }
    test_ctx.state = CLI_STATE_IN_LOBBY;
    test_ctx.logged_in = true;
    snprintf(test_ctx.username, sizeof(test_ctx.username), "test_user");
    if (test_ctx.state != CLI_STATE_IN_LOBBY || !test_ctx.logged_in) {
        fprintf(stderr, "[-] Test 9b failed: lobby transition\n");
        return 1;
    }
    test_ctx.state = CLI_STATE_IN_INVITE;
    test_ctx.pending_invite_id = 42;
    if (test_ctx.state != CLI_STATE_IN_INVITE || test_ctx.pending_invite_id != 42) {
        fprintf(stderr, "[-] Test 9c failed: invite transition\n");
        return 1;
    }
    test_ctx.state = CLI_STATE_IN_GAME;
    test_ctx.current_game_id = 99;
    test_ctx.my_stone = CELL_X;
    test_ctx.opp_stone = CELL_O;
    test_ctx.current_turn = CELL_X;
    test_ctx.expected_seq = 1;
    if (test_ctx.state != CLI_STATE_IN_GAME || test_ctx.current_game_id != 99 ||
        test_ctx.my_stone != CELL_X || test_ctx.expected_seq != 1) {
        fprintf(stderr, "[-] Test 9d failed: in-game transition\n");
        return 1;
    }
    test_ctx.state = CLI_STATE_IN_LOBBY;
    test_ctx.current_game_id = 0;
    if (test_ctx.state != CLI_STATE_IN_LOBBY || test_ctx.current_game_id != 0) {
        fprintf(stderr, "[-] Test 9e failed: lobby return transition\n");
        return 1;
    }
    printf("[+] Test 9 passed: client state machine transitions cleanly through all phases\n");

    printf("======================================================================\n");
    printf("[+] All client self-tests passed (9/9)\n");
    printf("======================================================================\n");
    return 0;
}

static void print_usage(const char *prog_name) {
    printf("Caro CLI Client (C11 / Linux)\n");
    printf("Usage:\n");
    printf("  %s [-h host] [-p port]                 : Run interactive CLI client with poll()\n", prog_name);
    printf("  %s [-h host] [-p port] --ping <msg>    : Batch mode: send PING <msg> and wait for PONG\n", prog_name);
    printf("  %s --test                              : Run client internal self-tests and exit\n", prog_name);
    printf("  %s --help                              : Display this usage help\n", prog_name);
}

int main(int argc, char *argv[]) {
    const char *host = "127.0.0.1";
    int port = 8888;
    bool self_test = false;
    const char *batch_ping_msg = NULL;

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-h") == 0 && i + 1 < argc) {
            host = argv[++i];
        } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--ping") == 0 && i + 1 < argc) {
            batch_ping_msg = argv[++i];
        } else if (strcmp(argv[i], "--test") == 0) {
            self_test = true;
        } else if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
    }

    if (client_net_init() < 0) {
        return 1;
    }

    int ret = 0;
    if (self_test) {
        ret = client_run_self_test();
    } else if (batch_ping_msg != NULL) {
        ret = client_run_batch_ping(host, port, batch_ping_msg, 5000);
    } else {
        ret = client_run_interactive_loop(host, port);
    }

    client_net_cleanup();
    return ret;
}
