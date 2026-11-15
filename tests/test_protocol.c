#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "protocol.h"

static int g_tests_run = 0;
static int g_tests_passed = 0;

#define TEST_ASSERT(cond, msg) do { \
    g_tests_run++; \
    if (!(cond)) { \
        printf("[-] FAIL: %s (line %d): %s\n", __func__, __LINE__, msg); \
    } else { \
        g_tests_passed++; \
        printf("[+] PASS: %s: %s\n", __func__, msg); \
    } \
} while(0)

/* Test 1: Standard Packing */
static void test_pack_frame_basic(void) {
    uint8_t out_buf[128];
    size_t out_len = 0;
    const char *payload = "PING\t12345\n";
    size_t payload_len = strlen(payload);

    int res = protocol_pack_frame(payload, payload_len, out_buf, sizeof(out_buf), &out_len);
    TEST_ASSERT(res == PROTO_OK, "Packing valid payload succeeds");
    TEST_ASSERT(out_len == PROTO_HEADER_SIZE + payload_len, "Output length matches header + payload");

    /* Verify big-endian length header */
    uint32_t len_be;
    memcpy(&len_be, out_buf, 4);
    uint32_t decoded_len = ntohl(len_be);
    TEST_ASSERT(decoded_len == (uint32_t)payload_len, "Decoded big-endian header matches payload length");

    /* Verify payload bytes */
    TEST_ASSERT(memcmp(out_buf + 4, payload, payload_len) == 0, "Payload bytes match verbatim");
}

/* Test 2: Packing adds missing newline automatically */
static void test_pack_frame_auto_newline(void) {
    uint8_t out_buf[128];
    size_t out_len = 0;
    const char *payload_no_nl = "PING\ttest";
    size_t raw_len = strlen(payload_no_nl);

    int res = protocol_pack_frame(payload_no_nl, raw_len, out_buf, sizeof(out_buf), &out_len);
    TEST_ASSERT(res == PROTO_OK, "Packing payload without newline succeeds");
    TEST_ASSERT(out_len == PROTO_HEADER_SIZE + raw_len + 1, "Output length accounts for added newline");

    uint32_t len_be;
    memcpy(&len_be, out_buf, 4);
    uint32_t decoded_len = ntohl(len_be);
    TEST_ASSERT(decoded_len == (uint32_t)(raw_len + 1), "Header includes added newline in length");
    TEST_ASSERT(out_buf[out_len - 1] == '\n', "Last byte is newline");
}

/* Test 3: Unpacking complete single frame */
static void test_unpack_frame_complete(void) {
    uint8_t wire[64];
    size_t wire_len = 0;
    protocol_pack_frame("PONG\t12345\n", 0, wire, sizeof(wire), &wire_len);

    char out_payload[64];
    size_t out_payload_len = 0;
    size_t consumed = 0;

    int res = protocol_unpack_frame(wire, wire_len, out_payload, sizeof(out_payload), &out_payload_len, &consumed);
    TEST_ASSERT(res == PROTO_OK, "Unpacking complete frame succeeds");
    TEST_ASSERT(consumed == wire_len, "Consumed bytes equals total wire length");
    TEST_ASSERT(strcmp(out_payload, "PONG\t12345\n") == 0, "Extracted payload content matches");
    TEST_ASSERT(out_payload_len == strlen("PONG\t12345\n"), "Extracted length matches");
}

/* Test 4: Partial frame handling (< 4 bytes header) */
static void test_unpack_partial_header(void) {
    uint8_t wire[64];
    size_t wire_len = 0;
    protocol_pack_frame("PING\tpartial\n", 0, wire, sizeof(wire), &wire_len);

    char out_payload[64];
    size_t out_payload_len = 0;
    size_t consumed = 0;

    /* Feed only 1, 2, 3 bytes */
    for (size_t partial_len = 1; partial_len < PROTO_HEADER_SIZE; ++partial_len) {
        int res = protocol_unpack_frame(wire, partial_len, out_payload, sizeof(out_payload), &out_payload_len, &consumed);
        TEST_ASSERT(res == PROTO_NEED_MORE_DATA, "Header < 4 bytes returns PROTO_NEED_MORE_DATA");
        TEST_ASSERT(consumed == 0, "No bytes consumed when header incomplete");
    }
}

/* Test 5: Partial frame handling (Header complete, payload incomplete) */
static void test_unpack_partial_payload(void) {
    uint8_t wire[64];
    size_t wire_len = 0;
    protocol_pack_frame("PING\tpartial_body\n", 0, wire, sizeof(wire), &wire_len);

    char out_payload[64];
    size_t out_payload_len = 0;
    size_t consumed = 0;

    /* Feed header + half of payload */
    size_t partial_len = PROTO_HEADER_SIZE + 4;
    int res = protocol_unpack_frame(wire, partial_len, out_payload, sizeof(out_payload), &out_payload_len, &consumed);
    TEST_ASSERT(res == PROTO_NEED_MORE_DATA, "Incomplete payload returns PROTO_NEED_MORE_DATA");
    TEST_ASSERT(consumed == 0, "No bytes consumed when payload incomplete");
}

/* Test 6: Coalesced frames (multiple frames back-to-back in input buffer) */
static void test_unpack_coalesced_frames(void) {
    uint8_t wire[256];
    size_t len1 = 0, len2 = 0;

    protocol_pack_frame("PING\tframe1\n", 0, wire, sizeof(wire), &len1);
    protocol_pack_frame("PONG\tframe2\n", 0, wire + len1, sizeof(wire) - len1, &len2);
    size_t total_wire = len1 + len2;

    char out_payload[128];
    size_t out_payload_len = 0;
    size_t consumed = 0;

    /* Unpack Frame 1 */
    int res1 = protocol_unpack_frame(wire, total_wire, out_payload, sizeof(out_payload), &out_payload_len, &consumed);
    TEST_ASSERT(res1 == PROTO_OK, "First coalesced frame unpacked successfully");
    TEST_ASSERT(consumed == len1, "First frame consumed exact length of frame 1");
    TEST_ASSERT(strcmp(out_payload, "PING\tframe1\n") == 0, "First frame payload matches");

    /* Advance buffer by consumed bytes */
    const uint8_t *next_ptr = wire + consumed;
    size_t remaining_bytes = total_wire - consumed;

    /* Unpack Frame 2 */
    int res2 = protocol_unpack_frame(next_ptr, remaining_bytes, out_payload, sizeof(out_payload), &out_payload_len, &consumed);
    TEST_ASSERT(res2 == PROTO_OK, "Second coalesced frame unpacked successfully");
    TEST_ASSERT(consumed == len2, "Second frame consumed exact length of frame 2");
    TEST_ASSERT(strcmp(out_payload, "PONG\tframe2\n") == 0, "Second frame payload matches");
    TEST_ASSERT(remaining_bytes - consumed == 0, "All bytes in coalesced stream processed");
}

/* Test 7: Malformed frame without trailing newline rejected */
static void test_unpack_missing_newline_rejected(void) {
    uint8_t wire[32];
    uint32_t len = 5;
    uint32_t len_be = htonl(len);
    memcpy(wire, &len_be, 4);
    memcpy(wire + 4, "HELLO", 5); /* No '\n' */

    char out_payload[32];
    size_t out_payload_len = 0;
    size_t consumed = 0;

    int res = protocol_unpack_frame(wire, 4 + len, out_payload, sizeof(out_payload), &out_payload_len, &consumed);
    TEST_ASSERT(res == PROTO_ERR_INVALID_FRAME, "Frame without trailing newline returns PROTO_ERR_INVALID_FRAME");
}

/* Test 8: Gigantic frame length rejected */
static void test_unpack_gigantic_frame_rejected(void) {
    uint8_t wire[8];
    uint32_t bad_len = 1000000; /* Exceeds 65536 */
    uint32_t len_be = htonl(bad_len);
    memcpy(wire, &len_be, 4);

    char out_payload[32];
    size_t out_payload_len = 0;
    size_t consumed = 0;

    int res = protocol_unpack_frame(wire, 8, out_payload, sizeof(out_payload), &out_payload_len, &consumed);
    TEST_ASSERT(res == PROTO_ERR_PAYLOAD_TOO_LARGE, "Oversized frame length returns PROTO_ERR_PAYLOAD_TOO_LARGE");
}

/* Test 9: Tokenization and message parsing */
static void test_parse_msg_various(void) {
    protocol_msg_t msg;

    /* Test PING */
    int res = protocol_parse_msg("PING\t1731770000\n", 0, &msg);
    TEST_ASSERT(res == PROTO_OK, "Parse PING succeeds");
    TEST_ASSERT(msg.cmd == CMD_PING, "Identified CMD_PING");
    TEST_ASSERT(msg.token_count == 2, "PING has 2 tokens");
    TEST_ASSERT(strcmp(msg.tokens[1], "1731770000") == 0, "PING token 1 matches");

    /* Test ERR */
    res = protocol_parse_msg("ERR\t101\tUnknown command\n", 0, &msg);
    TEST_ASSERT(res == PROTO_OK, "Parse ERR succeeds");
    TEST_ASSERT(msg.cmd == CMD_ERR, "Identified CMD_ERR");
    TEST_ASSERT(msg.token_count == 3, "ERR has 3 tokens");
    TEST_ASSERT(strcmp(msg.tokens[1], "101") == 0, "ERR code matches 101");
    TEST_ASSERT(strcmp(msg.tokens[2], "Unknown command") == 0, "ERR msg matches");

    /* Test MOVE with game_id and seq */
    res = protocol_parse_msg("MOVE\t42\t7\t8\t9\n", 0, &msg);
    TEST_ASSERT(res == PROTO_OK, "Parse MOVE succeeds");
    TEST_ASSERT(msg.cmd == CMD_MOVE, "Identified CMD_MOVE");
    TEST_ASSERT(msg.game_id == 42, "Parsed game_id == 42");
    TEST_ASSERT(msg.seq == 7, "Parsed seq == 7");
    TEST_ASSERT(strcmp(msg.tokens[3], "8") == 0, "Parsed x == 8");
    TEST_ASSERT(strcmp(msg.tokens[4], "9") == 0, "Parsed y == 9");

    /* Test LOGIN with request_id */
    res = protocol_parse_msg("LOGIN\t1001\talice\tsecret\n", 0, &msg);
    TEST_ASSERT(res == PROTO_OK, "Parse LOGIN succeeds");
    TEST_ASSERT(msg.cmd == CMD_LOGIN, "Identified CMD_LOGIN");
    TEST_ASSERT(msg.request_id == 1001, "Parsed request_id == 1001");
    TEST_ASSERT(strcmp(msg.tokens[2], "alice") == 0, "Parsed username == alice");
}

/* Test 10: Helper functions */
static void test_helpers(void) {
    char buf[128];

    protocol_create_ping("hello", buf, sizeof(buf));
    TEST_ASSERT(strcmp(buf, "PING\thello\n") == 0, "create_ping produces correct string");

    protocol_create_pong("hello", buf, sizeof(buf));
    TEST_ASSERT(strcmp(buf, "PONG\thello\n") == 0, "create_pong produces correct string");

    protocol_create_err(ERR_NOT_YOUR_TURN, NULL, buf, sizeof(buf));
    TEST_ASSERT(strstr(buf, "ERR\t401\t") != NULL, "create_err uses default message for 401");

    const char *err_str = protocol_err_str(ERR_BAD_FRAME);
    TEST_ASSERT(err_str != NULL && strlen(err_str) > 0, "protocol_err_str returns valid string");
}

int main(void) {
    printf("=========================================\n");
    printf("   RUNNING PROTOCOL UNIT TESTS (T1H2)   \n");
    printf("=========================================\n");

    test_pack_frame_basic();
    test_pack_frame_auto_newline();
    test_unpack_frame_complete();
    test_unpack_partial_header();
    test_unpack_partial_payload();
    test_unpack_coalesced_frames();
    test_unpack_missing_newline_rejected();
    test_unpack_gigantic_frame_rejected();
    test_parse_msg_various();
    test_helpers();

    printf("=========================================\n");
    printf("   RESULTS: %d / %d tests passed\n", g_tests_passed, g_tests_run);
    printf("=========================================\n");

    return (g_tests_passed == g_tests_run) ? 0 : 1;
}
