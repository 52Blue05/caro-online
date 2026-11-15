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

/* Process incoming server messages */
static void client_display_server_message(const char *payload, size_t len) {
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
            const char *code_str = (msg.token_count > 1) ? msg.tokens[1] : "0";
            const char *err_text = (msg.token_count > 2) ? msg.tokens[2] : "";
            printf("\n[SERVER ERROR %s] %s\n", code_str, err_text);
            break;
        }

        case CMD_OK: {
            const char *req_id = (msg.token_count > 1) ? msg.tokens[1] : "";
            printf("\n[SERVER] OK (request_id=%s)\n", req_id);
            break;
        }

        default: {
            printf("\n[SERVER] %s\n", msg.raw_payload);
            break;
        }
    }
}

/* Batch Ping Mode: Send PING, wait for PONG, verify, and exit */
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
        }
    }

    socket_close(sock_fd);
    return pong_received ? exit_code : 1;
}

/* Print help text for interactive commands */
static void print_interactive_help(void) {
    printf("=========================================\n");
    printf("     CARO ONLINE CLIENT COMMANDS         \n");
    printf("=========================================\n");
    printf("  ping <message>  : Send PING frame to server and receive PONG\n");
    printf("  help            : Show this command list\n");
    printf("  quit / exit     : Disconnect from server and exit\n");
    printf("=========================================\n");
}

/* Dispatch a command line entered by user */
static int client_dispatch_user_input(int sock_fd, char *line, bool *out_running) {
    char *cmd = trim_whitespace(line);
    if (*cmd == '\0') {
        return 0;
    }

    if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "exit") == 0) {
        printf("[*] Disconnecting from server and exiting...\n");
        *out_running = false;
        return 0;
    }

    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
        print_interactive_help();
        return 0;
    }

    if (strncmp(cmd, "ping", 4) == 0 && (cmd[4] == ' ' || cmd[4] == '\t' || cmd[4] == '\0')) {
        char *arg = cmd + 4;
        while (*arg && isspace((unsigned char)*arg)) arg++;
        const char *ping_msg = (*arg != '\0') ? arg : "hello";

        char payload[PROTO_MAX_PAYLOAD_SIZE];
        protocol_create_ping(ping_msg, payload, sizeof(payload));

        if (client_send_msg(sock_fd, payload) == 0) {
            printf("[*] Sent PING: '%s' -> Waiting for PONG...\n", ping_msg);
        } else {
            fprintf(stderr, "[-] Failed to send PING\n");
        }
        return 0;
    }

    printf("[!] Unknown command: '%s'. Type 'help' for available commands.\n", cmd);
    return 0;
}

/* Interactive Event Loop with poll() multiplexing stdin and socket */
static int client_run_interactive_loop(const char *host, int port) {
    printf("=========================================\n");
    printf("     CARO ONLINE CLI CLIENT (C/Linux)    \n");
    printf("=========================================\n");
    printf("[*] Connecting to server at %s:%d...\n", host, port);

    int sock_fd = client_connect_tcp(host, port);
    if (sock_fd < 0) {
        return 1;
    }

    printf("[+] Connected successfully to server!\n");
    printf("[*] Type 'help' for commands, 'quit' to exit.\n");
    printf("caro> ");
    fflush(stdout);

    uint8_t rx_buf[PROTO_MAX_FRAME_BUFFER];
    size_t rx_len = 0;
    bool running = true;

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

            /* Shift remaining bytes in stdin_buf */
            size_t consumed = line_len + 1;
            memmove(stdin_buf, stdin_buf + consumed, stdin_len - consumed);
            stdin_len -= consumed;

            client_dispatch_user_input(sock_fd, line, &running);
            if (running) {
                printf("caro> ");
                fflush(stdout);
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

                    client_display_server_message(payload, payload_len);

                    memmove(rx_buf, rx_buf + consumed, rx_len - consumed);
                    rx_len -= consumed;

                    if (running) {
                        printf("caro> ");
                        fflush(stdout);
                    }
                }
            } else if (n == 0) {
                printf("\n[!] Server closed the connection (EOF).\n");
                running = false;
                break;
            } else {
                if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                    perror("\n[-] Socket read error");
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
                /* Next iteration will extract and process lines from stdin_buf */
            } else if (n == 0) {
                /* Stdin closed (pipe finished or Ctrl+D) */
                if (stdin_len > 0) {
                    char line[1024];
                    size_t copy_len = (stdin_len < sizeof(line) - 1) ? stdin_len : sizeof(line) - 1;
                    memcpy(line, stdin_buf, copy_len);
                    line[copy_len] = '\0';
                    stdin_len = 0;
                    client_dispatch_user_input(sock_fd, line, &running);
                }
                printf("\n[*] Stdin EOF detected. Exiting...\n");
                running = false;
                break;
            }
        }
#else
        /* Windows fallback: poll socket, handle console input */
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

                    client_display_server_message(payload, payload_len);
                    memmove(rx_buf, rx_buf + consumed, rx_len - consumed);
                    rx_len -= consumed;

                    if (running) {
                        printf("caro> ");
                        fflush(stdout);
                    }
                }
            } else if (n == 0) {
                printf("\n[!] Server closed connection (EOF).\n");
                running = false;
                break;
            }
        }

        /* Check console input if running in terminal */
        if (_kbhit()) {
            char line[1024];
            if (fgets(line, sizeof(line), stdin) != NULL) {
                client_dispatch_user_input(sock_fd, line, &running);
                if (running) {
                    printf("caro> ");
                    fflush(stdout);
                }
            }
        }
#endif
    }

    printf("[*] Closing client socket...\n");
    socket_close(sock_fd);
    printf("[+] Client exited cleanly.\n");
    return 0;
}

/* Quick Client Self-Test */
static int client_run_self_test(void) {
    printf("=========================================\n");
    printf("     CARO CLIENT SELF-TEST (T1H3)        \n");
    printf("=========================================\n");

    /* Test 1: PING packing helper */
    char ping_payload[PROTO_MAX_PAYLOAD_SIZE];
    int res = protocol_create_ping("selftest_123", ping_payload, sizeof(ping_payload));
    if (res != PROTO_OK || strcmp(ping_payload, "PING\tselftest_123\n") != 0) {
        fprintf(stderr, "[-] Test 1 failed: protocol_create_ping\n");
        return 1;
    }
    printf("[+] Test 1 passed: protocol_create_ping produces correct wire string\n");

    /* Test 2: Frame packing and unpacking */
    uint8_t frame_buf[PROTO_MAX_FRAME_BUFFER];
    size_t frame_len = 0;
    res = protocol_pack_frame(ping_payload, strlen(ping_payload), frame_buf, sizeof(frame_buf), &frame_len);
    if (res != PROTO_OK || frame_len != PROTO_HEADER_SIZE + strlen(ping_payload)) {
        fprintf(stderr, "[-] Test 2 failed: protocol_pack_frame\n");
        return 1;
    }
    printf("[+] Test 2 passed: protocol_pack_frame generates valid 4-byte header\n");

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

    printf("=========================================\n");
    printf("[+] All client self-tests passed (4/4)\n");
    printf("=========================================\n");
    return 0;
}

static void print_usage(const char *prog_name) {
    printf("Caro CLI Client (C/Linux)\n");
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
