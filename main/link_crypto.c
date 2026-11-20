#include "link_crypto.h"

#include <string.h>
#include "bootloader_random.h"
#include "esp_random.h"
#include "mbedtls/gcm.h"
#include "mbedtls/platform_util.h"

static uint8_t boot_prefix[12];
static uint32_t nonce_counter;
static bool nonce_initialized, nonce_exhausted;

void link_crypto_init(void)
{
    if (nonce_initialized) return;
    /* RF is still off. Mix the bootloader's real entropy source into HWRNG
     * before sampling, and release ADC/RF resources before any peripherals. */
    bootloader_random_enable();
    esp_fill_random(boot_prefix, sizeof(boot_prefix));
    bootloader_random_disable();
    nonce_initialized = true;
}

bool link_crypto_nonce_available(void) { return nonce_initialized && !nonce_exhausted; }

bool link_crypto_next_nonce(uint8_t nonce[LP_NONCE_SIZE])
{
    if (!nonce) return false;
    if (!link_crypto_nonce_available()) {
        mbedtls_platform_zeroize(nonce, LP_NONCE_SIZE);
        return false;
    }
    memcpy(nonce, boot_prefix, sizeof(boot_prefix));
    for (unsigned i = 0; i < 4; ++i) nonce[12 + i] = (uint8_t)(nonce_counter >> (8 * i));
    if (nonce_counter == UINT32_MAX) nonce_exhausted = true;
    else ++nonce_counter;
    return true;
}

uint64_t link_crypto_nonces_used(void)
{
    return nonce_exhausted ? (UINT64_C(1) << 32) : nonce_counter;
}

uint64_t link_crypto_session(void)
{
    uint64_t session = 0;
    if (!nonce_initialized) return 0;
    for (unsigned i = 0; i < 8; ++i) session |= (uint64_t)boot_prefix[i] << (8 * i);
    return session ? session : 1;
}

static int hex_digit(char value)
{
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

bool link_crypto_parse_key(const char *hex, uint8_t key[LP_KEY_SIZE])
{
    if (!key) return false;
    mbedtls_platform_zeroize(key, LP_KEY_SIZE);
    if (!hex || strlen(hex) != LP_KEY_SIZE * 2) return false;
    uint8_t combined = 0;
    for (size_t i = 0; i < LP_KEY_SIZE; ++i) {
        int high = hex_digit(hex[i * 2]), low = hex_digit(hex[i * 2 + 1]);
        if (high < 0 || low < 0) {
            mbedtls_platform_zeroize(key, LP_KEY_SIZE);
            return false;
        }
        key[i] = (uint8_t)((high << 4) | low);
        combined |= key[i];
    }
    return combined != 0;
}

bool link_crypto_seal(const uint8_t key[LP_KEY_SIZE], const uint8_t nonce[LP_NONCE_SIZE],
                      const uint8_t *aad, size_t aad_length, const uint8_t *input,
                      size_t length, uint8_t *output, uint8_t tag[LP_TAG_SIZE], void *context)
{
    (void)context;
    if (!key || !nonce || (!aad && aad_length) || (!input && length) ||
        (!output && length) || !tag) return false;
    uint8_t empty = 0;
    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    int error = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256);
    if (!error) error = mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, length,
        nonce, LP_NONCE_SIZE, aad, aad_length, input ? input : &empty,
        output ? output : &empty, LP_TAG_SIZE, tag);
    mbedtls_gcm_free(&gcm);
    if (error) {
        if (output) mbedtls_platform_zeroize(output, length);
        mbedtls_platform_zeroize(tag, LP_TAG_SIZE);
    }
    return error == 0;
}

bool link_crypto_open(const uint8_t key[LP_KEY_SIZE], const uint8_t nonce[LP_NONCE_SIZE],
                      const uint8_t *aad, size_t aad_length, const uint8_t *input,
                      size_t length, const uint8_t tag[LP_TAG_SIZE], uint8_t *output, void *context)
{
    (void)context;
    if (!key || !nonce || (!aad && aad_length) || (!input && length) ||
        (!output && length) || !tag) return false;
    uint8_t empty = 0;
    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    int error = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256);
    /* auth_decrypt verifies the GCM tag and clears output on authentication
     * failure. No unauthenticated decrypt API is used by the receive path. */
    if (!error) error = mbedtls_gcm_auth_decrypt(&gcm, length, nonce, LP_NONCE_SIZE,
        aad, aad_length, tag, LP_TAG_SIZE, input ? input : &empty, output ? output : &empty);
    mbedtls_gcm_free(&gcm);
    if (error && output) mbedtls_platform_zeroize(output, length);
    return error == 0;
}
