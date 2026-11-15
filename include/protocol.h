#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#if defined(_WIN32)
    #include <winsock2.h>
    #include <ws2tcpip.h>
#else
    #include <arpa/inet.h>
    #include <sys/types.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Framing & Buffer Constants */
#define PROTO_HEADER_SIZE        4
#define PROTO_MAX_PAYLOAD_SIZE   4096
#define PROTO_MAX_FRAME_BUFFER   65536
#define PROTO_MAX_TOKENS         16
#define PROTO_MAX_TOKEN_LEN      256

/* Status codes for unpack / framing */
typedef enum {
    PROTO_OK                     =  0,
    PROTO_NEED_MORE_DATA         =  1,
    PROTO_ERR_INVALID_HEADER     = -1,
    PROTO_ERR_PAYLOAD_TOO_LARGE  = -2,
    PROTO_ERR_INVALID_FRAME      = -3,
    PROTO_ERR_BUFFER_OVERFLOW    = -4,
    PROTO_ERR_INVALID_PARAM      = -5
} proto_status_t;

/* Standard Application Error Codes */
typedef enum {
    ERR_OK                       = 0,
    /* Framing & Syntax (100 - 199) */
    ERR_BAD_FRAME                = 100,
    ERR_UNKNOWN_CMD              = 101,
    ERR_INVALID_PARAM            = 102,
    ERR_PAYLOAD_TOO_LARGE        = 103,
    /* Authentication & Account (200 - 299) */
    ERR_UNAUTHORIZED             = 200,
    ERR_AUTH_FAILED              = 201,
    ERR_USER_EXISTS              = 202,
    ERR_ALREADY_LOGGED_IN        = 203,
    /* Lobby & Presence (300 - 399) */
    ERR_USER_NOT_FOUND           = 300,
    ERR_USER_BUSY                = 301,
    ERR_INVITE_EXPIRED           = 302,
    ERR_INVITE_INVALID           = 303,
    /* Gameplay & Rules (400 - 499) */
    ERR_NOT_IN_GAME              = 400,
    ERR_NOT_YOUR_TURN            = 401,
    ERR_INVALID_MOVE             = 402,
    ERR_SEQ_MISMATCH             = 403,
    ERR_GAME_NOT_FOUND           = 404,
    /* Server Internal (500 - 599) */
    ERR_INTERNAL_SERVER          = 500,
    ERR_DATABASE                 = 501
} protocol_err_t;

/* Command Types */
typedef enum {
    CMD_UNKNOWN = 0,
    /* Week 1 Core */
    CMD_PING,
    CMD_PONG,
    CMD_ERR,
    /* Reserved for future weeks */
    CMD_REGISTER,
    CMD_LOGIN,
    CMD_OK,
    CMD_GET_ONLINE,
    CMD_ONLINE_LIST,
    CMD_CHALLENGE,
    CMD_CHALLENGE_REQ,
    CMD_CHALLENGE_RESP,
    CMD_GAME_START,
    CMD_MOVE,
    CMD_MOVE_ACK,
    CMD_MOVE_BROADCAST,
    CMD_RESIGN,
    CMD_OFFER_DRAW,
    CMD_DRAW_RESP,
    CMD_GAME_OVER,
    CMD_GET_LEADERBOARD,
    CMD_GET_GAME_LOG
} proto_cmd_t;

/* Parsed Protocol Message */
typedef struct {
    proto_cmd_t cmd;
    char        cmd_str[32];
    int         token_count;
    char        tokens[PROTO_MAX_TOKENS][PROTO_MAX_TOKEN_LEN];
    /* Extended fields (0 if not applicable) */
    uint32_t    request_id;
    uint32_t    game_id;
    uint32_t    seq;
    /* Raw payload copy (null-terminated, without trailing '\n') */
    char        raw_payload[PROTO_MAX_PAYLOAD_SIZE + 1];
    size_t      raw_payload_len;
} protocol_msg_t;

/**
 * @brief Pack text payload into a binary frame with 4-byte big-endian header.
 * 
 * Ensures payload ends with '\n'. Writes (4 + payload_len) bytes into out_buf.
 * 
 * @param payload Null-terminated string or buffer containing payload text
 * @param payload_len Length of payload text (if 0 and payload != NULL, strlen is used)
 * @param out_buf Output buffer for framed data
 * @param max_out Capacity of out_buf (must be >= 4 + payload_len + (needs_newline ? 1 : 0))
 * @param out_len Pointer to store total framed bytes written (header + payload)
 * @return PROTO_OK on success, negative error code on failure.
 */
int protocol_pack_frame(const char *payload, size_t payload_len,
                        uint8_t *out_buf, size_t max_out, size_t *out_len);

/**
 * @brief Attempt to unpack a complete frame from an accumulated input buffer.
 * 
 * Handles partial frames and multiple coalesced frames.
 * If input contains less than 4 bytes, returns PROTO_NEED_MORE_DATA.
 * If input contains 4 bytes header but fewer than (4 + payload_len) bytes,
 * returns PROTO_NEED_MORE_DATA.
 * 
 * When a full frame is available:
 * - Copies payload into out_payload and appends '\0'.
 * - Sets *out_payload_len to payload length.
 * - Sets *consumed_bytes to (4 + payload_len). Caller must advance buffer by this amount.
 * 
 * @param in_buf Pointer to accumulated raw bytes from socket
 * @param in_len Total number of bytes currently in in_buf
 * @param out_payload Buffer to receive extracted payload text
 * @param max_payload Capacity of out_payload
 * @param out_payload_len Pointer to receive length of payload (excluding '\0')
 * @param consumed_bytes Pointer to receive total bytes consumed from in_buf
 * @return PROTO_OK on success, PROTO_NEED_MORE_DATA if incomplete, negative error code on invalid data.
 */
int protocol_unpack_frame(const uint8_t *in_buf, size_t in_len,
                          char *out_payload, size_t max_payload,
                          size_t *out_payload_len, size_t *consumed_bytes);

/**
 * @brief Tokenize a payload string delimited by TAB ('\t').
 * 
 * Strips optional trailing '\r' and '\n'.
 * 
 * @param payload Null-terminated payload string
 * @param payload_len Length of payload
 * @param tokens 2D array to store extracted tokens
 * @param token_count Pointer to receive number of tokens extracted
 * @return PROTO_OK on success, negative error code on failure.
 */
int protocol_tokenize(const char *payload, size_t payload_len,
                      char tokens[PROTO_MAX_TOKENS][PROTO_MAX_TOKEN_LEN],
                      int *token_count);

/**
 * @brief Parse extracted payload into a structured protocol_msg_t.
 * 
 * Tokenizes by TAB, identifies command enum, and parses extended fields
 * (request_id, game_id, seq) when present.
 * 
 * @param payload Extracted payload string
 * @param payload_len Length of payload
 * @param msg Pointer to output message struct
 * @return PROTO_OK on success, negative error code on failure.
 */
int protocol_parse_msg(const char *payload, size_t payload_len, protocol_msg_t *msg);

/**
 * @brief Helper to format a PING payload: "PING\t<echo_str>\n"
 */
int protocol_create_ping(const char *echo_str, char *out_payload, size_t max_out);

/**
 * @brief Helper to format a PONG payload: "PONG\t<echo_str>\n"
 */
int protocol_create_pong(const char *echo_str, char *out_payload, size_t max_out);

/**
 * @brief Helper to format an ERR payload: "ERR\t<code>\t<message>\n"
 */
int protocol_create_err(int err_code, const char *err_msg, char *out_payload, size_t max_out);

/**
 * @brief Helper to get default error message string from error code.
 */
const char *protocol_err_str(int err_code);

/**
 * @brief Helper to convert string command name to proto_cmd_t enum.
 */
proto_cmd_t protocol_cmd_from_str(const char *cmd_str);

/**
 * @brief Helper to convert proto_cmd_t enum to string command name.
 */
const char *protocol_cmd_to_str(proto_cmd_t cmd);

#ifdef __cplusplus
}
#endif

#endif /* PROTOCOL_H */
