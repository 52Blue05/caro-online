#ifndef SERVER_H
#define SERVER_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

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

/* Client connection states */
typedef enum {
    STATE_DISCONNECTED     = 0,
    STATE_UNAUTHENTICATED  = 1,
    STATE_AUTHENTICATED    = 2,
    STATE_IN_LOBBY         = 3,
    STATE_IN_GAME          = 4
} client_state_t;

/* Backwards compatibility aliases */
#define CLIENT_STATE_DISCONNECTED   STATE_DISCONNECTED
#define CLIENT_STATE_CONNECTED      STATE_UNAUTHENTICATED
#define CLIENT_STATE_GUEST          STATE_UNAUTHENTICATED
#define CLIENT_STATE_AUTHENTICATED  STATE_AUTHENTICATED
#define CLIENT_STATE_IN_LOBBY       STATE_IN_LOBBY
#define CLIENT_STATE_IN_GAME        STATE_IN_GAME

/* Per-client connection context */
typedef struct {
    int             fd;                         /* Socket file descriptor (-1 if empty slot) */
    client_state_t  state;                      /* Connection state */
    char            ip_str[INET6_ADDRSTRLEN];   /* Client IP string */
    uint16_t        port;                       /* Client port */
    
    /* RX Buffer for accumulated framing stream */
    uint8_t         rx_buf[PROTO_MAX_FRAME_BUFFER];
    size_t          rx_len;                     /* Current unconsumed bytes in rx_buf */
    
    /* TX Buffer for outgoing framed data */
    uint8_t         tx_buf[PROTO_MAX_FRAME_BUFFER];
    size_t          tx_len;                     /* Current bytes queued in tx_buf */

    /* User identity & session state */
    uint32_t        client_id;                  /* Server slot / session sequential ID */
    uint32_t        user_id;                    /* Authenticated account ID (0 if guest) */
    char            username[32];               /* Logged in username */
    uint32_t        current_game_id;            /* Active game ID (0 if none) */
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

    /* Pollfd array: index 0 is listen_fd, 1..SERVER_MAX_CLIENTS are active clients */
    caro_pollfd_t   poll_fds[SERVER_MAX_CLIENTS + 1];
    nfds_t          poll_count;
    int             client_map[SERVER_MAX_CLIENTS + 1]; /* Maps poll_fds index -> clients[] index */
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

/**
 * @brief Safely close client socket, release resources, and clear slot.
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
 * 
 * @param srv Pointer to server_t instance
 * @param idx Client index
 * @param err_code Standard error code from protocol_err_t
 * @param reason Optional custom error message string (if NULL, default is used)
 * @return 0 on success, negative on error.
 */
int server_send_err(server_t *srv, int idx, int err_code, const char *reason);

/**
 * @brief Handle POLLIN event on client socket (read non-blocking and unpack frames).
 * 
 * @param srv Pointer to server_t instance
 * @param idx Client index
 * @return 0 on success, negative on disconnect/error.
 */
int server_handle_client_read(server_t *srv, int idx);

/**
 * @brief Handle POLLOUT event on client socket (send queued tx_buf data non-blocking).
 * 
 * @param srv Pointer to server_t instance
 * @param idx Client index
 * @return 0 on success, negative on disconnect/error.
 */
int server_handle_client_write(server_t *srv, int idx);

/**
 * @brief Dispatch unpacked payload message for a client.
 * 
 * @param srv Pointer to server_t instance
 * @param idx Client index
 * @param payload Extracted payload string
 * @param len Payload length
 * @return 0 on success, negative on error.
 */
int server_handle_client_payload(server_t *srv, int idx, const char *payload, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* SERVER_H */
