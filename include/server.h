#ifndef SERVER_H
#define SERVER_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <pthread.h>

#if defined(_WIN32)
    #include <winsock2.h>
    #include <ws2tcpip.h>
    typedef int socklen_t;
    typedef ULONG nfds_t;
    #define socket_close closesocket
    typedef WSAPOLLFD caro_pollfd_t;
    #define caro_poll(fds, nfds, timeout) WSAPoll((fds), (nfds), (timeout))
#else
    #include <sys/types.h>
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <poll.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <errno.h>
    #define socket_close close
    typedef struct pollfd caro_pollfd_t;
    #define caro_poll(fds, nfds, timeout) poll((fds), (nfds), (timeout))
#endif

#include "protocol.h"
#include "game.h"
#include "storage.h"
#include "auth.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Default Server Configuration */
#define DEFAULT_SERVER_PORT      8888
#define DEFAULT_SERVER_HOST      "0.0.0.0"
#define SERVER_MAX_CLIENTS       64
#define SERVER_LISTEN_BACKLOG    16
#define SERVER_RX_BUFFER_SIZE    PROTO_MAX_FRAME_BUFFER
#define SERVER_TX_BUFFER_SIZE    PROTO_MAX_FRAME_BUFFER

/* Resource & Work Limits (R1) */
#define SERVER_READ_BUDGET_BYTES    32768
#define SERVER_FRAME_BUDGET         16
#define SERVER_FRAME_TIMEOUT_SEC    10

/* Event Loop & Queue Limits (R2) */
#define SERVER_MAX_POLL_FDS         (SERVER_MAX_CLIENTS + 2) /* index 0=listener, 1=db_pipe, 2..65=clients */
#define DB_QUEUE_CAPACITY           64
#define AUTH_MAX_FAILED_ATTEMPTS    5
#define AUTH_LOCKOUT_DURATION_SEC   30
#define MAX_AUTH_THROTTLE_ENTRIES   64

/* Client connection states */
typedef enum {
    STATE_DISCONNECTED     = 0,
    STATE_UNAUTHENTICATED  = 1,
    STATE_AUTHENTICATED    = 2,
    STATE_IN_LOBBY         = 3,
    STATE_IN_INVITE        = 4,
    STATE_IN_GAME          = 5
} client_state_t;

/* Backwards compatibility aliases */
#define CLIENT_STATE_DISCONNECTED   STATE_DISCONNECTED
#define CLIENT_STATE_CONNECTED      STATE_UNAUTHENTICATED
#define CLIENT_STATE_GUEST          STATE_UNAUTHENTICATED
#define CLIENT_STATE_AUTHENTICATED  STATE_AUTHENTICATED
#define CLIENT_STATE_IN_LOBBY       STATE_IN_LOBBY
#define CLIENT_STATE_IN_INVITE      STATE_IN_INVITE
#define CLIENT_STATE_IN_GAME        STATE_IN_GAME

/* Challenge invite constants and structures (F15) */
#define SERVER_MAX_INVITES          32
#define DEFAULT_INVITE_TIMEOUT_SEC  30

typedef struct {
    uint32_t        invite_id;          /* Monotonic ID (>0) */
    uint32_t        req_id;             /* Challenger request_id for ERR 302 */
    int             challenger_idx;     /* Slot in srv->clients */
    uint32_t        challenger_gen;     /* Generation counter of challenger */
    int             target_idx;         /* Slot of target in srv->clients */
    uint32_t        target_gen;         /* Generation counter of target */
    time_t          created_at;         /* When invite was created */
    time_t          expires_at;         /* When invite expires */
    int64_t         expires_at_ms;      /* Monotonic timestamp in ms for expiration */
    bool            active;             /* Slot currently in use */
} challenge_invite_t;

/* Per-client connection context */
typedef struct {
    int             fd;                         /* Socket file descriptor (-1 if empty slot) */
    uint32_t        generation;                 /* Monotonic generation counter to prevent stale async results */
    client_state_t  state;                      /* Connection state */
    char            ip_str[INET6_ADDRSTRLEN];   /* Client IP string */
    uint16_t        port;                       /* Client port */
    
    /* RX Buffer for accumulated framing stream */
    uint8_t         rx_buf[PROTO_MAX_FRAME_BUFFER];
    size_t          rx_len;                     /* Current unconsumed bytes in rx_buf */
    
    /* TX Buffer for outgoing framed data */
    uint8_t         tx_buf[PROTO_MAX_FRAME_BUFFER];
    size_t          tx_len;                     /* Current bytes queued in tx_buf */
    size_t          tx_offset;                  /* Current bytes already sent in tx_buf (partial write tracking) */

    /* Activity & Timeout */
    time_t          last_active_ts;             /* Last activity timestamp for frame timeout detection */

    /* User identity & session state */
    uint32_t        client_id;                  /* Server slot / session sequential ID */
    uint32_t        user_id;                    /* Authenticated account ID (0 if guest) */
    char            username[32];               /* Logged in username */
    bool            is_guest;                   /* True if guest session, false if registered account */
    uint32_t        current_game_id;            /* Active game ID (0 if none) */
    uint32_t        pending_invite_id;          /* Active invite ID if STATE_IN_INVITE (0 if none) */
} client_conn_t;

/* Database Worker Request / Response Types */
typedef enum {
    DB_REQ_REGISTER,
    DB_REQ_LOGIN,
    DB_REQ_SETTLE_GAME
} db_req_type_t;

typedef struct {
    uint32_t        req_id;
    int             client_idx;
    uint32_t        generation;
    db_req_type_t   type;
    char            username[32];
    char            password[64];
    /* settlement fields */
    uint32_t        game_id;
    uint32_t        winner_id;
    uint32_t        loser_id;
    bool            is_draw;
    char            outcome[16];
    char            reason[32];
} db_request_t;

typedef struct {
    uint32_t        req_id;
    int             client_idx;
    uint32_t        generation;
    db_req_type_t   type;
    int             status_code;    /* 0 on success, error code otherwise */
    char            error_msg[64];
    uint32_t        user_id;
    char            username[32];
    int             points;
    int             wins;
    int             draws;
    int             losses;
    uint32_t        game_id;        /* Game ID for settlement response */
} db_response_t;

/* Per-user authentication throttling entry */
typedef struct {
    char            username[32];
    int             failed_attempts;
    time_t          lockout_until;
    time_t          last_attempt_ts;
} auth_throttle_entry_t;

/* Active match management slot (F16-F20) */
typedef struct {
    game_t          game;               /* Pure Caro game engine instance */
    bool            active;             /* True if slot currently contains an ongoing match */
    bool            settled;            /* True if match has been concluded (idempotency flag) */
    int             player_x_idx;       /* Client slot index for Player X */
    int             player_o_idx;       /* Client slot index for Player O */
    uint32_t        player_x_gen;       /* Client generation for Player X */
    uint32_t        player_o_gen;       /* Client generation for Player O */
    caro_cell_t     draw_offered_by;    /* CELL_EMPTY, CELL_X, or CELL_O */
    time_t          start_time;         /* Match start timestamp */
} game_slot_t;

/* Server master structure */
typedef struct {
    int             listen_fd;
    char            host[64];
    int             port;
    char            db_path[256];
    volatile bool   running;

    /* Clients tracking */
    client_conn_t   clients[SERVER_MAX_CLIENTS];
    size_t          active_clients_count;

    /* Pollfd array: index 0 is listen_fd, 1 is db_pipe[0], 2..SERVER_MAX_CLIENTS+1 are clients */
    caro_pollfd_t   poll_fds[SERVER_MAX_POLL_FDS];
    nfds_t          poll_count;
    int             client_map[SERVER_MAX_POLL_FDS]; /* Maps poll_fds index -> clients[] index */

    /* Database Worker & Synchronization */
    pthread_t       db_worker_thread;
    bool            db_worker_started;
    volatile bool   db_worker_shutdown;
    pthread_mutex_t db_queue_mutex;
    pthread_cond_t  db_queue_cond;
    int             db_pipe[2]; /* Self-pipe for worker -> main thread notification */

    /* Request queue */
    db_request_t    db_req_queue[DB_QUEUE_CAPACITY];
    int             db_req_head;
    int             db_req_tail;
    int             db_req_count;

    /* Response queue */
    db_response_t   db_resp_queue[DB_QUEUE_CAPACITY];
    int             db_resp_head;
    int             db_resp_tail;
    int             db_resp_count;

    /* Auth throttle table */
    auth_throttle_entry_t throttle_table[MAX_AUTH_THROTTLE_ENTRIES];

    /* Challenge invites tracking (F15) */
    challenge_invite_t  invites[SERVER_MAX_INVITES];
    uint32_t            next_invite_id;
    int                 invite_timeout_sec;
    int64_t             invite_timeout_ms;

    /* Match tracking (F16-F20) */
    game_slot_t         games[CARO_MAX_GAMES];
    size_t              active_games_count;
    uint32_t            next_game_id;
} server_t;

/**
 * @brief Initialize server structure, bind to host:port, open storage, and start DB worker.
 * 
 * @param srv Pointer to server_t instance
 * @param host Bind address (e.g., "0.0.0.0")
 * @param port Port number (e.g., 8888)
 * @param db_path Database path (if NULL or empty, defaults to CARO_DB_PATH or "caro.db")
 * @return 0 on success, negative error code on failure.
 */
int server_init(server_t *srv, const char *host, int port, const char *db_path);

/**
 * @brief Set socket fd to non-blocking mode.
 * 
 * @param fd Socket file descriptor
 * @return 0 on success, -1 on error.
 */
int server_set_nonblocking(int fd);

/**
 * @brief Start main poll() event loop and serve connections.
 * 
 * @param srv Pointer to initialized server_t
 * @return 0 on clean exit, negative error code on failure.
 */
int server_run(server_t *srv);

/**
 * @brief Request server to stop running.
 * 
 * @param srv Pointer to server_t instance
 */
void server_stop(server_t *srv);

/**
 * @brief Close all open client sockets, stop worker thread, close DB and self-pipe.
 * 
 * @param srv Pointer to server_t instance
 */
void server_cleanup(server_t *srv);

/**
 * @brief Safely close client socket, release resources, advance generation, and clear slot.
 * 
 * @param srv Pointer to server_t instance
 * @param idx Index of client in srv->clients array (0 <= idx < SERVER_MAX_CLIENTS)
 * @param reason Human-readable reason for disconnection (for logging)
 */
void server_disconnect_client(server_t *srv, int idx, const char *reason);

/**
 * @brief Queue framed data into client's tx_buf.
 * 
 * @param srv Pointer to server_t instance
 * @param idx Client index
 * @param data Byte pointer of framed data to send
 * @param len Length in bytes
 * @return 0 on success, negative on buffer overflow or invalid client.
 */
int server_queue_send(server_t *srv, int idx, const uint8_t *data, size_t len);

/**
 * @brief Send standard ERR message to client with framing.
 */
int server_send_err(server_t *srv, int idx, int err_code, const char *reason);

/**
 * @brief Send ERR message with request_id reference: "ERR\t<ref_id>\t<code>\t<reason>\n"
 */
int server_send_err_ref(server_t *srv, int idx, uint32_t ref_id, int err_code, const char *reason);

/**
 * @brief Send OK message with request_id and optional extra fields: "OK\t<req_id>\t<extra>\n"
 */
int server_send_ok(server_t *srv, int idx, uint32_t req_id, const char *extra_data);

/**
 * @brief Enqueue a DB request to the async worker queue. Rejects immediately if full.
 */
int server_queue_db_request(server_t *srv, const db_request_t *req);

/**
 * @brief Drain DB worker response notifications from self-pipe and dispatch completed responses.
 */
void server_handle_db_responses(server_t *srv);

/**
 * @brief Handle POLLIN event on client socket (read non-blocking and unpack frames).
 */
int server_handle_client_read(server_t *srv, int idx);

/**
 * @brief Process pending frames in client's user-space rx_buf up to SERVER_FRAME_BUDGET.
 */
int server_process_pending_frames(server_t *srv, int idx);

/**
 * @brief Handle POLLOUT event on client socket (send queued tx_buf data non-blocking).
 */
int server_handle_client_write(server_t *srv, int idx);

/**
 * @brief Dispatch unpacked payload message for a client.
 */
int server_handle_client_payload(server_t *srv, int idx, const char *payload, size_t len);

/**
 * @brief Accept pending incoming TCP connections on listener socket non-blocking.
 */
int server_accept_new_client(server_t *srv);

/**
 * @brief Broadcast CMD_ONLINE_LIST to all active authenticated and lobby clients.
 */
int server_broadcast_online_list(server_t *srv);

/* Challenge & Invitation Management (F15) */
int challenge_create(server_t *srv, int challenger_idx, const char *target_name, uint32_t req_id);
int challenge_respond(server_t *srv, int responder_idx, uint32_t invite_id, bool accept);
void challenge_check_timeouts(server_t *srv);
void challenge_handle_disconnect(server_t *srv, int client_idx);

/* Game Management Functions (F16-F20) */
game_slot_t *server_find_game(server_t *srv, uint32_t game_id);
int server_find_free_game_slot(server_t *srv);
int game_manager_start(server_t *srv, int challenger_idx, int target_idx);
int game_manager_handle_move(server_t *srv, int client_idx, uint32_t game_id, uint32_t seq, int row, int col);
int game_manager_conclude(server_t *srv, uint32_t game_id, const char *outcome, const char *reason,
                          int winner_idx, int loser_idx, bool is_draw);
void game_manager_handle_disconnect(server_t *srv, int disconnected_idx);

/* Per-game structured logging (F20) */
int game_log_init(uint32_t game_id, const char *pX_name, uint32_t pX_id, const char *pO_name, uint32_t pO_id);
int game_log_move(uint32_t game_id, uint32_t seq, const char *stone, uint32_t user_id, const char *username, int row, int col);
int game_log_conclude(uint32_t game_id, const char *outcome, const char *reason, uint32_t winner_id, const char *winner_name, uint32_t total_moves);

#ifdef __cplusplus
}
#endif

#endif /* SERVER_H */
