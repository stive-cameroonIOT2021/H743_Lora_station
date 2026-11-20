#pragma once

#include "link_protocol.h"

/* Call once at the start of app_main, before GPIO/ADC/RF/peripheral setup.
 * Repeated calls never reset the boot prefix or packet counter. The station
 * task is the sole owner of next_nonce after initialization. */
void link_crypto_init(void);
bool link_crypto_nonce_available(void);
bool link_crypto_next_nonce(uint8_t nonce[LP_NONCE_SIZE]);
uint64_t link_crypto_session(void);
uint64_t link_crypto_nonces_used(void);

/* Strict build-key validation; failures clear key without printing its value. */
bool link_crypto_parse_key(const char *hex, uint8_t key[LP_KEY_SIZE]);

bool link_crypto_seal(const uint8_t key[LP_KEY_SIZE], const uint8_t nonce[LP_NONCE_SIZE],
                      const uint8_t *aad, size_t aad_length, const uint8_t *input,
                      size_t length, uint8_t *output, uint8_t tag[LP_TAG_SIZE], void *context);
bool link_crypto_open(const uint8_t key[LP_KEY_SIZE], const uint8_t nonce[LP_NONCE_SIZE],
                      const uint8_t *aad, size_t aad_length, const uint8_t *input,
                      size_t length, const uint8_t tag[LP_TAG_SIZE], uint8_t *output, void *context);
