#include "protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* Command string mapping table */
typedef struct {
    proto_cmd_t cmd;
    const char *str;
} cmd_map_t;

static const cmd_map_t CMD_TABLE[] = {
    { CMD_PING,             "PING" },
    { CMD_PONG,             "PONG" },
    { CMD_ERR,              "ERR" },
    { CMD_REGISTER,         "REGISTER" },
    { CMD_LOGIN,            "LOGIN" },
    { CMD_OK,               "OK" },
    { CMD_GET_ONLINE,       "GET_ONLINE" },
    { CMD_ONLINE_LIST,      "ONLINE_LIST" },
    { CMD_CHALLENGE,        "CHALLENGE" },
    { CMD_CHALLENGE_REQ,    "CHALLENGE_REQ" },
    { CMD_CHALLENGE_RESP,   "CHALLENGE_RESP" },
    { CMD_GAME_START,       "GAME_START" },
    { CMD_MOVE,             "MOVE" },
    { CMD_MOVE_ACK,         "MOVE_ACK" },
    { CMD_MOVE_BROADCAST,   "MOVE_BROADCAST" },
    { CMD_RESIGN,           "RESIGN" },
    { CMD_OFFER_DRAW,       "OFFER_DRAW" },
    { CMD_DRAW_RESP,        "DRAW_RESP" },
    { CMD_GAME_OVER,        "GAME_OVER" },
    { CMD_GET_LEADERBOARD,  "GET_LEADERBOARD" },
    { CMD_GET_GAME_LOG,     "GET_GAME_LOG" },
    { CMD_UNKNOWN,          NULL }
};

proto_cmd_t protocol_cmd_from_str(const char *cmd_str) {
    if (!cmd_str) return CMD_UNKNOWN;
    for (size_t i = 0; CMD_TABLE[i].str != NULL; ++i) {
        if (strcmp(cmd_str, CMD_TABLE[i].str) == 0) {
            return CMD_TABLE[i].cmd;
        }
    }
    return CMD_UNKNOWN;
}

const char *protocol_cmd_to_str(proto_cmd_t cmd) {
    for (size_t i = 0; CMD_TABLE[i].str != NULL; ++i) {
        if (CMD_TABLE[i].cmd == cmd) {
            return CMD_TABLE[i].str;
        }
    }
    return "UNKNOWN";
}

const char *protocol_err_str(int err_code) {
    switch (err_code) {
        case ERR_OK:                  return "OK";
        case ERR_BAD_FRAME:           return "Bad frame format or invalid framing header";
        case ERR_UNKNOWN_CMD:         return "Unknown command";
        case ERR_INVALID_PARAM:       return "Invalid parameter(s)";
        case ERR_PAYLOAD_TOO_LARGE:   return "Payload exceeds maximum allowed size";
        case ERR_UNAUTHORIZED:        return "Unauthorized action; login required";
        case ERR_AUTH_FAILED:         return "Authentication failed; invalid username or password";
        case ERR_USER_EXISTS:         return "Username already exists";
        case ERR_ALREADY_LOGGED_IN:   return "Account is already logged in elsewhere";
        case ERR_USER_NOT_FOUND:      return "Target user not found";
        case ERR_USER_BUSY:           return "Target user is currently busy or in a match";
        case ERR_INVITE_EXPIRED:      return "Challenge invitation expired (30s timeout)";
        case ERR_INVITE_INVALID:      return "Invalid or non-existent challenge invitation";
        case ERR_NOT_IN_GAME:         return "User is not currently in an active game";
        case ERR_NOT_YOUR_TURN:       return "Not your turn to make a move";
        case ERR_INVALID_MOVE:        return "Invalid move: coordinate out of bounds or cell occupied";
        case ERR_SEQ_MISMATCH:        return "Sequence number mismatch; out of sync";
        case ERR_GAME_NOT_FOUND:      return "Game ID not found";
        case ERR_INTERNAL_SERVER:     return "Internal server error";
        case ERR_DATABASE:            return "Database operation failed";
        default:                      return "Unknown error";
    }
}

int protocol_pack_frame(const char *payload, size_t payload_len,
                        uint8_t *out_buf, size_t max_out, size_t *out_len) {
    if (!payload || !out_buf || !out_len) {
        return PROTO_ERR_INVALID_PARAM;
    }

    if (payload_len == 0) {
        payload_len = strlen(payload);
    }

    if (payload_len == 0) {
        return PROTO_ERR_INVALID_PARAM;
    }

    /* Check whether trailing newline '\n' is needed */
    bool needs_newline = (payload[payload_len - 1] != '\n');
    size_t final_payload_len = payload_len + (needs_newline ? 1 : 0);

    if (final_payload_len > PROTO_MAX_PAYLOAD_SIZE) {
        return PROTO_ERR_PAYLOAD_TOO_LARGE;
    }

    size_t total_len = PROTO_HEADER_SIZE + final_payload_len;
    if (max_out < total_len) {
        return PROTO_ERR_BUFFER_OVERFLOW;
    }

    /* Encode 4-byte length in network byte order (big-endian) */
    uint32_t len_be = htonl((uint32_t)final_payload_len);
    memcpy(out_buf, &len_be, PROTO_HEADER_SIZE);

    /* Copy payload body */
    memcpy(out_buf + PROTO_HEADER_SIZE, payload, payload_len);
    if (needs_newline) {
        out_buf[PROTO_HEADER_SIZE + payload_len] = '\n';
    }

    *out_len = total_len;
    return PROTO_OK;
}

int protocol_unpack_frame(const uint8_t *in_buf, size_t in_len,
                          char *out_payload, size_t max_payload,
                          size_t *out_payload_len, size_t *consumed_bytes) {
    if (!in_buf || !out_payload || !out_payload_len || !consumed_bytes) {
        return PROTO_ERR_INVALID_PARAM;
    }

    *consumed_bytes = 0;
    *out_payload_len = 0;

    /* Need at least 4 bytes for header */
    if (in_len < PROTO_HEADER_SIZE) {
        return PROTO_NEED_MORE_DATA;
    }

    /* Decode big-endian length */
    uint32_t len_be;
    memcpy(&len_be, in_buf, PROTO_HEADER_SIZE);
    uint32_t payload_len = ntohl(len_be);

    /* Zero payload length is invalid in Caro protocol */
    if (payload_len == 0) {
        return PROTO_ERR_INVALID_FRAME;
    }

    /* Safety check against gigantic frames / denial of service */
    if (payload_len > PROTO_MAX_FRAME_BUFFER) {
        return PROTO_ERR_PAYLOAD_TOO_LARGE;
    }

    size_t total_frame_len = (size_t)PROTO_HEADER_SIZE + (size_t)payload_len;

    /* Check if complete frame has arrived in input buffer */
    if (in_len < total_frame_len) {
        return PROTO_NEED_MORE_DATA;
    }

    /* Check if caller's output buffer can hold payload + null terminator */
    if (max_payload < (size_t)payload_len + 1) {
        return PROTO_ERR_BUFFER_OVERFLOW;
    }

    /* Extract payload */
    memcpy(out_payload, in_buf + PROTO_HEADER_SIZE, payload_len);
    out_payload[payload_len] = '\0';

    /* Verify payload terminates with newline */
    if (out_payload[payload_len - 1] != '\n') {
        return PROTO_ERR_INVALID_FRAME;
    }

    *out_payload_len = (size_t)payload_len;
    *consumed_bytes = total_frame_len;
    return PROTO_OK;
}

int protocol_tokenize(const char *payload, size_t payload_len,
                      char tokens[PROTO_MAX_TOKENS][PROTO_MAX_TOKEN_LEN],
                      int *token_count) {
    if (!payload || !tokens || !token_count) {
        return PROTO_ERR_INVALID_PARAM;
    }

    *token_count = 0;

    if (payload_len == 0) {
        payload_len = strlen(payload);
    }

    if (payload_len == 0) {
        return PROTO_ERR_INVALID_FRAME;
    }

    /* Strip trailing '\r' and '\n' */
    size_t clean_len = payload_len;
    while (clean_len > 0 && (payload[clean_len - 1] == '\n' || payload[clean_len - 1] == '\r')) {
        clean_len--;
    }

    if (clean_len == 0) {
        return PROTO_ERR_INVALID_FRAME;
    }

    size_t token_idx = 0;
    size_t char_idx = 0;

    for (size_t i = 0; i < clean_len; ++i) {
        char c = payload[i];

        if (c == '\t') {
            /* Terminate current token */
            tokens[token_idx][char_idx] = '\0';
            token_idx++;
            char_idx = 0;

            if (token_idx >= PROTO_MAX_TOKENS) {
                break;
            }
        } else {
            if (char_idx < PROTO_MAX_TOKEN_LEN - 1) {
                tokens[token_idx][char_idx++] = c;
            }
            /* Silently truncate token if exceeding MAX_TOKEN_LEN */
        }
    }

    /* Terminate final token */
    if (token_idx < PROTO_MAX_TOKENS) {
        tokens[token_idx][char_idx] = '\0';
        token_idx++;
    }

    *token_count = (int)token_idx;
    return PROTO_OK;
}

int protocol_parse_msg(const char *payload, size_t payload_len, protocol_msg_t *msg) {
    if (!payload || !msg) {
        return PROTO_ERR_INVALID_PARAM;
    }

    memset(msg, 0, sizeof(*msg));

    if (payload_len == 0) {
        payload_len = strlen(payload);
    }

    /* Copy raw payload (stripping trailing newline) */
    size_t copy_len = payload_len;
    while (copy_len > 0 && (payload[copy_len - 1] == '\n' || payload[copy_len - 1] == '\r')) {
        copy_len--;
    }

    if (copy_len > PROTO_MAX_PAYLOAD_SIZE) {
        copy_len = PROTO_MAX_PAYLOAD_SIZE;
    }
    memcpy(msg->raw_payload, payload, copy_len);
    msg->raw_payload[copy_len] = '\0';
    msg->raw_payload_len = copy_len;

    int tok_res = protocol_tokenize(payload, payload_len, msg->tokens, &msg->token_count);
    if (tok_res != PROTO_OK || msg->token_count < 1) {
        return PROTO_ERR_INVALID_FRAME;
    }

    /* First token is always command name */
    strncpy(msg->cmd_str, msg->tokens[0], sizeof(msg->cmd_str) - 1);
    msg->cmd = protocol_cmd_from_str(msg->cmd_str);

    /* Command-specific parsing of extended fields */
    switch (msg->cmd) {
        case CMD_REGISTER:
        case CMD_LOGIN:
        case CMD_OK:
        case CMD_CHALLENGE:
        case CMD_GET_ONLINE:
        case CMD_ONLINE_LIST:
        case CMD_GET_LEADERBOARD:
        case CMD_GET_GAME_LOG:
            if (msg->token_count >= 2) {
                msg->request_id = (uint32_t)strtoul(msg->tokens[1], NULL, 10);
            }
            break;

        case CMD_CHALLENGE_REQ:
        case CMD_CHALLENGE_RESP:
            if (msg->token_count >= 2) {
                /* token 1 is invite_id */
                msg->request_id = (uint32_t)strtoul(msg->tokens[1], NULL, 10);
            }
            break;

        case CMD_GAME_START:
        case CMD_RESIGN:
        case CMD_OFFER_DRAW:
        case CMD_DRAW_RESP:
        case CMD_GAME_OVER:
            if (msg->token_count >= 2) {
                msg->game_id = (uint32_t)strtoul(msg->tokens[1], NULL, 10);
            }
            break;

        case CMD_MOVE:
        case CMD_MOVE_ACK:
        case CMD_MOVE_BROADCAST:
            if (msg->token_count >= 3) {
                msg->game_id = (uint32_t)strtoul(msg->tokens[1], NULL, 10);
                msg->seq = (uint32_t)strtoul(msg->tokens[2], NULL, 10);
            }
            break;

        case CMD_ERR:
            /* Format: ERR\t<code>\t<message> or ERR\t<ref_id>\t<code>\t<message> */
            if (msg->token_count == 3) {
                /* Code and message */
                msg->request_id = 0;
            } else if (msg->token_count >= 4) {
                msg->request_id = (uint32_t)strtoul(msg->tokens[1], NULL, 10);
            }
            break;

        default:
            break;
    }

    return PROTO_OK;
}

int protocol_create_ping(const char *echo_str, char *out_payload, size_t max_out) {
    if (!out_payload || max_out < 6) {
        return PROTO_ERR_INVALID_PARAM;
    }
    const char *payload = echo_str ? echo_str : "";
    int written = snprintf(out_payload, max_out, "PING\t%s\n", payload);
    if (written < 0 || (size_t)written >= max_out) {
        return PROTO_ERR_BUFFER_OVERFLOW;
    }
    return PROTO_OK;
}

int protocol_create_pong(const char *echo_str, char *out_payload, size_t max_out) {
    if (!out_payload || max_out < 6) {
        return PROTO_ERR_INVALID_PARAM;
    }
    const char *payload = echo_str ? echo_str : "";
    int written = snprintf(out_payload, max_out, "PONG\t%s\n", payload);
    if (written < 0 || (size_t)written >= max_out) {
        return PROTO_ERR_BUFFER_OVERFLOW;
    }
    return PROTO_OK;
}

int protocol_create_err(int err_code, const char *err_msg, char *out_payload, size_t max_out) {
    if (!out_payload || max_out < 8) {
        return PROTO_ERR_INVALID_PARAM;
    }
    const char *msg = err_msg ? err_msg : protocol_err_str(err_code);
    int written = snprintf(out_payload, max_out, "ERR\t%d\t%s\n", err_code, msg);
    if (written < 0 || (size_t)written >= max_out) {
        return PROTO_ERR_BUFFER_OVERFLOW;
    }
    return PROTO_OK;
}
