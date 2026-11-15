#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include "server.h"

static server_t g_server;

static void handle_signal(int sig) {
    (void)sig;
    printf("\n[*] Received shutdown signal, stopping server...\n");
    server_stop(&g_server);
}

int main(int argc, char *argv[]) {
    int port = DEFAULT_SERVER_PORT;
    const char *host = DEFAULT_SERVER_HOST;
    bool test_mode = false;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-h") == 0 && i + 1 < argc) {
            host = argv[++i];
        } else if (strcmp(argv[i], "--test") == 0) {
            test_mode = true;
        } else if (strcmp(argv[i], "--help") == 0) {
            printf("Caro Server (C/Linux)\n");
            printf("Usage: %s [-h host] [-p port] [--test]\n", argv[0]);
            return 0;
        }
    }

    printf("=========================================\n");
    printf("     CARO ONLINE TCP SERVER - SKELETON   \n");
    printf("=========================================\n");

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    if (server_init(&g_server, host, port) != 0) {
        fprintf(stderr, "[-] Failed to initialize server\n");
        return 1;
    }

    if (test_mode) {
        printf("[+] Skeleton verification mode: Server initialized successfully!\n");
        server_cleanup(&g_server);
        return 0;
    }

    server_run(&g_server);
    server_cleanup(&g_server);

    return 0;
}
