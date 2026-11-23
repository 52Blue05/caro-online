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
    }

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

    /* Push presence list to clients active in lobby or authenticated */
    for (int i = 0; i < SERVER_MAX_CLIENTS; ++i) {
        if (srv->clients[i].fd >= 0 &&
            (srv->clients[i].state == STATE_AUTHENTICATED || srv->clients[i].state == STATE_IN_LOBBY)) {
            server_queue_send(srv, i, frame_buf, frame_len);
        }
    }

    return 0;
}

void server_disconnect_client(server_t *srv, int idx, const char *reason) {
    if (!srv || idx < 0 || idx >= SERVER_MAX_CLIENTS) return;
    client_conn_t *c = &srv->clients[idx];
    if (c->fd < 0) return;

    printf("[-] Client disconnected [slot %d, fd=%d, gen=%u, %s:%u]: %s\n",
           idx, c->fd, c->generation, c->ip_str, c->port, reason ? reason : "Unknown reason");

    /* If client was in an active game, handle opponent notification cleanly */
    if (c->current_game_id > 0) {
        uint32_t gid = c->current_game_id;
        c->current_game_id = 0;
        for (int i = 0; i < SERVER_MAX_CLIENTS; ++i) {
            if (i != idx && srv->clients[i].fd >= 0 && srv->clients[i].current_game_id == gid) {
                printf("[*] Opponent in slot %d notified of game %u abort due to disconnect\n", i, gid);
                server_send_err(srv, i, ERR_GAME_NOT_FOUND, "Opponent disconnected");
                srv->clients[i].current_game_id = 0;
                if (srv->clients[i].state == STATE_IN_GAME) {
                    srv->clients[i].state = STATE_IN_LOBBY;
                }
            }
        }
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
        fprintf(stderr, "[-] Invalid message syntax from client %d: %s\n", idx, payload);
        return server_send_err(srv, idx, ERR_BAD_FRAME, "Invalid protocol format");
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
