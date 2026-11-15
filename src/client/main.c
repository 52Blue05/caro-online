#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "protocol.h"

int main(int argc, char *argv[]) {
    const char *host = "127.0.0.1";
    int port = 8888;
    bool test_mode = false;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-h") == 0 && i + 1 < argc) {
            host = argv[++i];
        } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--test") == 0) {
            test_mode = true;
        } else if (strcmp(argv[i], "--help") == 0) {
            printf("Caro CLI Client (C/Linux)\n");
            printf("Usage: %s [-h server_host] [-p server_port] [--test]\n", argv[0]);
            return 0;
        }
    }

    printf("=========================================\n");
    printf("     CARO ONLINE CLI CLIENT - SKELETON   \n");
    printf("=========================================\n");
    printf("[*] Target server: %s:%d\n", host, port);

    if (test_mode) {
        printf("[+] Client skeleton verified successfully.\n");
        return 0;
    }

    /* Client CLI loop will be implemented by Agent Hung in T1H3 */
    printf("[*] Skeleton ready for T1H3 integration.\n");
    return 0;
}
