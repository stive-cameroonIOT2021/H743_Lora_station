/* Include the production source to let this test reach the final nonce counter
 * without generating 2^32 packets. Crypto functions are never substituted. */
#include "../main/link_crypto.c"

static unsigned entropy_phase, entropy_reads;
static bool entropy_order_ok = true;

void bootloader_random_enable(void)
{
    if (entropy_phase != 0) entropy_order_ok = false;
    entropy_phase = 1;
}

void esp_fill_random(void *buffer, size_t length)
{
    if (entropy_phase != 1 || length != 12) entropy_order_ok = false;
    for (size_t i = 0; i < length; ++i) ((uint8_t *)buffer)[i] = (uint8_t)(0x10 + i);
    ++entropy_reads;
    entropy_phase = 2;
}

void bootloader_random_disable(void)
{
    if (entropy_phase != 2) entropy_order_ok = false;
    entropy_phase = 3;
}

bool host_entropy_order_ok(void) { return entropy_order_ok && entropy_phase == 3 && entropy_reads == 1; }
void host_last_nonce(void) { nonce_counter = UINT32_MAX; nonce_exhausted = false; }

int host_encode(uint8_t kind, uint8_t src, uint8_t dst, uint64_t session,
                uint32_t sequence, uint32_t reply_to, const uint8_t *payload,
                uint16_t payload_length, const uint8_t *key, const uint8_t *nonce,
                uint8_t *output, size_t capacity, size_t *length)
{
    lp_frame_t frame = {.kind = kind, .src = src, .dst = dst, .session = session,
        .sequence = sequence, .reply_to = reply_to, .payload_len = payload_length, .payload = payload};
    return lp_encode(&frame, key, nonce, link_crypto_seal, NULL, output, capacity, length);
}

int host_decode(const uint8_t *input, size_t length, const uint8_t *key,
                uint8_t src, uint8_t dst, uint8_t *plaintext, size_t capacity, lp_frame_t *frame)
{
    return lp_decode(input, length, key, link_crypto_open, NULL, src, dst, plaintext, capacity, frame);
}
