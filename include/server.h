#ifndef SERVER_H
#define SERVER_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#if defined(_WIN32)
    #include <winsock2.h>
    #include <ws2tcpip.h>
    typedef int socklen_t;
    #define socket_close closesocket
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
#endif

#include "protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Default Server Configuration */
#define DEFAULT_SERVER_PORT      8888
#define DEFAULT_SERVER_HOST      "0.0.0.0"
#define SERVER_MAX_CLIENTS       64
#define SERVER_LISTEN_BACKLOG    16
#define SERVER_RX_BUFFER_SIZE    65536
#define SERVER_TX_BUFFER_SIZE    65536

/* Client connection states */
typedef enum {
    CLIENT_STATE_DISCONNECTED = 0,
    CLIENT_STATE_CONNECTED    = 1,    /* Raw socket accepted */
    CLIENT_STATE_GUEST        = 2,    /* Connected, unauthenticated */
    CLIENT_STATE_AUTHENTICATED= 3,    /* Logged in with valid account */
    CLIENT_STATE_IN_LOBBY     = 4,    /* In lobby, available for challenge */
    CLIENT_STATE_IN_GAME      = 5     /* Playing a game */
} client_state_t;

/* Per-client connection context */
typedef struct {
    int             fd;                         /* Socket file descriptor (-1 if empty) */
    client_state_t  state;                      /* Connection state */
    char            ip_str[INET6_ADDRSTRLEN];   /* Client IP string */
    uint16_t        port;                       /* Client port */
    
    /* RX Buffer for framing stream */
    uint8_t         rx_buf[SERVER_RX_BUFFER_SIZE];
    size_t          rx_len;
    
    /* TX Buffer for outgoing frames */
    uint8_t         tx_buf[SERVER_TX_BUFFER_SIZE];
    size_t          tx_len;
    size_t          tx_sent;                    /* Bytes already sent */

    /* User identity (for future auth & lobby integration) */
    uint32_t        client_id;
    uint32_t        user_id;
    char            username[32];
    uint32_t        current_game_id;
} client_conn_t;

/* Server master structure */
typedef struct {
    int             listen_fd;
    char            host[64];
    int             port;
    volatile bool   running;

    /* Clients tracking */
    client_conn_t   clients[SERVER_MAX_CLIENTS];
    size_t          active_clients_count;

#if !defined(_WIN32)
    /* Pollfd array: index 0 is listen_fd, 1..SERVER_MAX_CLIENTS are clients */
    struct pollfd   poll_fds[SERVER_MAX_CLIENTS + 1];
    nfds_t          poll_count;
#endif
} server_t;

/**
 * @brief Initialize server structure, bind to host:port and listen non-blocking.
 * 
 * @param srv Pointer to server_t instance
 * @param host Bind address (e.g., "0.0.0.0")
 * @param port Port number (e.g., 8888)
 * @return 0 on success, negative error code on failure.
 */
int server_init(server_t *srv, const char *host, int port);

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
 * @brief Close all open client sockets and listener fd, free server resources.
 * 
 * @param srv Pointer to server_t instance
 */
void server_cleanup(server_t *srv);

#ifdef __cplusplus
}
#endif

#endif /* SERVER_H */