#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <strings.h>
#include "server.h"

#if defined(_WIN32)
    #include <winsock2.h>
    #include <ws2tcpip.h>
#else
    #include <unistd.h>
    #include <fcntl.h>
    #include <errno.h>
    #include <arpa/inet.h>
    #include <sys/socket.h>
    #include <poll.h>
#endif

#include <sys/stat.h>
#if defined(_WIN32)
    #include <direct.h>
    #define caro_mkdir(p) _mkdir(p)
#else
    #define caro_mkdir(p) mkdir(p, 0755)
#endif

static int64_t current_time_ms(void) {
#if defined(_WIN32)
    return (int64_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + (ts.tv_nsec / 1000000);
#endif
}

/* ------------------------------------------------------------------------- */
/* Pipe Helpers (POSIX pipe on Linux, Loopback TCP Pair on Windows)          */
/* ------------------------------------------------------------------------- */
static int caro_pipe_create(int fds[2]) {
#if defined(_WIN32)
    SOCKET listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener == INVALID_SOCKET) return -1;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        closesocket(listener);
        return -1;
    }
    int len = sizeof(addr);
    if (getsockname(listener, (struct sockaddr *)&addr, &len) != 0) {
        closesocket(listener);
        return -1;
    }
    if (listen(listener, 1) != 0) {
        closesocket(listener);
        return -1;
    }
    SOCKET write_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (write_sock == INVALID_SOCKET) {
        closesocket(listener);
        return -1;
    }
    if (connect(write_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        closesocket(write_sock);
        closesocket(listener);
        return -1;
    }
    SOCKET read_sock = accept(listener, NULL, NULL);
    closesocket(listener);
    if (read_sock == INVALID_SOCKET) {
        closesocket(write_sock);
        return -1;
    }
    u_long mode = 1;
    ioctlsocket(read_sock, FIONBIO, &mode);
    fds[0] = (int)read_sock;
    fds[1] = (int)write_sock;
    return 0;
#else
    if (pipe(fds) != 0) return -1;
    int flags = fcntl(fds[0], F_GETFL, 0);
    if (flags != -1) {
        fcntl(fds[0], F_SETFL, flags | O_NONBLOCK);
    }
    return 0;
#endif
}

static void caro_pipe_notify(int fd) {
    uint8_t tok = 1;
#if defined(_WIN32)
    send((SOCKET)fd, (const char *)&tok, 1, 0);
#else
    ssize_t res = write(fd, &tok, 1);
    (void)res;
#endif
}

static void caro_pipe_drain(int fd) {
    uint8_t buf[128];
#if defined(_WIN32)
    while (recv((SOCKET)fd, (char *)buf, sizeof(buf), 0) > 0);
#else
    while (read(fd, buf, sizeof(buf)) > 0);
#endif
}

static void caro_pipe_close(int fds[2]) {
    for (int i = 0; i < 2; ++i) {
        if (fds[i] >= 0) {
#if defined(_WIN32)
            closesocket((SOCKET)fds[i]);
#else
            close(fds[i]);
#endif
            fds[i] = -1;
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Auth Throttle Table Management                                            */
/* ------------------------------------------------------------------------- */
static auth_throttle_entry_t *server_get_throttle_entry(server_t *srv, const char *username) {
    if (!srv || !username || !*username) return NULL;
    for (int i = 0; i < MAX_AUTH_THROTTLE_ENTRIES; ++i) {
        if (srv->throttle_table[i].username[0] != '\0' &&
            strcasecmp(srv->throttle_table[i].username, username) == 0) {
            return &srv->throttle_table[i];
        }
    }
    return NULL;
}

static auth_throttle_entry_t *server_get_or_create_throttle_entry(server_t *srv, const char *username) {
    auth_throttle_entry_t *entry = server_get_throttle_entry(srv, username);
    if (entry) return entry;

    int free_idx = -1;
    time_t oldest_ts = time(NULL);
    int oldest_idx = 0;

    for (int i = 0; i < MAX_AUTH_THROTTLE_ENTRIES; ++i) {
        if (srv->throttle_table[i].username[0] == '\0') {
            free_idx = i;
            break;
        }
        if (srv->throttle_table[i].last_attempt_ts < oldest_ts) {
            oldest_ts = srv->throttle_table[i].last_attempt_ts;
            oldest_idx = i;
        }
    }

    int target_idx = (free_idx >= 0) ? free_idx : oldest_idx;
    entry = &srv->throttle_table[target_idx];
    memset(entry, 0, sizeof(*entry));
    snprintf(entry->username, sizeof(entry->username), "%s", username);
    return entry;
}

/* ------------------------------------------------------------------------- */
/* Background Async DB Worker Thread                                         */
/* ------------------------------------------------------------------------- */
static void *db_worker_thread_func(void *arg) {
    server_t *srv = (server_t *)arg;
    if (!srv) return NULL;

    while (1) {
        pthread_mutex_lock(&srv->db_queue_mutex);
        while (!srv->db_worker_shutdown && srv->db_req_count == 0) {
            pthread_cond_wait(&srv->db_queue_cond, &srv->db_queue_mutex);
        }

        if (srv->db_worker_shutdown && srv->db_req_count == 0) {
            pthread_mutex_unlock(&srv->db_queue_mutex);
            break;
        }

        /* Pop request from bounded circular queue */
        db_request_t req = srv->db_req_queue[srv->db_req_head];
        srv->db_req_head = (srv->db_req_head + 1) % DB_QUEUE_CAPACITY;
        srv->db_req_count--;
        pthread_mutex_unlock(&srv->db_queue_mutex);

        /* Process request outside lock: main thread never blocked */
        db_response_t resp;
        memset(&resp, 0, sizeof(resp));
        resp.req_id = req.req_id;
        resp.client_idx = req.client_idx;
        resp.generation = req.generation;
        resp.type = req.type;
        resp.game_id = req.game_id;
        resp.status_code = -1;

        if (req.type == DB_REQ_REGISTER) {
            char salt[AUTH_SALT_STR_SIZE];
            char hash[AUTH_HASH_STR_SIZE];
            int salt_rc = auth_generate_salt(salt, sizeof(salt));
            int hash_rc = auth_hash_password(req.password, salt, hash, sizeof(hash));
            if (salt_rc != AUTH_OK || hash_rc != AUTH_OK) {
                resp.status_code = -1;
                snprintf(resp.error_msg, sizeof(resp.error_msg), "Cryptographic error");
            } else {
                uint32_t new_uid = 0;
                int db_rc = storage_register_user(req.username, salt, hash, &new_uid);
                resp.status_code = db_rc;
                snprintf(resp.username, sizeof(resp.username), "%s", req.username);
                if (db_rc == STORAGE_OK) {
                    resp.user_id = new_uid;
                } else if (db_rc == STORAGE_ERR_EXISTS) {
                    snprintf(resp.error_msg, sizeof(resp.error_msg), "Username already exists");
                } else {
                    snprintf(resp.error_msg, sizeof(resp.error_msg), "Registration database error");
                }
            }
        } else if (req.type == DB_REQ_LOGIN) {
            uint32_t uid = 0;
            char db_salt[AUTH_SALT_STR_SIZE];
            char db_hash[AUTH_HASH_STR_SIZE];
            int db_rc = storage_get_user_auth(req.username, &uid, db_salt, sizeof(db_salt),
                                              db_hash, sizeof(db_hash));
            snprintf(resp.username, sizeof(resp.username), "%s", req.username);
            if (db_rc != STORAGE_OK) {
                resp.status_code = (db_rc == STORAGE_ERR_NOTFOUND) ? -2 : -1;
                snprintf(resp.error_msg, sizeof(resp.error_msg), "Authentication failed");
            } else {
                bool match = auth_verify_password(req.password, db_salt, db_hash);
                if (!match) {
                    resp.status_code = -3;
                    snprintf(resp.error_msg, sizeof(resp.error_msg), "Invalid password");
                } else {
                    int points = 0, wins = 0, draws = 0, losses = 0;
                    storage_get_user_stats(uid, &points, &wins, &draws, &losses);
                    resp.status_code = 0;
                    resp.user_id = uid;
                    resp.points = points;
                    resp.wins = wins;
                    resp.draws = draws;
                    resp.losses = losses;
                }
            }
        } else if (req.type == DB_REQ_SETTLE_GAME) {
            int rc = storage_settle_game(req.game_id, req.outcome, req.reason,
                                         req.winner_id, req.loser_id, req.is_draw);
            resp.status_code = rc;
        }

        /* Push response and notify main loop via self-pipe */
        pthread_mutex_lock(&srv->db_queue_mutex);
        if (srv->db_resp_count < DB_QUEUE_CAPACITY) {
            srv->db_resp_queue[srv->db_resp_tail] = resp;
            srv->db_resp_tail = (srv->db_resp_tail + 1) % DB_QUEUE_CAPACITY;
            srv->db_resp_count++;
            caro_pipe_notify(srv->db_pipe[1]);
        } else {
            fprintf(stderr, "[-] DB response queue full; response dropped\n");
        }
        pthread_mutex_unlock(&srv->db_queue_mutex);
    }

    return NULL;
}

int server_queue_db_request(server_t *srv, const db_request_t *req) {
    if (!srv || !req) return -1;

    pthread_mutex_lock(&srv->db_queue_mutex);
    if (srv->db_req_count >= DB_QUEUE_CAPACITY) {
        pthread_mutex_unlock(&srv->db_queue_mutex);
        /* Reject on full: immediate non-blocking 500 error */
        server_send_err_ref(srv, req->client_idx, req->req_id, ERR_INTERNAL_SERVER, "Server busy, please retry");
        return -1;
    }

    srv->db_req_queue[srv->db_req_tail] = *req;
    srv->db_req_tail = (srv->db_req_tail + 1) % DB_QUEUE_CAPACITY;
    srv->db_req_count++;
    pthread_cond_signal(&srv->db_queue_cond);
    pthread_mutex_unlock(&srv->db_queue_mutex);
    return 0;
}

void server_handle_db_responses(server_t *srv) {
    if (!srv) return;

    /* Drain notification tokens from self-pipe */
    caro_pipe_drain(srv->db_pipe[0]);

    /* Drain completed responses into stack buffer under mutex */
    db_response_t batch[DB_QUEUE_CAPACITY];
    int batch_count = 0;

    pthread_mutex_lock(&srv->db_queue_mutex);
    while (srv->db_resp_count > 0 && batch_count < DB_QUEUE_CAPACITY) {
        batch[batch_count++] = srv->db_resp_queue[srv->db_resp_head];
        srv->db_resp_head = (srv->db_resp_head + 1) % DB_QUEUE_CAPACITY;
        srv->db_resp_count--;
    }
    pthread_mutex_unlock(&srv->db_queue_mutex);

    /* Process batch on main event loop thread */
    for (int b = 0; b < batch_count; ++b) {
        const db_response_t *resp = &batch[b];

        /* Asynchronous game settlement completion */
        if (resp->type == DB_REQ_SETTLE_GAME) {
            printf("[DB] Settlement committed for game %u (status=%d)\n",
                   resp->game_id, resp->status_code);
            continue;
        }

        int idx = resp->client_idx;
        if (idx < 0 || idx >= SERVER_MAX_CLIENTS) continue;

        client_conn_t *c = &srv->clients[idx];

        /* Stale result check: verify slot active and generation matches */
        if (c->fd < 0 || c->state == STATE_DISCONNECTED) {
            printf("[*] Stale DB response discarded (client %d already disconnected)\n", idx);
            continue;
        }
        if (c->generation != resp->generation) {
            printf("[*] Stale DB response discarded (slot %d generation %u != resp generation %u)\n",
                   idx, c->generation, resp->generation);
            continue;
        }

        if (resp->type == DB_REQ_REGISTER) {
            if (resp->status_code == STORAGE_OK) {
                char extra[64];
                snprintf(extra, sizeof(extra), "%u\t%s", resp->user_id, resp->username);
                server_send_ok(srv, idx, resp->req_id, extra);
            } else if (resp->status_code == STORAGE_ERR_EXISTS) {
                server_send_err_ref(srv, idx, resp->req_id, ERR_USER_EXISTS, "Username already exists");
            } else {
                server_send_err_ref(srv, idx, resp->req_id, ERR_DATABASE, "Registration failed");
            }
        } else if (resp->type == DB_REQ_LOGIN) {
            auth_throttle_entry_t *th = server_get_or_create_throttle_entry(srv, resp->username);

            if (resp->status_code != 0) {
                /* Login failed: increment failure counter */
                time_t now = time(NULL);
                th->failed_attempts++;
                th->last_attempt_ts = now;
                if (th->failed_attempts >= AUTH_MAX_FAILED_ATTEMPTS) {
                    th->lockout_until = now + AUTH_LOCKOUT_DURATION_SEC;
                    server_send_err_ref(srv, idx, resp->req_id, ERR_AUTH_FAILED,
                                        "Account locked out: 5 consecutive failed attempts. Try again in 30s");
                } else {
                    server_send_err_ref(srv, idx, resp->req_id, ERR_AUTH_FAILED,
                                        "Authentication failed; invalid username or password");
                }
                continue;
            }

            /* Single active session check */
            bool already_logged_in = false;
            for (int i = 0; i < SERVER_MAX_CLIENTS; ++i) {
                if (i != idx && srv->clients[i].fd >= 0 &&
                    (srv->clients[i].state == STATE_AUTHENTICATED ||
                     srv->clients[i].state == STATE_IN_LOBBY ||
                     srv->clients[i].state == STATE_IN_INVITE ||
                     srv->clients[i].state == STATE_IN_GAME)) {
                    if (srv->clients[i].user_id == resp->user_id ||
                        strcasecmp(srv->clients[i].username, resp->username) == 0) {
                        already_logged_in = true;
                        break;
                    }
                }
            }

            if (already_logged_in) {
                server_send_err_ref(srv, idx, resp->req_id, ERR_ALREADY_LOGGED_IN,
                                    "Account already logged in");
                continue;
            }

            /* Login successful: reset throttling counters */
            th->failed_attempts = 0;
            th->lockout_until = 0;

            /* Update client session state */
            c->user_id = resp->user_id;
            snprintf(c->username, sizeof(c->username), "%s", resp->username);
            c->is_guest = false;
            c->state = STATE_IN_LOBBY;

            printf("[PRESENCE] User '%s' (user_id=%u) authenticated on slot %d\n",
                   c->username, c->user_id, idx);

            char extra[128];
            snprintf(extra, sizeof(extra), "%u\t%d\t%d\t%d\t%d",
                     resp->user_id, resp->points, resp->wins, resp->draws, resp->losses);
            server_send_ok(srv, idx, resp->req_id, extra);

            /* Broadcast updated online list to lobby */
            server_broadcast_online_list(srv);
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Server Lifecycle & Networking                                             */
/* ------------------------------------------------------------------------- */
int server_set_nonblocking(int fd) {
    if (fd < 0) return -1;
#if defined(_WIN32)
    u_long mode = 1;
    if (ioctlsocket((SOCKET)fd, FIONBIO, &mode) != 0) {
        return -1;
    }
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) return -1;
#endif
    return 0;
}

int server_init(server_t *srv, const char *host, int port, const char *db_path) {
    if (!srv) return -1;
    memset(srv, 0, sizeof(*srv));
    srv->listen_fd = -1;
    srv->db_pipe[0] = -1;
    srv->db_pipe[1] = -1;
    srv->running = false;
    srv->port = (port > 0) ? port : DEFAULT_SERVER_PORT;

    const char *bind_host = (host && *host) ? host : DEFAULT_SERVER_HOST;
    snprintf(srv->host, sizeof(srv->host), "%s", bind_host);

    const char *actual_db = (db_path && *db_path) ? db_path : getenv("CARO_DB_PATH");
    if (!actual_db || !*actual_db) actual_db = "caro.db";
    snprintf(srv->db_path, sizeof(srv->db_path), "%s", actual_db);

    for (size_t i = 0; i < SERVER_MAX_CLIENTS; ++i) {
        srv->clients[i].fd = -1;
        srv->clients[i].state = STATE_DISCONNECTED;
        srv->clients[i].generation = 1;
        srv->clients[i].pending_invite_id = 0;
    }

    /* Milestone 4: Initialize challenge invites and games */
    srv->next_invite_id = 1;
    srv->invite_timeout_sec = DEFAULT_INVITE_TIMEOUT_SEC;
    srv->invite_timeout_ms = (int64_t)DEFAULT_INVITE_TIMEOUT_SEC * 1000;
    const char *env_to = getenv("CARO_INVITE_TIMEOUT");
    if (env_to && *env_to) {
        double val = atof(env_to);
        if (val > 0.0) {
            srv->invite_timeout_sec = (val < 1.0) ? 1 : (int)val;
            srv->invite_timeout_ms = (int64_t)(val * 1000.0);
            if (srv->invite_timeout_ms <= 1000 && srv->invite_timeout_ms > 200) {
                srv->invite_timeout_ms -= 100;
            }
        }
    }
    memset(srv->invites, 0, sizeof(srv->invites));
    memset(srv->games, 0, sizeof(srv->games));
    srv->active_games_count = 0;
    srv->next_game_id = 1;

#if defined(_WIN32)
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "[-] WSAStartup failed\n");
        return -1;
    }
#else
    signal(SIGPIPE, SIG_IGN);
#endif

    /* Synchronously initialize SQLite storage & recover crashed games */
    if (storage_init(srv->db_path) != STORAGE_OK) {
        fprintf(stderr, "[-] Failed to initialize database: %s\n", srv->db_path);
        return -1;
    }
    storage_recover_crashed_games();

    /* Create DB worker self-pipe */
    if (caro_pipe_create(srv->db_pipe) != 0) {
        fprintf(stderr, "[-] Failed to create DB self-pipe\n");
        storage_close();
        return -1;
    }

    /* Initialize DB worker synchronization */
    pthread_mutex_init(&srv->db_queue_mutex, NULL);
    pthread_cond_init(&srv->db_queue_cond, NULL);
    srv->db_worker_shutdown = false;
    srv->db_req_head = srv->db_req_tail = srv->db_req_count = 0;
    srv->db_resp_head = srv->db_resp_tail = srv->db_resp_count = 0;

    if (pthread_create(&srv->db_worker_thread, NULL, db_worker_thread_func, srv) != 0) {
        fprintf(stderr, "[-] Failed to create DB worker thread\n");
        caro_pipe_close(srv->db_pipe);
        storage_close();
        return -1;
    }
    srv->db_worker_started = true;

    /* Create TCP listener socket */
    int listen_fd = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("[-] socket creation failed");
        server_cleanup(srv);
        return -1;
    }

    int opt = 1;
#if defined(_WIN32)
    setsockopt((SOCKET)listen_fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));
#else
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif

    if (server_set_nonblocking(listen_fd) < 0) {
        perror("[-] Failed to set listen socket non-blocking");
        socket_close(listen_fd);
        server_cleanup(srv);
        return -1;
    }

    struct sockaddr_in srv_addr;
    memset(&srv_addr, 0, sizeof(srv_addr));
    srv_addr.sin_family = AF_INET;
    srv_addr.sin_port = htons((uint16_t)srv->port);
    if (inet_pton(AF_INET, srv->host, &srv_addr.sin_addr) <= 0) {
        srv_addr.sin_addr.s_addr = INADDR_ANY;
    }

    if (bind(listen_fd, (struct sockaddr *)&srv_addr, sizeof(srv_addr)) < 0) {
        perror("[-] bind failed");
        socket_close(listen_fd);
        server_cleanup(srv);
        return -1;
    }

    if (listen(listen_fd, SERVER_LISTEN_BACKLOG) < 0) {
        perror("[-] listen failed");
        socket_close(listen_fd);
        server_cleanup(srv);
        return -1;
    }

    srv->listen_fd = listen_fd;
    srv->running = true;

    printf("[+] Caro Server initialized on %s:%d (listen_fd=%d, db=%s)\n",
           srv->host, srv->port, srv->listen_fd, srv->db_path);
    return 0;
}

void server_stop(server_t *srv) {
    if (srv) {
        srv->running = false;
    }
}

int server_broadcast_online_list(server_t *srv) {
    if (!srv) return -1;

    char payload[PROTO_MAX_PAYLOAD_SIZE];
    int count = 0;
    for (int i = 0; i < SERVER_MAX_CLIENTS; ++i) {
        if (srv->clients[i].fd >= 0) {
            count++;
        }
    }

    int offset = snprintf(payload, sizeof(payload), "ONLINE_LIST\t0\t%d", count);
    for (int i = 0; i < SERVER_MAX_CLIENTS && offset < (int)(sizeof(payload) - 64); ++i) {
        if (srv->clients[i].fd >= 0) {
            const char *st_str = "IDLE";
            if (srv->clients[i].state == STATE_IN_GAME) {
                st_str = "IN_GAME";
            } else if (srv->clients[i].state == STATE_IN_INVITE) {
                st_str = "BUSY";
            } else if (srv->clients[i].state == STATE_IN_LOBBY) {
                st_str = "LOBBY";
            }
            offset += snprintf(payload + offset, sizeof(payload) - (size_t)offset,
                               "\t%s:%s", srv->clients[i].username, st_str);
        }
    }
    if (offset < (int)(sizeof(payload) - 1)) {
        payload[offset++] = '\n';
        payload[offset] = '\0';
    }

    uint8_t frame_buf[PROTO_MAX_PAYLOAD_SIZE + PROTO_HEADER_SIZE];
    size_t frame_len = 0;
    if (protocol_pack_frame(payload, (size_t)offset, frame_buf, sizeof(frame_buf), &frame_len) != PROTO_OK) {
        return -1;
    }

    /* Push presence list to clients active in lobby, authenticated, or in invite */
    for (int i = 0; i < SERVER_MAX_CLIENTS; ++i) {
        if (srv->clients[i].fd >= 0 &&
            (srv->clients[i].state == STATE_AUTHENTICATED ||
             srv->clients[i].state == STATE_IN_LOBBY ||
             srv->clients[i].state == STATE_IN_INVITE)) {
            server_queue_send(srv, i, frame_buf, frame_len);
        }
    }

    return 0;
}

/* ========================================================================= */
/* Structured Per-Game Logging Routines (F20)                               */
/* ========================================================================= */

int game_log_init(uint32_t game_id, const char *pX_name, uint32_t pX_id,
                  const char *pO_name, uint32_t pO_id) {
    caro_mkdir("logs");
    char path[128];
    snprintf(path, sizeof(path), "logs/game_%u.log", game_id);
    FILE *f = fopen(path, "w");
    if (!f) return -1;

    time_t now = time(NULL);
    char ts[64];
    struct tm tm_buf;
#if defined(_WIN32)
    localtime_s(&tm_buf, &now);
#else
    localtime_r(&now, &tm_buf);
#endif
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_buf);

    fprintf(f, "============================================================\n");
    fprintf(f, "CARO (GOMOKU 15x15) MATCH LOG\n");
    fprintf(f, "============================================================\n");
    fprintf(f, "Match ID   : %u\n", game_id);
    fprintf(f, "Start Time : %s\n", ts);
    fprintf(f, "Player X   : %s (ID: %u)\n", pX_name ? pX_name : "Unknown", pX_id);
    fprintf(f, "Player O   : %s (ID: %u)\n", pO_name ? pO_name : "Unknown", pO_id);
    fprintf(f, "------------------------------------------------------------\n");
    fprintf(f, "Seq   Timestamp            Player  Username         Coords\n");
    fprintf(f, "------------------------------------------------------------\n");
    fflush(f);
    fclose(f);
    return 0;
}

int game_log_move(uint32_t game_id, uint32_t seq, const char *stone,
                  uint32_t user_id, const char *username, int row, int col) {
    (void)user_id;
    char path[128];
    snprintf(path, sizeof(path), "logs/game_%u.log", game_id);
    FILE *f = fopen(path, "a");
    if (!f) return -1;

    time_t now = time(NULL);
    char ts[64];
    struct tm tm_buf;
#if defined(_WIN32)
    localtime_s(&tm_buf, &now);
#else
    localtime_r(&now, &tm_buf);
#endif
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_buf);

    fprintf(f, "%03u   %s  %-6s  %-16s (%2d, %2d)\n",
            seq, ts, stone ? stone : "?", username ? username : "Anon", row, col);
    fflush(f);
    fclose(f);
    return 0;
}

int game_log_conclude(uint32_t game_id, const char *outcome, const char *reason,
                      uint32_t winner_id, const char *winner_name, uint32_t total_moves) {
    char path[128];
    snprintf(path, sizeof(path), "logs/game_%u.log", game_id);
    FILE *f = fopen(path, "a");
    if (!f) return -1;

    time_t now = time(NULL);
    char ts[64];
    struct tm tm_buf;
#if defined(_WIN32)
    localtime_s(&tm_buf, &now);
#else
    localtime_r(&now, &tm_buf);
#endif
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_buf);

    fprintf(f, "------------------------------------------------------------\n");
    fprintf(f, "MATCH CONCLUSION\n");
    fprintf(f, "------------------------------------------------------------\n");
    fprintf(f, "End Time   : %s\n", ts);
    fprintf(f, "Outcome    : %s\n", outcome ? outcome : "UNKNOWN");
    fprintf(f, "Reason     : %s\n", reason ? reason : "UNKNOWN");
    if (winner_id > 0) {
        fprintf(f, "Winner     : %s (ID: %u)\n", winner_name ? winner_name : "Unknown", winner_id);
    } else {
        fprintf(f, "Winner     : None (Draw)\n");
    }
    fprintf(f, "Total Moves: %u\n", total_moves);
    fprintf(f, "============================================================\n");
    fflush(f);
    fclose(f);
    return 0;
}

/* ========================================================================= */
/* Active Match Management & Rules Integration (F16-F20)                     */
/* ========================================================================= */

game_slot_t *server_find_game(server_t *srv, uint32_t game_id) {
    if (!srv || game_id == 0) return NULL;
    for (size_t i = 0; i < CARO_MAX_GAMES; ++i) {
        if (srv->games[i].active && srv->games[i].game.game_id == game_id) {
            return &srv->games[i];
        }
    }
    return NULL;
}

int server_find_free_game_slot(server_t *srv) {
    if (!srv) return -1;
    for (int i = 0; i < CARO_MAX_GAMES; ++i) {
        if (!srv->games[i].active) return i;
    }
    return -1;
}

int game_manager_conclude(server_t *srv, uint32_t game_id, const char *outcome,
                          const char *reason, int winner_idx, int loser_idx, bool is_draw) {
    if (!srv || game_id == 0) return -1;

    game_slot_t *slot = NULL;
    for (int i = 0; i < CARO_MAX_GAMES; ++i) {
        if (srv->games[i].active && srv->games[i].game.game_id == game_id) {
            slot = &srv->games[i];
            break;
        }
    }
    if (!slot) return -1;

    /* 1. Idempotency guard: ensure conclusion executes exactly once per match */
    if (slot->settled) {
        printf("[*] Game %u already settled (idempotent no-op)\n", game_id);
        return 0;
    }
    slot->settled = true;
    slot->game.status = GAME_STATUS_FINISHED;

    /* 2. Resolve winner and loser IDs and names */
    uint32_t winner_uid = 0;
    uint32_t loser_uid = 0;
    const char *winner_name = "NONE";

    if (!is_draw) {
        if (winner_idx >= 0 && winner_idx < SERVER_MAX_CLIENTS) {
            winner_uid = srv->clients[winner_idx].user_id;
            winner_name = srv->clients[winner_idx].username;
        } else if (winner_idx == slot->player_x_idx) {
            winner_uid = slot->game.player_x_id;
            winner_name = slot->game.player_x_name;
        } else {
            winner_uid = slot->game.player_o_id;
            winner_name = slot->game.player_o_name;
        }

        if (loser_idx >= 0 && loser_idx < SERVER_MAX_CLIENTS) {
            loser_uid = srv->clients[loser_idx].user_id;
        } else if (winner_uid == slot->game.player_x_id) {
            loser_uid = slot->game.player_o_id;
        } else {
            loser_uid = slot->game.player_x_id;
        }
    }

    /* 3. Log match conclusion to logs/game_<game_id>.log (F20) */
    game_log_conclude(game_id, outcome, reason, winner_uid, winner_name, slot->game.move_count);

    /* 4. Format and broadcast GAME_OVER wire frame */
    char go_payload[256];
    snprintf(go_payload, sizeof(go_payload), "GAME_OVER\t%u\t%s\t%s\t%s\n",
             game_id, outcome ? outcome : "UNKNOWN", winner_name, reason ? reason : "UNKNOWN");

    uint8_t frame_buf[512];
    size_t frame_len = 0;
    protocol_pack_frame(go_payload, strlen(go_payload), frame_buf, sizeof(frame_buf), &frame_len);

    /* Push to Player X if valid and generation matches */
    int px = slot->player_x_idx;
    if (px >= 0 && px < SERVER_MAX_CLIENTS) {
        client_conn_t *cx = &srv->clients[px];
        if (cx->fd >= 0 && cx->state != STATE_DISCONNECTED && cx->generation == slot->player_x_gen) {
            server_queue_send(srv, px, frame_buf, frame_len);
            cx->current_game_id = 0;
            cx->state = STATE_IN_LOBBY;
        }
    }

    /* Push to Player O if valid and generation matches */
    int po = slot->player_o_idx;
    if (po >= 0 && po < SERVER_MAX_CLIENTS) {
        client_conn_t *co = &srv->clients[po];
        if (co->fd >= 0 && co->state != STATE_DISCONNECTED && co->generation == slot->player_o_gen) {
            server_queue_send(srv, po, frame_buf, frame_len);
            co->current_game_id = 0;
            co->state = STATE_IN_LOBBY;
        }
    }

    /* 5. Enqueue asynchronous DB settlement job */
    db_request_t req;
    memset(&req, 0, sizeof(req));
    req.req_id = game_id;
    req.type = DB_REQ_SETTLE_GAME;
    req.client_idx = (winner_idx >= 0) ? winner_idx : -1;
    req.generation = (winner_idx >= 0 && winner_idx < SERVER_MAX_CLIENTS) ? srv->clients[winner_idx].generation : 0;
    req.game_id = game_id;
    req.winner_id = is_draw ? 0 : winner_uid;
    req.loser_id = is_draw ? 0 : loser_uid;
    req.is_draw = is_draw;
    snprintf(req.outcome, sizeof(req.outcome), "%s", outcome ? outcome : "");
    snprintf(req.reason, sizeof(req.reason), "%s", reason ? reason : "");
    server_queue_db_request(srv, &req);

    /* 6. Recycle game slot */
    slot->active = false;
    if (srv->active_games_count > 0) {
        srv->active_games_count--;
    }

    printf("[GAME] Match concluded: game_id=%u | outcome=%s reason=%s winner=%s\n",
           game_id, outcome ? outcome : "", reason ? reason : "", winner_name);

    /* 7. Broadcast updated online list to restore players as available in lobby */
    server_broadcast_online_list(srv);
    return 0;
}

void game_manager_handle_disconnect(server_t *srv, int disconnected_idx) {
    if (!srv || disconnected_idx < 0 || disconnected_idx >= SERVER_MAX_CLIENTS) return;
    client_conn_t *c = &srv->clients[disconnected_idx];
    if (c->current_game_id == 0) return;

    uint32_t gid = c->current_game_id;
    game_slot_t *slot = NULL;
    for (int i = 0; i < CARO_MAX_GAMES; ++i) {
        if (srv->games[i].active && srv->games[i].game.game_id == gid) {
            slot = &srv->games[i];
            break;
        }
    }
    if (!slot || slot->settled) return;

    int winner_idx = -1;
    int loser_idx = disconnected_idx;
    if (disconnected_idx == slot->player_x_idx) {
        winner_idx = slot->player_o_idx;
    } else if (disconnected_idx == slot->player_o_idx) {
        winner_idx = slot->player_x_idx;
    } else {
        return;
    }

    printf("[GAME] Disconnect forfeit in game %u: slot %d left -> slot %d wins\n",
           gid, disconnected_idx, winner_idx);

    game_manager_conclude(srv, gid, "DISCONNECT", "DISCONNECT", winner_idx, loser_idx, false);
}

int game_manager_start(server_t *srv, int challenger_idx, int target_idx) {
    if (!srv || challenger_idx < 0 || target_idx < 0 ||
        challenger_idx >= SERVER_MAX_CLIENTS || target_idx >= SERVER_MAX_CLIENTS) {
        return -1;
    }
    int slot_idx = server_find_free_game_slot(srv);
    if (slot_idx < 0) {
        fprintf(stderr, "[-] No free game slots available (max %d)\n", CARO_MAX_GAMES);
        server_send_err_ref(srv, challenger_idx, 0, ERR_INTERNAL_SERVER, "Server game capacity reached");
        server_send_err_ref(srv, target_idx, 0, ERR_INTERNAL_SERVER, "Server game capacity reached");
        srv->clients[challenger_idx].state = STATE_IN_LOBBY;
        srv->clients[target_idx].state = STATE_IN_LOBBY;
        return -1;
    }

    client_conn_t *cX = &srv->clients[challenger_idx];
    client_conn_t *cO = &srv->clients[target_idx];

    /* Assign Game ID via storage or monotonically increasing counter */
    uint32_t gid = 0;
    if (cX->user_id > 0 && cO->user_id > 0 && cX->user_id != cO->user_id) {
        storage_create_game(cX->user_id, cO->user_id, &gid);
    }
    if (gid == 0) {
        gid = srv->next_game_id++;
        if (gid == 0) gid = srv->next_game_id++;
    }

    game_slot_t *slot = &srv->games[slot_idx];
    memset(slot, 0, sizeof(*slot));
    game_init(&slot->game, gid, cX->fd, cO->fd, cX->user_id, cO->user_id);

    slot->active = true;
    slot->settled = false;
    slot->player_x_idx = challenger_idx;
    slot->player_o_idx = target_idx;
    slot->player_x_gen = cX->generation;
    slot->player_o_gen = cO->generation;
    slot->draw_offered_by = CELL_EMPTY;
    slot->start_time = time(NULL);
    strncpy(slot->game.player_x_name, cX->username, sizeof(slot->game.player_x_name) - 1);
    slot->game.player_x_name[sizeof(slot->game.player_x_name) - 1] = '\0';
    strncpy(slot->game.player_o_name, cO->username, sizeof(slot->game.player_o_name) - 1);
    slot->game.player_o_name[sizeof(slot->game.player_o_name) - 1] = '\0';

    cX->state = STATE_IN_GAME;
    cX->current_game_id = gid;
    cX->pending_invite_id = 0;

    cO->state = STATE_IN_GAME;
    cO->current_game_id = gid;
    cO->pending_invite_id = 0;

    srv->active_games_count++;

    /* Send GAME_START to challenger (X) and target (O) */
    char start_payload[PROTO_MAX_PAYLOAD_SIZE];
    uint8_t frame_buf[PROTO_MAX_FRAME_BUFFER];
    size_t frame_len = 0;

    /* To Challenger: GAME_START\t<game_id>\tX\t<target_username>\n */
    snprintf(start_payload, sizeof(start_payload), "GAME_START\t%u\tX\t%s\n", gid, cO->username);
    if (protocol_pack_frame(start_payload, strlen(start_payload), frame_buf, sizeof(frame_buf), &frame_len) == PROTO_OK) {
        server_queue_send(srv, challenger_idx, frame_buf, frame_len);
    }

    /* To Target: GAME_START\t<game_id>\tO\t<challenger_username>\n */
    snprintf(start_payload, sizeof(start_payload), "GAME_START\t%u\tO\t%s\n", gid, cX->username);
    if (protocol_pack_frame(start_payload, strlen(start_payload), frame_buf, sizeof(frame_buf), &frame_len) == PROTO_OK) {
        server_queue_send(srv, target_idx, frame_buf, frame_len);
    }

    /* Initialize log file (F20) */
    game_log_init(gid, cX->username, cX->user_id, cO->username, cO->user_id);

    printf("[GAME] Match started: game_id=%u | P1 '%s' (X, slot %d) vs P2 '%s' (O, slot %d)\n",
           gid, cX->username, challenger_idx, cO->username, target_idx);

    server_broadcast_online_list(srv);
    return 0;
}

int game_manager_handle_move(server_t *srv, int idx, uint32_t game_id, uint32_t seq, int row, int col) {
    if (!srv || idx < 0 || idx >= SERVER_MAX_CLIENTS) return -1;
    client_conn_t *c = &srv->clients[idx];

    /* Step 1: Session state validation */
    if (c->state != STATE_IN_GAME || c->current_game_id != game_id) {
        return server_send_err_ref(srv, idx, game_id, ERR_NOT_IN_GAME, "Not currently in this game");
    }

    game_slot_t *slot = server_find_game(srv, game_id);
    if (!slot || !slot->active || slot->settled || slot->game.status != GAME_STATUS_IN_PROGRESS) {
        return server_send_err_ref(srv, idx, game_id, ERR_NOT_IN_GAME, "Game not found or finished");
    }

    /* Step 2: Game membership check */
    bool is_x = (slot->player_x_idx == idx && slot->player_x_gen == c->generation);
    bool is_o = (slot->player_o_idx == idx && slot->player_o_gen == c->generation);
    if (!is_x && !is_o) {
        return server_send_err_ref(srv, idx, game_id, ERR_NOT_IN_GAME, "Player not assigned to this match");
    }

    /* Step 3: Turn validation */
    caro_cell_t mover_stone = is_x ? CELL_X : CELL_O;
    if (slot->game.current_turn != mover_stone) {
        return server_send_err_ref(srv, idx, game_id, ERR_NOT_YOUR_TURN, "Not your turn");
    }

    /* Step 4: Sequence validation */
    if (seq != slot->game.expected_seq) {
        return server_send_err_ref(srv, idx, game_id, ERR_SEQ_MISMATCH, "Sequence number mismatch");
    }

    /* Step 5: Coordinate bounds validation [0..14] */
    if (row < 0 || row >= CARO_BOARD_SIZE || col < 0 || col >= CARO_BOARD_SIZE) {
        return server_send_err_ref(srv, idx, game_id, ERR_INVALID_MOVE, "Coordinates out of bounds");
    }

    /* Step 6: Empty cell validation */
    if (slot->game.board[row][col] != CELL_EMPTY) {
        return server_send_err_ref(srv, idx, game_id, ERR_INVALID_MOVE, "Cell is already occupied");
    }

    /* Execute move via rules engine */
    if (!game_make_move(&slot->game, mover_stone, row, col)) {
        return server_send_err_ref(srv, idx, game_id, ERR_INVALID_MOVE, "Illegal move");
    }

    /* Send MOVE_ACK to mover */
    char ack_payload[PROTO_MAX_PAYLOAD_SIZE];
    uint8_t frame_buf[PROTO_MAX_FRAME_BUFFER];
    size_t frame_len = 0;
    snprintf(ack_payload, sizeof(ack_payload), "MOVE_ACK\t%u\t%u\t%d\t%d\n", game_id, seq, row, col);
    if (protocol_pack_frame(ack_payload, strlen(ack_payload), frame_buf, sizeof(frame_buf), &frame_len) == PROTO_OK) {
        server_queue_send(srv, idx, frame_buf, frame_len);
    }

    /* Send MOVE_BROADCAST to opponent */
    int opp_idx = is_x ? slot->player_o_idx : slot->player_x_idx;
    uint32_t opp_gen = is_x ? slot->player_o_gen : slot->player_x_gen;
    if (opp_idx >= 0 && opp_idx < SERVER_MAX_CLIENTS &&
        srv->clients[opp_idx].fd >= 0 && srv->clients[opp_idx].generation == opp_gen) {
        char bcast_payload[PROTO_MAX_PAYLOAD_SIZE];
        snprintf(bcast_payload, sizeof(bcast_payload), "MOVE_BROADCAST\t%u\t%u\t%d\t%d\n", game_id, seq, row, col);
        if (protocol_pack_frame(bcast_payload, strlen(bcast_payload), frame_buf, sizeof(frame_buf), &frame_len) == PROTO_OK) {
            server_queue_send(srv, opp_idx, frame_buf, frame_len);
        }
    }

    /* Persist move */
    if (c->user_id > 0) {
        storage_record_move(game_id, seq, c->user_id, is_x ? "X" : "O", row, col);
    }

    /* Structured per-game move log (F20) */
    game_log_move(game_id, seq, is_x ? "X" : "O", c->user_id, c->username, row, col);

    /* Check match conclusion */
    if (slot->game.status == GAME_STATUS_FINISHED) {
        if (slot->game.result == GAME_RESULT_X_WON) {
            game_manager_conclude(srv, game_id, "WIN", "FIVE_IN_A_ROW", slot->player_x_idx, slot->player_o_idx, false);
        } else if (slot->game.result == GAME_RESULT_O_WON) {
            game_manager_conclude(srv, game_id, "WIN", "FIVE_IN_A_ROW", slot->player_o_idx, slot->player_x_idx, false);
        } else if (slot->game.result == GAME_RESULT_DRAW) {
            game_manager_conclude(srv, game_id, "DRAW", "BOARD_FULL", -1, -1, true);
        }
    }

    return 0;
}

/* ========================================================================= */
/* Challenge & Invitation Flow (F15)                                         */
/* ========================================================================= */

int challenge_create(server_t *srv, int challenger_idx, const char *target_name, uint32_t req_id) {
    if (!srv || challenger_idx < 0 || challenger_idx >= SERVER_MAX_CLIENTS || !target_name) {
        return -1;
    }
    client_conn_t *c_src = &srv->clients[challenger_idx];

    /* 1. Challenger must be in LOBBY */
    if (c_src->state == STATE_IN_GAME || c_src->state == STATE_IN_INVITE) {
        return server_send_err_ref(srv, challenger_idx, req_id, ERR_USER_BUSY, "You are currently busy or in a match");
    }
    if (c_src->state != STATE_IN_LOBBY) {
        return server_send_err_ref(srv, challenger_idx, req_id, ERR_UNAUTHORIZED, "Login required to challenge");
    }

    /* 2. Locate target user */
    int target_idx = -1;
    for (int i = 0; i < SERVER_MAX_CLIENTS; ++i) {
        if (srv->clients[i].fd >= 0 && strcasecmp(srv->clients[i].username, target_name) == 0) {
            target_idx = i;
            break;
        }
    }
    if (target_idx < 0) {
        return server_send_err_ref(srv, challenger_idx, req_id, ERR_USER_NOT_FOUND, "Target user not found");
    }

    /* 3. Cannot challenge self */
    if (target_idx == challenger_idx) {
        return server_send_err_ref(srv, challenger_idx, req_id, ERR_INVALID_PARAM, "Cannot challenge yourself");
    }

    /* 4. Target must be in LOBBY */
    client_conn_t *c_tgt = &srv->clients[target_idx];
    if (c_tgt->state != STATE_IN_LOBBY) {
        return server_send_err_ref(srv, challenger_idx, req_id, ERR_USER_BUSY, "Target user is currently busy or in a match");
    }

    /* 5. Find free invite slot */
    int slot = -1;
    for (int i = 0; i < SERVER_MAX_INVITES; ++i) {
        if (!srv->invites[i].active) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        return server_send_err_ref(srv, challenger_idx, req_id, ERR_INTERNAL_SERVER, "Server invite capacity reached");
    }

    /* 6. Allocate invite and lock both participants */
    uint32_t inv_id = srv->next_invite_id++;
    if (inv_id == 0) inv_id = srv->next_invite_id++;

    challenge_invite_t *inv = &srv->invites[slot];
    inv->active = true;
    inv->invite_id = inv_id;
    inv->req_id = req_id;
    inv->challenger_idx = challenger_idx;
    inv->challenger_gen = c_src->generation;
    inv->target_idx = target_idx;
    inv->target_gen = c_tgt->generation;
    inv->created_at = time(NULL);
    inv->expires_at = inv->created_at + (srv->invite_timeout_sec > 0 ? srv->invite_timeout_sec : DEFAULT_INVITE_TIMEOUT_SEC);
    inv->expires_at_ms = current_time_ms() + (srv->invite_timeout_ms > 0 ? srv->invite_timeout_ms : (DEFAULT_INVITE_TIMEOUT_SEC * 1000));

    c_src->state = STATE_IN_INVITE;
    c_src->pending_invite_id = inv_id;
    c_tgt->state = STATE_IN_INVITE;
    c_tgt->pending_invite_id = inv_id;

    /* 7. Send OK to challenger */
    server_send_ok(srv, challenger_idx, req_id, NULL);

    /* 8. Send CHALLENGE_REQ to target */
    char req_payload[PROTO_MAX_PAYLOAD_SIZE];
    int written = snprintf(req_payload, sizeof(req_payload), "CHALLENGE_REQ\t%u\t%u\t%s\n",
                           inv_id, c_src->user_id, c_src->username);
    uint8_t frame_buf[PROTO_MAX_PAYLOAD_SIZE + PROTO_HEADER_SIZE];
    size_t frame_len = 0;
    if (protocol_pack_frame(req_payload, (size_t)written, frame_buf, sizeof(frame_buf), &frame_len) == PROTO_OK) {
        server_queue_send(srv, target_idx, frame_buf, frame_len);
    }

    printf("[CHALLENGE] Created invite %u: '%s' (slot %d) -> '%s' (slot %d)\n",
           inv_id, c_src->username, challenger_idx, c_tgt->username, target_idx);

    server_broadcast_online_list(srv);
    return 0;
}

int challenge_respond(server_t *srv, int responder_idx, uint32_t invite_id, bool accept) {
    if (!srv || responder_idx < 0 || responder_idx >= SERVER_MAX_CLIENTS) return -1;
    client_conn_t *c_resp = &srv->clients[responder_idx];

    /* 1. Find active invite */
    int slot = -1;
    for (int i = 0; i < SERVER_MAX_INVITES; ++i) {
        if (srv->invites[i].active && srv->invites[i].invite_id == invite_id) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        return server_send_err_ref(srv, responder_idx, invite_id, ERR_INVITE_INVALID, "Invalid or expired invitation ID");
    }

    challenge_invite_t *inv = &srv->invites[slot];

    /* 2. Validate responder identity (Invalid Responder check TC-T2-06) */
    if (inv->target_idx != responder_idx || inv->target_gen != c_resp->generation) {
        return server_send_err_ref(srv, responder_idx, invite_id, ERR_INVITE_INVALID, "You are not the recipient of this invitation");
    }

    int c_idx = inv->challenger_idx;
    uint32_t c_gen = inv->challenger_gen;

    /* 3. Handle Decline */
    if (!accept) {
        inv->active = false;
        c_resp->state = STATE_IN_LOBBY;
        c_resp->pending_invite_id = 0;

        if (srv->clients[c_idx].fd >= 0 && srv->clients[c_idx].generation == c_gen) {
            srv->clients[c_idx].state = STATE_IN_LOBBY;
            srv->clients[c_idx].pending_invite_id = 0;
            server_send_err_ref(srv, c_idx, inv->req_id, ERR_USER_BUSY, "Invitation declined");
        }
        printf("[CHALLENGE] Invite %u declined by '%s'\n", invite_id, c_resp->username);
        server_broadcast_online_list(srv);
        return 0;
    }

    /* 4. Handle Accept: Check challenger still connected */
    if (srv->clients[c_idx].fd < 0 || srv->clients[c_idx].generation != c_gen) {
        inv->active = false;
        c_resp->state = STATE_IN_LOBBY;
        c_resp->pending_invite_id = 0;
        server_broadcast_online_list(srv);
        return server_send_err_ref(srv, responder_idx, invite_id, ERR_USER_NOT_FOUND, "Challenger disconnected");
    }

    /* Deactivate invite & unlock participants */
    inv->active = false;
    c_resp->pending_invite_id = 0;
    srv->clients[c_idx].pending_invite_id = 0;

    /* 5. Start match via game_manager_start */
    return game_manager_start(srv, c_idx, responder_idx);
}

void challenge_check_timeouts(server_t *srv) {
    if (!srv) return;
    int64_t now_ms = current_time_ms();

    for (int i = 0; i < SERVER_MAX_INVITES; ++i) {
        if (!srv->invites[i].active) continue;

        if (now_ms >= srv->invites[i].expires_at_ms) {
            challenge_invite_t *inv = &srv->invites[i];
            int c_idx = inv->challenger_idx;
            int t_idx = inv->target_idx;

            printf("[CHALLENGE] Invite %u expired (timeout %ds)\n",
                   inv->invite_id, srv->invite_timeout_sec);

            /* Unlock challenger and send ERR 302 */
            if (srv->clients[c_idx].fd >= 0 && srv->clients[c_idx].generation == inv->challenger_gen) {
                srv->clients[c_idx].state = STATE_IN_LOBBY;
                srv->clients[c_idx].pending_invite_id = 0;
                server_send_err_ref(srv, c_idx, inv->req_id, ERR_INVITE_EXPIRED, "Challenge invitation expired (30s timeout)");
            }

            /* Unlock target */
            if (srv->clients[t_idx].fd >= 0 && srv->clients[t_idx].generation == inv->target_gen) {
                srv->clients[t_idx].state = STATE_IN_LOBBY;
                srv->clients[t_idx].pending_invite_id = 0;
            }

            inv->active = false;
            inv->invite_id = 0;
            server_broadcast_online_list(srv);
        }
    }
}

void challenge_handle_disconnect(server_t *srv, int client_idx) {
    if (!srv || client_idx < 0 || client_idx >= SERVER_MAX_CLIENTS) return;

    for (int i = 0; i < SERVER_MAX_INVITES; ++i) {
        if (!srv->invites[i].active) continue;

        /* If disconnected client was challenger */
        if (srv->invites[i].challenger_idx == client_idx &&
            srv->invites[i].challenger_gen == srv->clients[client_idx].generation) {
            int t_idx = srv->invites[i].target_idx;
            if (srv->clients[t_idx].fd >= 0 && srv->clients[t_idx].generation == srv->invites[i].target_gen) {
                srv->clients[t_idx].state = STATE_IN_LOBBY;
                srv->clients[t_idx].pending_invite_id = 0;
                printf("[CHALLENGE] Target slot %d unlocked due to challenger disconnect\n", t_idx);
            }
            srv->invites[i].active = false;
        }
        /* If disconnected client was target */
        else if (srv->invites[i].target_idx == client_idx &&
                 srv->invites[i].target_gen == srv->clients[client_idx].generation) {
            int c_idx = srv->invites[i].challenger_idx;
            if (srv->clients[c_idx].fd >= 0 && srv->clients[c_idx].generation == srv->invites[i].challenger_gen) {
                srv->clients[c_idx].state = STATE_IN_LOBBY;
                srv->clients[c_idx].pending_invite_id = 0;
                server_send_err_ref(srv, c_idx, srv->invites[i].req_id, ERR_USER_BUSY, "Target user disconnected");
                printf("[CHALLENGE] Challenger slot %d unlocked due to target disconnect\n", c_idx);
            }
            srv->invites[i].active = false;
        }
    }
}

void server_disconnect_client(server_t *srv, int idx, const char *reason) {
    if (!srv || idx < 0 || idx >= SERVER_MAX_CLIENTS) return;
    client_conn_t *c = &srv->clients[idx];
    if (c->fd < 0) return;

    printf("[-] Client disconnected [slot %d, fd=%d, gen=%u, %s:%u]: %s\n",
           idx, c->fd, c->generation, c->ip_str, c->port, reason ? reason : "Unknown reason");

    /* Handle challenge disconnect cleanup (F15) */
    challenge_handle_disconnect(srv, idx);

    /* If client was in an active game, forfeit cleanly and notify opponent via GAME_OVER (F19) */
    if (c->current_game_id > 0) {
        game_manager_handle_disconnect(srv, idx);
        c->current_game_id = 0;
    }

    socket_close(c->fd);
    c->fd = -1;
    c->state = STATE_DISCONNECTED;
    c->generation++; /* Monotonically increment generation counter to discard stale results */
    c->rx_len = 0;
    c->tx_len = 0;
    c->tx_offset = 0;
    c->last_active_ts = 0;
    c->user_id = 0;
    c->username[0] = '\0';
    c->is_guest = false;
    c->current_game_id = 0;
    c->pending_invite_id = 0;
    memset(c->rx_buf, 0, sizeof(c->rx_buf));
    memset(c->tx_buf, 0, sizeof(c->tx_buf));

    if (srv->active_clients_count > 0) {
        srv->active_clients_count--;
    }

    /* Broadcast updated presence list to lobby */
    server_broadcast_online_list(srv);
}

void server_cleanup(server_t *srv) {
    if (!srv) return;

    /* 1. Disconnect all clients */
    for (int i = 0; i < SERVER_MAX_CLIENTS; ++i) {
        if (srv->clients[i].fd >= 0) {
            server_disconnect_client(srv, i, "Server shutting down");
        }
    }

    /* 2. Close TCP listener */
    if (srv->listen_fd >= 0) {
        socket_close(srv->listen_fd);
        srv->listen_fd = -1;
    }

    /* 3. Stop DB worker thread cleanly */
    if (srv->db_worker_started) {
        pthread_mutex_lock(&srv->db_queue_mutex);
        srv->db_worker_shutdown = true;
        pthread_cond_broadcast(&srv->db_queue_cond);
        pthread_mutex_unlock(&srv->db_queue_mutex);

        pthread_join(srv->db_worker_thread, NULL);
        srv->db_worker_started = false;
    }

    /* 4. Close self-pipe */
    caro_pipe_close(srv->db_pipe);

    /* 5. Destroy mutex and condition variable */
    pthread_mutex_destroy(&srv->db_queue_mutex);
    pthread_cond_destroy(&srv->db_queue_cond);

    /* 6. Close SQLite storage (worker thread joined, serialized) */
    storage_close();

    srv->running = false;
    srv->active_clients_count = 0;

#if defined(_WIN32)
    WSACleanup();
#endif
    printf("[+] Server cleanup completed\n");
}

int server_queue_send(server_t *srv, int idx, const uint8_t *data, size_t len) {
    if (!srv || idx < 0 || idx >= SERVER_MAX_CLIENTS || !data || len == 0) {
        return -1;
    }
    client_conn_t *c = &srv->clients[idx];
    if (c->fd < 0) return -1;

    /* If all queued bytes were already sent, reset offset and length */
    if (c->tx_offset >= c->tx_len) {
        c->tx_len = 0;
        c->tx_offset = 0;
    }

    /* If space is tight and dead space exists before tx_offset, compact */
    if (c->tx_len + len > sizeof(c->tx_buf) && c->tx_offset > 0) {
        memmove(c->tx_buf, c->tx_buf + c->tx_offset, c->tx_len - c->tx_offset);
        c->tx_len -= c->tx_offset;
        c->tx_offset = 0;
    }

    if (c->tx_len + len > sizeof(c->tx_buf)) {
        fprintf(stderr, "[-] TX buffer overflow for client slot %d (queued %zu, adding %zu)\n",
                idx, c->tx_len, len);
        server_disconnect_client(srv, idx, "TX buffer overflow");
        return -1;
    }

    memcpy(c->tx_buf + c->tx_len, data, len);
    c->tx_len += len;
    return 0;
}

int server_send_err(server_t *srv, int idx, int err_code, const char *reason) {
    return server_send_err_ref(srv, idx, 0, err_code, reason);
}

int server_send_err_ref(server_t *srv, int idx, uint32_t ref_id, int err_code, const char *reason) {
    if (!srv || idx < 0 || idx >= SERVER_MAX_CLIENTS) return -1;

    const char *msg = reason ? reason : protocol_err_str(err_code);
    char err_payload[PROTO_MAX_PAYLOAD_SIZE];
    int written;
    if (ref_id > 0) {
        written = snprintf(err_payload, sizeof(err_payload), "ERR\t%u\t%d\t%s\n", ref_id, err_code, msg);
    } else {
        written = snprintf(err_payload, sizeof(err_payload), "ERR\t%d\t%s\n", err_code, msg);
    }
    if (written < 0 || (size_t)written >= sizeof(err_payload)) return -1;

    uint8_t frame_buf[PROTO_MAX_PAYLOAD_SIZE + PROTO_HEADER_SIZE];
    size_t frame_len = 0;
    int pack_res = protocol_pack_frame(err_payload, (size_t)written,
                                       frame_buf, sizeof(frame_buf), &frame_len);
    if (pack_res != PROTO_OK) {
        return -1;
    }

    return server_queue_send(srv, idx, frame_buf, frame_len);
}

int server_send_ok(server_t *srv, int idx, uint32_t req_id, const char *extra_data) {
    if (!srv || idx < 0 || idx >= SERVER_MAX_CLIENTS) return -1;

    char ok_payload[PROTO_MAX_PAYLOAD_SIZE];
    int written;
    if (extra_data && *extra_data) {
        written = snprintf(ok_payload, sizeof(ok_payload), "OK\t%u\t%s\n", req_id, extra_data);
    } else {
        written = snprintf(ok_payload, sizeof(ok_payload), "OK\t%u\n", req_id);
    }
    if (written < 0 || (size_t)written >= sizeof(ok_payload)) return -1;

    uint8_t frame_buf[PROTO_MAX_PAYLOAD_SIZE + PROTO_HEADER_SIZE];
    size_t frame_len = 0;
    int pack_res = protocol_pack_frame(ok_payload, (size_t)written,
                                       frame_buf, sizeof(frame_buf), &frame_len);
    if (pack_res != PROTO_OK) {
        return -1;
    }

    return server_queue_send(srv, idx, frame_buf, frame_len);
}

int server_handle_client_payload(server_t *srv, int idx, const char *payload, size_t len) {
    if (!srv || idx < 0 || idx >= SERVER_MAX_CLIENTS || !payload) return -1;
    client_conn_t *c = &srv->clients[idx];
    if (c->fd < 0) return -1;

    protocol_msg_t msg;
    int parse_res = protocol_parse_msg(payload, len, &msg);
    if (parse_res != PROTO_OK) {
        /* Check if this was an invalid MOVE command (e.g. coordinates out of bounds >14) */
        char temp_tokens[PROTO_MAX_TOKENS][PROTO_MAX_TOKEN_LEN];
        int tok_count = 0;
        if (protocol_tokenize(payload, len, temp_tokens, &tok_count) == PROTO_OK && tok_count >= 2) {
            if (strcmp(temp_tokens[0], "MOVE") == 0) {
                uint32_t gid = (uint32_t)strtoul(temp_tokens[1], NULL, 10);
                return server_send_err_ref(srv, idx, gid, ERR_INVALID_PARAM, "Invalid move parameters");
            }
        }
        fprintf(stderr, "[-] Invalid message syntax from client %d: %s\n", idx, payload);
        int err_code = (parse_res == PROTO_ERR_INVALID_PARAM) ? ERR_INVALID_PARAM : ERR_BAD_FRAME;
        return server_send_err(srv, idx, err_code, "Invalid protocol format");
    }

    switch (msg.cmd) {
        case CMD_PING: {
            const char *echo_str = (msg.token_count > 1) ? msg.tokens[1] : "";
            char pong_payload[PROTO_MAX_PAYLOAD_SIZE];
            protocol_create_pong(echo_str, pong_payload, sizeof(pong_payload));

            uint8_t frame_buf[PROTO_MAX_PAYLOAD_SIZE + PROTO_HEADER_SIZE];
            size_t frame_len = 0;
            protocol_pack_frame(pong_payload, strlen(pong_payload),
                                frame_buf, sizeof(frame_buf), &frame_len);

            printf("[*] Client [%d] PING received (echo='%s') -> queueing PONG\n", idx, echo_str);
            return server_queue_send(srv, idx, frame_buf, frame_len);
        }

        case CMD_PONG: {
            const char *echo_str = (msg.token_count > 1) ? msg.tokens[1] : "";
            printf("[*] Client [%d] PONG received (echo='%s')\n", idx, echo_str);
            return 0;
        }

        case CMD_GET_ONLINE: {
            uint32_t req_id = (msg.token_count > 1) ? msg.request_id : 0;
            char list_payload[PROTO_MAX_PAYLOAD_SIZE];
            int count = 0;
            for (int i = 0; i < SERVER_MAX_CLIENTS; ++i) {
                if (srv->clients[i].fd >= 0) count++;
            }
            int offset = snprintf(list_payload, sizeof(list_payload), "ONLINE_LIST\t%u\t%d", req_id, count);
            for (int i = 0; i < SERVER_MAX_CLIENTS && offset < (int)(sizeof(list_payload) - 64); ++i) {
                if (srv->clients[i].fd >= 0) {
                    const char *st_str = (srv->clients[i].state == STATE_IN_GAME) ? "IN_GAME" :
                                         (srv->clients[i].state == STATE_IN_INVITE) ? "BUSY" :
                                         (srv->clients[i].state == STATE_IN_LOBBY) ? "LOBBY" : "IDLE";
                    offset += snprintf(list_payload + offset, sizeof(list_payload) - (size_t)offset,
                                       "\t%s:%s", srv->clients[i].username, st_str);
                }
            }
            if (offset < (int)(sizeof(list_payload) - 1)) {
                list_payload[offset++] = '\n';
                list_payload[offset] = '\0';
            }
            uint8_t frame_buf[PROTO_MAX_PAYLOAD_SIZE + PROTO_HEADER_SIZE];
            size_t frame_len = 0;
            if (protocol_pack_frame(list_payload, (size_t)offset, frame_buf, sizeof(frame_buf), &frame_len) == PROTO_OK) {
                server_queue_send(srv, idx, frame_buf, frame_len);
            }
            return 0;
        }

        case CMD_REGISTER: {
            if (msg.token_count < 4) {
                return server_send_err_ref(srv, idx, msg.request_id, ERR_INVALID_PARAM,
                                           "Invalid parameters for REGISTER");
            }
            const char *username = msg.tokens[2];
            const char *password = msg.tokens[3];
            if (strlen(username) == 0 || strlen(username) >= 32 ||
                strlen(password) == 0 || strlen(password) >= 64) {
                return server_send_err_ref(srv, idx, msg.request_id, ERR_INVALID_PARAM,
                                           "Username or password length invalid");
            }

            db_request_t req;
            memset(&req, 0, sizeof(req));
            req.req_id = msg.request_id;
            req.client_idx = idx;
            req.generation = c->generation;
            req.type = DB_REQ_REGISTER;
            snprintf(req.username, sizeof(req.username), "%s", username);
            snprintf(req.password, sizeof(req.password), "%s", password);

            return server_queue_db_request(srv, &req);
        }

        case CMD_LOGIN: {
            if (msg.token_count < 4) {
                return server_send_err_ref(srv, idx, msg.request_id, ERR_INVALID_PARAM,
                                           "Invalid parameters for LOGIN");
            }
            const char *username = msg.tokens[2];
            const char *password = msg.tokens[3];
            if (strlen(username) == 0 || strlen(username) >= 32 ||
                strlen(password) == 0 || strlen(password) >= 64) {
                return server_send_err_ref(srv, idx, msg.request_id, ERR_INVALID_PARAM,
                                           "Username or password length invalid");
            }

            /* Brute-force throttling check */
            time_t now = time(NULL);
            auth_throttle_entry_t *th = server_get_throttle_entry(srv, username);
            if (th && th->lockout_until > now) {
                return server_send_err_ref(srv, idx, msg.request_id, ERR_AUTH_FAILED,
                                           "Account locked out: 5 consecutive failed attempts. Try again in 30s");
            }
            if (th && th->lockout_until > 0 && th->lockout_until <= now) {
                th->failed_attempts = 0;
                th->lockout_until = 0;
            }

            db_request_t req;
            memset(&req, 0, sizeof(req));
            req.req_id = msg.request_id;
            req.client_idx = idx;
            req.generation = c->generation;
            req.type = DB_REQ_LOGIN;
            snprintf(req.username, sizeof(req.username), "%s", username);
            snprintf(req.password, sizeof(req.password), "%s", password);

            return server_queue_db_request(srv, &req);
        }

        case CMD_CHALLENGE: {
            if (msg.token_count < 3) {
                return server_send_err_ref(srv, idx, msg.request_id, ERR_INVALID_PARAM, "Invalid parameters for CHALLENGE");
            }
            return challenge_create(srv, idx, msg.tokens[2], msg.request_id);
        }

        case CMD_CHALLENGE_RESP: {
            if (msg.token_count < 3) {
                return server_send_err_ref(srv, idx, msg.request_id, ERR_INVALID_PARAM, "Invalid parameters for CHALLENGE_RESP");
            }
            uint32_t invite_id = msg.request_id;
            bool accept = false;
            if (strcmp(msg.tokens[2], "1") == 0 || strcasecmp(msg.tokens[2], "ACCEPT") == 0) {
                accept = true;
            } else if (strcmp(msg.tokens[2], "0") == 0 || strcasecmp(msg.tokens[2], "DECLINE") == 0) {
                accept = false;
            } else {
                return server_send_err_ref(srv, idx, invite_id, ERR_INVALID_PARAM, "Invalid accept token (must be 0 or 1)");
            }
            return challenge_respond(srv, idx, invite_id, accept);
        }

        case CMD_MOVE: {
            return game_manager_handle_move(srv, idx, msg.game_id, msg.seq, msg.move_row, msg.move_col);
        }

        case CMD_RESIGN: {
            if (c->state != STATE_IN_GAME || c->current_game_id == 0) {
                return server_send_err(srv, idx, ERR_NOT_IN_GAME, "Not currently in a match");
            }
            uint32_t gid = (msg.game_id > 0) ? msg.game_id : c->current_game_id;
            if (gid != c->current_game_id) {
                return server_send_err_ref(srv, idx, gid, ERR_NOT_IN_GAME, "Not a participant in this game");
            }

            game_slot_t *slot = server_find_game(srv, gid);
            if (!slot || slot->settled) {
                return server_send_err_ref(srv, idx, gid, ERR_GAME_NOT_FOUND, "Game not active or already concluded");
            }

            int winner_idx = (idx == slot->player_x_idx) ? slot->player_o_idx : slot->player_x_idx;
            return game_manager_conclude(srv, gid, "RESIGN", "RESIGNATION", winner_idx, idx, false);
        }

        case CMD_OFFER_DRAW: {
            if (c->state != STATE_IN_GAME || c->current_game_id == 0) {
                return server_send_err(srv, idx, ERR_NOT_IN_GAME, "Not currently in a match");
            }
            uint32_t gid = (msg.game_id > 0) ? msg.game_id : c->current_game_id;
            if (gid != c->current_game_id) {
                return server_send_err_ref(srv, idx, gid, ERR_NOT_IN_GAME, "Not a participant in this game");
            }

            game_slot_t *slot = server_find_game(srv, gid);
            if (!slot || slot->settled) {
                return server_send_err_ref(srv, idx, gid, ERR_GAME_NOT_FOUND, "Game not active");
            }

            bool is_x = (idx == slot->player_x_idx);
            slot->draw_offered_by = is_x ? CELL_X : CELL_O;

            int opp_idx = is_x ? slot->player_o_idx : slot->player_x_idx;
            if (opp_idx >= 0 && opp_idx < SERVER_MAX_CLIENTS && srv->clients[opp_idx].fd >= 0) {
                char notify[64];
                snprintf(notify, sizeof(notify), "OFFER_DRAW\t%u\n", gid);
                uint8_t fbuf[128]; size_t flen = 0;
                protocol_pack_frame(notify, strlen(notify), fbuf, sizeof(fbuf), &flen);
                server_queue_send(srv, opp_idx, fbuf, flen);
            }
            return 0;
        }

        case CMD_DRAW_RESP: {
            if (c->state != STATE_IN_GAME || c->current_game_id == 0) {
                return server_send_err(srv, idx, ERR_NOT_IN_GAME, "Not currently in a match");
            }
            uint32_t gid = (msg.game_id > 0) ? msg.game_id : c->current_game_id;
            if (gid != c->current_game_id) {
                return server_send_err_ref(srv, idx, gid, ERR_NOT_IN_GAME, "Not a participant in this game");
            }

            game_slot_t *slot = server_find_game(srv, gid);
            if (!slot || slot->settled) {
                return server_send_err_ref(srv, idx, gid, ERR_GAME_NOT_FOUND, "Game not active");
            }

            bool accept = false;
            if (msg.token_count > 2) {
                const char *t = msg.tokens[2];
                if (strcmp(t, "1") == 0 || strcasecmp(t, "ACCEPT") == 0) {
                    accept = true;
                }
            }

            if (accept) {
                return game_manager_conclude(srv, gid, "DRAW", "MUTUAL_AGREEMENT", -1, -1, true);
            } else {
                slot->draw_offered_by = CELL_EMPTY;
                int opp_idx = (idx == slot->player_x_idx) ? slot->player_o_idx : slot->player_x_idx;
                if (opp_idx >= 0 && opp_idx < SERVER_MAX_CLIENTS && srv->clients[opp_idx].fd >= 0) {
                    char decl[64];
                    snprintf(decl, sizeof(decl), "DRAW_RESP\t%u\t0\n", gid);
                    uint8_t fbuf[128]; size_t flen = 0;
                    protocol_pack_frame(decl, strlen(decl), fbuf, sizeof(fbuf), &flen);
                    server_queue_send(srv, opp_idx, fbuf, flen);
                }
                return 0;
            }
        }

        case CMD_ERR: {
            const char *code_str = (msg.token_count > 1) ? msg.tokens[1] : "0";
            const char *err_text = (msg.token_count > 2) ? msg.tokens[2] : "";
            fprintf(stderr, "[-] Client [%d] reported ERR %s: %s\n", idx, code_str, err_text);
            return 0;
        }

        default: {
            printf("[*] Client [%d] sent unhandled cmd '%s'\n", idx, msg.cmd_str);
            return server_send_err_ref(srv, idx, msg.request_id, ERR_UNKNOWN_CMD, "Command not implemented");
        }
    }
}

int server_process_pending_frames(server_t *srv, int idx) {
    if (!srv || idx < 0 || idx >= SERVER_MAX_CLIENTS) return -1;
    client_conn_t *c = &srv->clients[idx];
    if (c->fd < 0) return -1;

    /* Unpack frames from accumulated rx_buf up to SERVER_FRAME_BUDGET */
    size_t frames_processed = 0;
    while (c->rx_len > 0 && frames_processed < SERVER_FRAME_BUDGET) {
        char payload[PROTO_MAX_PAYLOAD_SIZE + 1];
        size_t payload_len = 0;
        size_t consumed = 0;

        int status = protocol_unpack_frame(c->rx_buf, c->rx_len,
                                           payload, sizeof(payload),
                                           &payload_len, &consumed);
        if (status == PROTO_NEED_MORE_DATA) {
            break;
        }
        if (status != PROTO_OK) {
            fprintf(stderr, "[-] Framing violation from client %d (status=%d)\n", idx, status);
            server_disconnect_client(srv, idx, "Protocol framing violation");
            return -1;
        }

        /* Slide remaining bytes in rx_buf */
        if (consumed > 0 && consumed <= c->rx_len) {
            memmove(c->rx_buf, c->rx_buf + consumed, c->rx_len - consumed);
            c->rx_len -= consumed;
        }

        frames_processed++;
        c->last_active_ts = time(NULL);

        /* Dispatch application message */
        server_handle_client_payload(srv, idx, payload, payload_len);

        /* Check if handler disconnected the client */
        if (c->fd < 0) {
            return -1;
        }
    }

    return (int)frames_processed;
}

int server_handle_client_read(server_t *srv, int idx) {
    if (!srv || idx < 0 || idx >= SERVER_MAX_CLIENTS) return -1;
    client_conn_t *c = &srv->clients[idx];
    if (c->fd < 0) return -1;

    size_t total_read = 0;

    /* Non-blocking read until EAGAIN / EWOULDBLOCK or SERVER_READ_BUDGET_BYTES */
    while (total_read < SERVER_READ_BUDGET_BYTES) {
        if (c->rx_len >= sizeof(c->rx_buf)) {
            server_disconnect_client(srv, idx, "RX buffer full without valid frame");
            return -1;
        }

        size_t available_space = sizeof(c->rx_buf) - c->rx_len;
        size_t budget_left = SERVER_READ_BUDGET_BYTES - total_read;
        size_t to_read = (available_space < budget_left) ? available_space : budget_left;

        #if defined(_WIN32)
        int n = recv((SOCKET)c->fd, (char *)(c->rx_buf + c->rx_len),
                     (int)to_read, 0);
        #else
        ssize_t n = recv(c->fd, c->rx_buf + c->rx_len,
                         to_read, 0);
        #endif

        if (n > 0) {
            c->rx_len += (size_t)n;
            total_read += (size_t)n;
            c->last_active_ts = time(NULL);
        } else if (n == 0) {
            server_disconnect_client(srv, idx, "Client disconnected (EOF)");
            return -1;
        } else {
            #if defined(_WIN32)
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK) break;
            if (err == WSAEINTR) continue;
            #else
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            #endif
            server_disconnect_client(srv, idx, "Socket read error");
            return -1;
        }
    }

    if (server_process_pending_frames(srv, idx) < 0) {
        return -1;
    }

    return 0;
}

int server_handle_client_write(server_t *srv, int idx) {
    if (!srv || idx < 0 || idx >= SERVER_MAX_CLIENTS) return -1;
    client_conn_t *c = &srv->clients[idx];
    if (c->fd < 0) return -1;

    if (c->tx_offset >= c->tx_len) {
        c->tx_len = 0;
        c->tx_offset = 0;
        return 0;
    }

    while (c->tx_len > c->tx_offset) {
        size_t remaining = c->tx_len - c->tx_offset;
        #if defined(_WIN32)
        int flags = 0;
        int sent = send((SOCKET)c->fd, (const char *)(c->tx_buf + c->tx_offset), (int)remaining, flags);
        #else
        #if defined(MSG_NOSIGNAL)
        int flags = MSG_NOSIGNAL;
        #else
        int flags = 0;
        #endif
        ssize_t sent = send(c->fd, c->tx_buf + c->tx_offset, remaining, flags);
        #endif

        if (sent > 0) {
            c->tx_offset += (size_t)sent;
            c->last_active_ts = time(NULL);
            if (c->tx_offset >= c->tx_len) {
                c->tx_len = 0;
                c->tx_offset = 0;
                break;
            }
            break;
        } else if (sent < 0) {
            #if defined(_WIN32)
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK) break;
            if (err == WSAEINTR) continue;
            #else
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            #endif
            server_disconnect_client(srv, idx, "Socket write error");
            return -1;
        } else {
            break;
        }
    }

    return 0;
}

int server_accept_new_client(server_t *srv) {
    if (!srv || srv->listen_fd < 0) return -1;

    int accepted_count = 0;
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        #if defined(_WIN32)
        int client_fd = (int)accept((SOCKET)srv->listen_fd, (struct sockaddr *)&client_addr, &client_len);
        #else
        int client_fd = accept(srv->listen_fd, (struct sockaddr *)&client_addr, &client_len);
        #endif

        if (client_fd < 0) {
            #if defined(_WIN32)
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK) break;
            if (err == WSAEINTR) continue;
            #else
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            #endif
            perror("[-] accept error");
            break;
        }

        if (server_set_nonblocking(client_fd) < 0) {
            perror("[-] failed to set client non-blocking");
            socket_close(client_fd);
            continue;
        }

        int slot = -1;
        for (int i = 0; i < SERVER_MAX_CLIENTS; ++i) {
            if (srv->clients[i].fd < 0) {
                slot = i;
                break;
            }
        }

        if (slot >= 0) {
            client_conn_t *c = &srv->clients[slot];
            uint32_t saved_generation = c->generation + 1;
            memset(c, 0, sizeof(*c));
            c->fd = client_fd;
            c->generation = saved_generation;
            c->state = STATE_UNAUTHENTICATED;
            c->client_id = (uint32_t)(slot + 1);
            c->user_id = 0;
            c->is_guest = true;
            snprintf(c->username, sizeof(c->username), "Guest_%d", slot);
            c->current_game_id = 0;
            c->rx_len = 0;
            c->tx_len = 0;
            c->tx_offset = 0;
            c->last_active_ts = time(NULL);
            inet_ntop(AF_INET, &client_addr.sin_addr, c->ip_str, sizeof(c->ip_str));
            c->port = ntohs(client_addr.sin_port);
            srv->active_clients_count++;
            accepted_count++;
            printf("[+] Client accepted [slot %d, fd=%d, gen=%u, %s:%u, name=%s]\n",
                   slot, client_fd, c->generation, c->ip_str, c->port, c->username);

            /* Broadcast updated presence list to lobby */
            server_broadcast_online_list(srv);
        } else {
            fprintf(stderr, "[-] Server full (%d clients): rejected new connection\n",
                    SERVER_MAX_CLIENTS);
            socket_close(client_fd);
        }
    }

    return accepted_count;
}

static bool server_client_has_pending_frame(const client_conn_t *c) {
    if (!c || c->fd < 0 || c->rx_len < PROTO_HEADER_SIZE) {
        return false;
    }
    uint32_t len_be;
    memcpy(&len_be, c->rx_buf, PROTO_HEADER_SIZE);
    uint32_t payload_len = ntohl(len_be);
    if (payload_len == 0 || payload_len > PROTO_MAX_PAYLOAD_SIZE) {
        return true;
    }
    return c->rx_len >= (size_t)PROTO_HEADER_SIZE + (size_t)payload_len;
}

int server_run(server_t *srv) {
    if (!srv || srv->listen_fd < 0) return -1;

    printf("[+] Server event loop started (Max clients: %d)\n", SERVER_MAX_CLIENTS);

    while (srv->running) {
        /* Rebuild poll array: Index 0 = listen_fd, Index 1 = db_pipe[0] */
        srv->poll_fds[0].fd = srv->listen_fd;
        srv->poll_fds[0].events = POLLIN;
        srv->poll_fds[0].revents = 0;
        srv->client_map[0] = -1;

        srv->poll_fds[1].fd = srv->db_pipe[0];
        srv->poll_fds[1].events = POLLIN;
        srv->poll_fds[1].revents = 0;
        srv->client_map[1] = -2;

        nfds_t nfds = 2;

        /* Compact active clients into poll_fds starting at index 2 */
        for (int i = 0; i < SERVER_MAX_CLIENTS; ++i) {
            if (srv->clients[i].fd >= 0) {
                srv->poll_fds[nfds].fd = srv->clients[i].fd;
                srv->poll_fds[nfds].events = POLLIN;
                if (srv->clients[i].tx_len > srv->clients[i].tx_offset) {
                    srv->poll_fds[nfds].events |= POLLOUT;
                }
                srv->poll_fds[nfds].revents = 0;
                srv->client_map[nfds] = i;
                nfds++;
            }
        }
        srv->poll_count = nfds;

        int poll_timeout = 100;
        for (int i = 0; i < SERVER_MAX_CLIENTS; ++i) {
            if (srv->clients[i].fd >= 0 && server_client_has_pending_frame(&srv->clients[i])) {
                poll_timeout = 0;
                break;
            }
        }

        int ret = caro_poll(srv->poll_fds, srv->poll_count, poll_timeout);
        if (ret < 0) {
#if defined(_WIN32)
            int err = WSAGetLastError();
            if (err == WSAEINTR) continue;
#else
            if (errno == EINTR) continue;
#endif
            perror("[-] poll error");
            break;
        }

        /* Slowloris attack mitigation */
        time_t now = time(NULL);
        for (int i = 0; i < SERVER_MAX_CLIENTS; ++i) {
            client_conn_t *c = &srv->clients[i];
            if (c->fd >= 0 && c->rx_len > 0) {
                char dummy_payload[PROTO_MAX_PAYLOAD_SIZE + 1];
                size_t dummy_payload_len = 0;
                size_t dummy_consumed = 0;
                int status = protocol_unpack_frame(c->rx_buf, c->rx_len,
                                                   dummy_payload, sizeof(dummy_payload),
                                                   &dummy_payload_len, &dummy_consumed);
                if (status == PROTO_NEED_MORE_DATA) {
                    if (now - c->last_active_ts > SERVER_FRAME_TIMEOUT_SEC) {
                        printf("[-] Client slot %d timed out holding incomplete frame (%ld sec)\n",
                               i, (long)(now - c->last_active_ts));
                        server_disconnect_client(srv, i, "Incomplete frame timeout");
                    }
                }
            }
        }

        /* Check challenge invite timeouts (F15) */
        challenge_check_timeouts(srv);

        /* 1. Handle incoming connections on listener */
        if (ret > 0 && (srv->poll_fds[0].revents & POLLIN)) {
            server_accept_new_client(srv);
        }

        /* 2. Handle DB worker responses on self-pipe */
        if (ret > 0 && (srv->poll_fds[1].revents & POLLIN)) {
            server_handle_db_responses(srv);
        }

        /* 3. Handle I/O events on active client sockets (indices 2 .. poll_count - 1) */
        bool client_read_handled[SERVER_MAX_CLIENTS] = {false};

        if (ret > 0) {
            for (nfds_t p = 2; p < srv->poll_count; ++p) {
                int idx = srv->client_map[p];
                if (idx < 0 || idx >= SERVER_MAX_CLIENTS) continue;
                client_conn_t *c = &srv->clients[idx];
                if (c->fd < 0) continue;

                short revents = srv->poll_fds[p].revents;
                if (revents == 0) continue;

                if (revents & (POLLERR | POLLNVAL)) {
                    server_disconnect_client(srv, idx, "Socket poll error (POLLERR/POLLNVAL)");
                    continue;
                }

                if (revents & (POLLIN | POLLHUP)) {
                    server_handle_client_read(srv, idx);
                    client_read_handled[idx] = true;
                    if (c->fd < 0) continue;
                }

                if (revents & POLLOUT) {
                    server_handle_client_write(srv, idx);
                }
            }
        }

        /* 4. Process pending complete frames */
        for (int i = 0; i < SERVER_MAX_CLIENTS; ++i) {
            if (srv->clients[i].fd >= 0 && !client_read_handled[i]) {
                if (server_client_has_pending_frame(&srv->clients[i])) {
                    server_process_pending_frames(srv, i);
                }
            }
        }
    }

    return 0;
}
