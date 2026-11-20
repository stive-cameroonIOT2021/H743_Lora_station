#ifndef H743_LINK_PROTOCOL_H
#define H743_LINK_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LP_VERSION 2u
#define LP_HEADER_SIZE 24u
#define LP_MAX_PAYLOAD 192u
#define LP_TAG_SIZE 16u
#define LP_KEY_SIZE 32u
#define LP_NONCE_SIZE 16u
#define LP_MIN_FRAME (LP_HEADER_SIZE + LP_NONCE_SIZE + LP_TAG_SIZE)
#define LP_MAX_FRAME (LP_MIN_FRAME + LP_MAX_PAYLOAD)
#define LP_ACK_SIZE 16u
#define LP_NO_REPLY UINT32_MAX

enum { LP_TYPE_TELEMETRY = 1, LP_TYPE_ACK = 2, LP_TYPE_RESULT = 3, LP_TYPE_EVENT = 4 };
enum { LP_OPCODE_NONE = 0, LP_OPCODE_PING = 1, LP_OPCODE_DETAIL = 2 };
enum { LP_RESULT_OK = 0, LP_RESULT_UNSUPPORTED = 1, LP_RESULT_EXPIRED = 2 };
enum { LP_OK = 0, LP_E_LENGTH = -1, LP_E_FORMAT = -2,
       LP_E_AUTH = -4, LP_E_ARGUMENT = -5 };

typedef struct {
    uint8_t kind, src, dst;
    uint64_t session;
    uint32_t sequence, reply_to;
    uint16_t payload_len;
    const uint8_t *payload; /* decode borrows the caller's plaintext storage */
} lp_frame_t;

typedef struct {
    uint64_t peer_session;
    uint32_t request_id;
    uint8_t opcode, argument;
    uint16_t ttl_ms;
} lp_ack_t;

typedef bool (*lp_seal_fn)(const uint8_t key[LP_KEY_SIZE], const uint8_t nonce[LP_NONCE_SIZE],
                          const uint8_t *aad, size_t aad_length, const uint8_t *input,
                          size_t length, uint8_t *output, uint8_t tag[LP_TAG_SIZE], void *context);
typedef bool (*lp_open_fn)(const uint8_t key[LP_KEY_SIZE], const uint8_t nonce[LP_NONCE_SIZE],
                          const uint8_t *aad, size_t aad_length, const uint8_t *input,
                          size_t length, const uint8_t tag[LP_TAG_SIZE], uint8_t *output, void *context);

int lp_encode(const lp_frame_t *frame, const uint8_t key[LP_KEY_SIZE],
              const uint8_t nonce[LP_NONCE_SIZE], lp_seal_fn seal, void *context, uint8_t *output,
              size_t capacity, size_t *output_length);
int lp_decode(const uint8_t *input, size_t length, const uint8_t key[LP_KEY_SIZE],
              lp_open_fn open, void *context, uint8_t expected_src,
              uint8_t expected_dst, uint8_t *plaintext, size_t plaintext_capacity, lp_frame_t *output);
void lp_ack_pack(uint8_t output[LP_ACK_SIZE], const lp_ack_t *ack);
int lp_ack_unpack(const uint8_t *input, size_t length, lp_ack_t *ack);

#endif
