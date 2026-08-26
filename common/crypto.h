#ifndef __CRYPTO_H__
#define __CRYPTO_H__

#ifdef __cplusplus
extern "C" {
#endif

#define CRYPTO_KEY_LEN        32
#define CRYPTO_IV_LEN         12    /* GCM IV (nonce) */
#define CRYPTO_TAG_LEN        16    /* GCM authentication tag */
#define CRYPTO_SALT_LEN       16

/*
 * GCM ciphertext layout:
 *   IV(12 bytes) + ciphertext (same length as plaintext, no padding) + TAG(16 bytes)
 *
 * Total overhead = 12 + 16 = 28 bytes (close to legacy CBC overhead of 16+padding)
 */

#define CRYPTO_GCM_OVERHEAD   (CRYPTO_IV_LEN + CRYPTO_TAG_LEN)

/*
 * Debug / production mode switch
 * Debug (default): use built-in hardcoded key
 * Production: read key from /etc/retail/crypto.key (per-device key)
 */
/* #define CRYPTO_PRODUCTION */

#ifdef CRYPTO_PRODUCTION
#define CRYPTO_KEY_FILE  "/etc/retail/crypto.key"
#else
#define CRYPTO_KEY_FILE  NULL
#endif

int crypto_init(const char *key_file);

int crypto_encrypt(const unsigned char *plaintext, int plain_len,
                   unsigned char **out, int *out_len);

int crypto_decrypt(const unsigned char *ciphertext, int cipher_len,
                   unsigned char **out, int *out_len);

void crypto_cleanup(void);

#ifdef __cplusplus
}
#endif

#endif
