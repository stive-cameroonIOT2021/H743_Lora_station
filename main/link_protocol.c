#include "link_protocol.h"
#include <string.h>

static uint16_t get16(const uint8_t *p) { return (uint16_t)p[0] | (uint16_t)p[1] << 8; }
static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t get64(const uint8_t *p) { return (uint64_t)get32(p) | (uint64_t)get32(p + 4) << 32; }
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) {
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (8 * i));
}
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }
static bool valid_kind(uint8_t kind) { return kind >= LP_TYPE_TELEMETRY && kind <= LP_TYPE_EVENT; }
static bool valid_node(uint8_t node) { return node > 0 && node < 255; }

static void clear_bytes(void *storage, size_t length) {
    volatile uint8_t *bytes = storage;
    while (length--) *bytes++ = 0;
}

int lp_encode(const lp_frame_t *frame, const uint8_t key[LP_KEY_SIZE],
              const uint8_t nonce[LP_NONCE_SIZE], lp_seal_fn seal, void *context, uint8_t *output,
              size_t capacity, size_t *output_length) {
    if (output_length) *output_length = 0;
    if (!frame || !key || !nonce || !seal || !output || !output_length ||
        (frame->payload_len && !frame->payload)) return LP_E_ARGUMENT;
    if (!valid_kind(frame->kind) || !valid_node(frame->src) || !valid_node(frame->dst)) return LP_E_FORMAT;
    const size_t total = LP_MIN_FRAME + frame->payload_len;
    if (frame->payload_len > LP_MAX_PAYLOAD || capacity < total) return LP_E_LENGTH;
    output[0] = 'H'; output[1] = 'L'; output[2] = LP_VERSION;
    output[3] = frame->kind; output[4] = frame->src; output[5] = frame->dst;
    put64(output + 6, frame->session);
    put32(output + 14, frame->sequence);
    put32(output + 18, frame->reply_to);
    put16(output + 22, frame->payload_len);
    memcpy(output + LP_HEADER_SIZE, nonce, LP_NONCE_SIZE);
    uint8_t *ciphertext = output + LP_HEADER_SIZE + LP_NONCE_SIZE;
    if (!seal(key, nonce, output, LP_HEADER_SIZE, frame->payload, frame->payload_len,
              ciphertext, ciphertext + frame->payload_len, context)) {
        clear_bytes(output, total);
        return LP_E_AUTH;
    }
    *output_length = total;
    return LP_OK;
}

int lp_decode(const uint8_t *input, size_t length, const uint8_t key[LP_KEY_SIZE],
              lp_open_fn open, void *context, uint8_t expected_src,
              uint8_t expected_dst, uint8_t *plaintext, size_t plaintext_capacity, lp_frame_t *output) {
    if (output) memset(output, 0, sizeof(*output));
    if (plaintext) clear_bytes(plaintext, plaintext_capacity);
    if (!input || !key || !open || !output || (!plaintext && plaintext_capacity)) return LP_E_ARGUMENT;
    if (length < LP_MIN_FRAME || length > LP_MAX_FRAME) return LP_E_LENGTH;
    if (input[0] != 'H' || input[1] != 'L' || input[2] != LP_VERSION ||
        !valid_kind(input[3]) || !valid_node(input[4]) || !valid_node(input[5]) ||
        (expected_src && input[4] != expected_src) || (expected_dst && input[5] != expected_dst)) return LP_E_FORMAT;
    uint16_t payload_length = get16(input + 22);
    if (payload_length > LP_MAX_PAYLOAD || length != LP_MIN_FRAME + payload_length ||
        payload_length > plaintext_capacity || (payload_length && !plaintext)) return LP_E_LENGTH;
    const uint8_t *ciphertext = input + LP_HEADER_SIZE + LP_NONCE_SIZE;
    if (!open(key, input + LP_HEADER_SIZE, input, LP_HEADER_SIZE, ciphertext, payload_length,
              ciphertext + payload_length, plaintext, context)) {
        if (plaintext) clear_bytes(plaintext, plaintext_capacity);
        return LP_E_AUTH;
    }
    *output = (lp_frame_t){ .kind = input[3], .src = input[4], .dst = input[5],
        .session = get64(input + 6), .sequence = get32(input + 14), .reply_to = get32(input + 18),
        .payload_len = payload_length, .payload = plaintext };
    return LP_OK;
}

void lp_ack_pack(uint8_t output[LP_ACK_SIZE], const lp_ack_t *ack) {
    put64(output, ack->peer_session); put32(output + 8, ack->request_id);
    output[12] = ack->opcode; output[13] = ack->argument; put16(output + 14, ack->ttl_ms);
}

int lp_ack_unpack(const uint8_t *input, size_t length, lp_ack_t *ack) {
    if (!input || !ack) return LP_E_ARGUMENT;
    if (length != LP_ACK_SIZE) return LP_E_LENGTH;
    if (input[12] > LP_OPCODE_DETAIL || (input[12] == LP_OPCODE_DETAIL && input[13] > 3)) return LP_E_FORMAT;
    *ack = (lp_ack_t){ .peer_session = get64(input), .request_id = get32(input + 8),
        .opcode = input[12], .argument = input[13], .ttl_ms = get16(input + 14) };
    return LP_OK;
}
