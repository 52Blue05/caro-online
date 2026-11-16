#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
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

int server_init(server_t *srv, const char *host, int port) {
    if (!srv) return -1;
    memset(srv, 0, sizeof(*srv));
    srv->listen_fd = -1;
    srv->running = false;
    srv->port = (port > 0) ? port : DEFAULT_SERVER_PORT;

    const char *bind_host = (host && *host) ? host : DEFAULT_SERVER_HOST;
    snprintf(srv->host, sizeof(srv->host), "%s", bind_host);

    for (size_t i = 0; i < SERVER_MAX_CLIENTS; ++i) {
        srv->clients[i].fd = -1;
        srv->clients[i].state = STATE_DISCONNECTED;
    }

#if defined(_WIN32)
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "[-] WSAStartup failed\n");
        return -1;
    }
#else
    /* Ignore SIGPIPE so writes to closed sockets do not terminate server */
    signal(SIGPIPE, SIG_IGN);
#endif

    int listen_fd = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("[-] socket creation failed");
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
        return -1;
    }

    if (listen(listen_fd, SERVER_LISTEN_BACKLOG) < 0) {
        perror("[-] listen failed");
        socket_close(listen_fd);
        return -1;
    }

    srv->listen_fd = listen_fd;
    srv->running = true;

    printf("[+] Caro Server initialized on %s:%d (listen_fd=%d)\n",
           srv->host, srv->port, srv->listen_fd);
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

    /* Push presence list only to clients active in lobby or authenticated */
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

    printf("[-] Client disconnected [slot %d, fd=%d, %s:%u]: %s\n",
           idx, c->fd, c->ip_str, c->port, reason ? reason : "Unknown reason");

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

    for (int i = 0; i < SERVER_MAX_CLIENTS; ++i) {
        if (srv->clients[i].fd >= 0) {
            server_disconnect_client(srv, i, "Server shutting down");
        }
    }

    if (srv->listen_fd >= 0) {
        socket_close(srv->listen_fd);
        srv->listen_fd = -1;
    }

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

    /* If space is tight and there is dead space before tx_offset, compact unsent data once */
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
    if (!srv || idx < 0 || idx >= SERVER_MAX_CLIENTS) return -1;

    char err_payload[PROTO_MAX_PAYLOAD_SIZE];
    protocol_create_err(err_code, reason, err_payload, sizeof(err_payload));

    uint8_t frame_buf[PROTO_MAX_PAYLOAD_SIZE + PROTO_HEADER_SIZE];
    size_t frame_len = 0;
    int pack_res = protocol_pack_frame(err_payload, strlen(err_payload),
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

        case CMD_ERR: {
            const char *code_str = (msg.token_count > 1) ? msg.tokens[1] : "0";
            const char *err_text = (msg.token_count > 2) ? msg.tokens[2] : "";
            fprintf(stderr, "[-] Client [%d] reported ERR %s: %s\n", idx, code_str, err_text);
            return 0;
        }

        default: {
            printf("[*] Client [%d] sent unhandled cmd '%s' (not in Week 1 scope)\n",
                   idx, msg.cmd_str);
            return server_send_err(srv, idx, ERR_UNKNOWN_CMD, "Command not implemented in Week 1");
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
            /* Partial frame, wait for subsequent socket reads */
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
            /* Client performed orderly shutdown (EOF) */
            server_disconnect_client(srv, idx, "Client disconnected (EOF)");
            return -1;
        } else {
            #if defined(_WIN32)
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK) {
                break; /* Finished reading all available bytes for now */
            }
            if (err == WSAEINTR) {
                continue; /* Interrupted by signal, retry */
            }
            #else
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break; /* Finished reading all available bytes for now */
            }
            if (errno == EINTR) {
                continue; /* Interrupted by signal, retry */
            }
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
                /* All queued data successfully sent */
                c->tx_len = 0;
                c->tx_offset = 0;
                break;
            }
            /* Partial write: tx_offset is updated, await next POLLOUT without memmove */
            break;
        } else if (sent < 0) {
            #if defined(_WIN32)
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK) {
                break; /* Output buffer temporarily full, wait for next POLLOUT */
            }
            if (err == WSAEINTR) {
                continue; /* Interrupted, retry */
            }
            #else
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break; /* Output buffer temporarily full, wait for next POLLOUT */
            }
            if (errno == EINTR) {
                continue; /* Interrupted, retry */
            }
            #endif
            server_disconnect_client(srv, idx, "Socket write error");
            return -1;
        } else {
            /* sent == 0, treat as would block */
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
            if (err == WSAEWOULDBLOCK) {
                break; /* All pending connections accepted */
            }
            if (err == WSAEINTR) continue;
            #else
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break; /* All pending connections accepted */
            }
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
            memset(c, 0, sizeof(*c));
            c->fd = client_fd;
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
            printf("[+] Client accepted [slot %d, fd=%d, %s:%u, name=%s]\n",
                   slot, client_fd, c->ip_str, c->port, c->username);

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
        return true; /* Invalid frame: ready to unpack and reject */
    }
    return c->rx_len >= (size_t)PROTO_HEADER_SIZE + (size_t)payload_len;
}

int server_run(server_t *srv) {
    if (!srv || srv->listen_fd < 0) return -1;

    printf("[+] Server event loop started (Max clients: %d)\n", SERVER_MAX_CLIENTS);

    while (srv->running) {
        /* Rebuild poll array: index 0 is listen_fd */
        srv->poll_fds[0].fd = srv->listen_fd;
        srv->poll_fds[0].events = POLLIN;
        srv->poll_fds[0].revents = 0;
        srv->client_map[0] = -1;
        nfds_t nfds = 1;

        /* Compact active clients into poll_fds */
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

        /* Check if any client has pending complete frames waiting to be processed */
        int poll_timeout = 100; /* 100ms default timeout for graceful shutdown */
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

        /* Check for frame timeout on clients holding incomplete frames (Slowloris attack mitigation) */
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

        /* 2. Handle I/O events on active client sockets */
        bool client_read_handled[SERVER_MAX_CLIENTS] = {false};

        if (ret > 0) {
            for (nfds_t p = 1; p < srv->poll_count; ++p) {
                int idx = srv->client_map[p];
                if (idx < 0 || idx >= SERVER_MAX_CLIENTS) continue;
                client_conn_t *c = &srv->clients[idx];
                if (c->fd < 0) continue;

                short revents = srv->poll_fds[p].revents;
                if (revents == 0) continue;

                /* Check socket errors */
                if (revents & (POLLERR | POLLNVAL)) {
                    server_disconnect_client(srv, idx, "Socket poll error (POLLERR/POLLNVAL)");
                    continue;
                }

                /* Read ready or hung up */
                if (revents & (POLLIN | POLLHUP)) {
                    server_handle_client_read(srv, idx);
                    client_read_handled[idx] = true;
                    if (c->fd < 0) continue; /* Client was disconnected in read handler */
                }

                /* Write ready */
                if (revents & POLLOUT) {
                    server_handle_client_write(srv, idx);
                }
            }
        }

        /* 3. Process pending complete frames for clients whose sockets were not read this tick */
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
