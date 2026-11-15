#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
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
    strncpy(srv->host, (host && *host) ? host : DEFAULT_SERVER_HOST, sizeof(srv->host) - 1);

    for (size_t i = 0; i < SERVER_MAX_CLIENTS; ++i) {
        srv->clients[i].fd = -1;
        srv->clients[i].state = CLIENT_STATE_DISCONNECTED;
    }

#if defined(_WIN32)
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "[-] WSAStartup failed\n");
        return -1;
    }
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

    printf("[+] Caro Server initialized on %s:%d (listen_fd=%d)\n", srv->host, srv->port, srv->listen_fd);
    return 0;
}

void server_stop(server_t *srv) {
    if (srv) {
        srv->running = false;
    }
}

void server_cleanup(server_t *srv) {
    if (!srv) return;

    for (size_t i = 0; i < SERVER_MAX_CLIENTS; ++i) {
        if (srv->clients[i].fd >= 0) {
            socket_close(srv->clients[i].fd);
            srv->clients[i].fd = -1;
            srv->clients[i].state = CLIENT_STATE_DISCONNECTED;
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

int server_run(server_t *srv) {
    if (!srv || srv->listen_fd < 0) return -1;

    printf("[+] Server event loop ready (Max clients: %d)\n", SERVER_MAX_CLIENTS);

#if !defined(_WIN32)
    while (srv->running) {
        srv->poll_fds[0].fd = srv->listen_fd;
        srv->poll_fds[0].events = POLLIN;
        srv->poll_fds[0].revents = 0;
        nfds_t nfds = 1;

        for (size_t i = 0; i < SERVER_MAX_CLIENTS; ++i) {
            if (srv->clients[i].fd >= 0) {
                srv->poll_fds[nfds].fd = srv->clients[i].fd;
                srv->poll_fds[nfds].events = POLLIN;
                if (srv->clients[i].tx_len > srv->clients[i].tx_sent) {
                    srv->poll_fds[nfds].events |= POLLOUT;
                }
                srv->poll_fds[nfds].revents = 0;
                nfds++;
            }
        }
        srv->poll_count = nfds;

        int ret = poll(srv->poll_fds, srv->poll_count, 100); /* 100ms timeout for responsive exit */
        if (ret < 0) {
            if (errno == EINTR) continue;
            perror("[-] poll error");
            break;
        }

        if (ret == 0) continue;

        /* Accept new connections */
        if (srv->poll_fds[0].revents & POLLIN) {
            struct sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);
            int client_fd = (int)accept(srv->listen_fd, (struct sockaddr *)&client_addr, &client_len);
            if (client_fd >= 0) {
                if (server_set_nonblocking(client_fd) < 0) {
                    perror("[-] set non-blocking on client failed");
                    socket_close(client_fd);
                } else {
                    int slot = -1;
                    for (size_t i = 0; i < SERVER_MAX_CLIENTS; ++i) {
                        if (srv->clients[i].fd < 0) {
                            slot = (int)i;
                            break;
                        }
                    }
                    if (slot >= 0) {
                        srv->clients[slot].fd = client_fd;
                        srv->clients[slot].state = CLIENT_STATE_CONNECTED;
                        srv->clients[slot].client_id = (uint32_t)(slot + 1);
                        srv->clients[slot].rx_len = 0;
                        srv->clients[slot].tx_len = 0;
                        srv->clients[slot].tx_sent = 0;
                        inet_ntop(AF_INET, &client_addr.sin_addr, srv->clients[slot].ip_str, sizeof(srv->clients[slot].ip_str));
                        srv->clients[slot].port = ntohs(client_addr.sin_port);
                        srv->active_clients_count++;
                        printf("[+] Client accepted [slot %d, fd=%d, %s:%u]\n",
                               slot, client_fd, srv->clients[slot].ip_str, srv->clients[slot].port);
                    } else {
                        fprintf(stderr, "[-] Server full: rejected connection\n");
                        socket_close(client_fd);
                    }
                }
            }
        }
    }
#else
    printf("[*] Windows stub event loop (runs cleanly, stop immediately for skeleton verification)\n");
#endif

    return 0;
}
