#include "crypto.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/err.h>

static unsigned char g_key[CRYPTO_KEY_LEN];
static int g_initialized = 0;
static pthread_mutex_t g_crypto_mtx = PTHREAD_MUTEX_INITIALIZER;

static const unsigned char DEFAULT_KEY[CRYPTO_KEY_LEN] = {
    0x7a, 0x3b, 0x8c, 0x1d, 0x4e, 0x9f, 0x2a, 0x6b,
    0x5d, 0x0e, 0x3f, 0x7c, 0x8a, 0x1b, 0x9d, 0x4e,
    0x2f, 0x6c, 0x0a, 0x8b, 0x3d, 0x7e, 0x1f, 0x5c,
    0x9a, 0x4b, 0x8d, 0x0e, 0x2c, 0x6f, 0x1a, 0x3b
};

int crypto_init(const char *key_file)
{
    if (__atomic_load_n(&g_initialized, __ATOMIC_ACQUIRE)) return 0;

#ifdef CRYPTO_PRODUCTION
    if (!key_file) {
        fprintf(stderr, "[CRYPTO] production mode requires a key file path\n");
        return -1;
    }
    FILE *fp = fopen(key_file, "rb");
    if (!fp) {
        fprintf(stderr, "[CRYPTO] cannot open key file: %s\n", key_file);
        return -1;
    }
    size_t n = fread(g_key, 1, CRYPTO_KEY_LEN, fp);
    fclose(fp);
    if (n != CRYPTO_KEY_LEN) {
        fprintf(stderr, "[CRYPTO] key file length mismatch: expected=%d, actual=%zu\n",
                CRYPTO_KEY_LEN, n);
        return -1;
    }
    fprintf(stderr, "[CRYPTO] production mode: key loaded from %s\n", key_file);
#else
    if (key_file) {
        FILE *fp = fopen(key_file, "rb");
        if (fp) {
            size_t n = fread(g_key, 1, CRYPTO_KEY_LEN, fp);
            fclose(fp);
            if (n == CRYPTO_KEY_LEN) {
                __atomic_store_n(&g_initialized, 1, __ATOMIC_RELEASE);
                fprintf(stderr, "[CRYPTO] debug mode: key loaded from %s\n", key_file);
                return 0;
            }
            fprintf(stderr, "[CRYPTO] key file too short, falling back to built-in key\n");
        } else {
            fprintf(stderr, "[CRYPTO] cannot open %s, falling back to built-in key\n", key_file);
        }
    }
    memcpy(g_key, DEFAULT_KEY, CRYPTO_KEY_LEN);
    fprintf(stderr, "[CRYPTO] debug mode: using built-in hardcoded key\n");
#endif
    __atomic_store_n(&g_initialized, 1, __ATOMIC_RELEASE);
    return 0;
}

/*
 * AES-256-GCM encryption
 *
 * Output layout:
 *   IV(12 bytes) | ciphertext (same length as plaintext, GCM has no padding) | TAG(16 bytes)
 *
 * GCM = CTR-mode encryption + GMAC authentication, providing both
 * confidentiality and integrity. On decryption EVP_DecryptFinal verifies
 * the TAG automatically and returns -1 on mismatch.
 */
int crypto_encrypt(const unsigned char *plaintext, int plain_len,
                   unsigned char **out, int *out_len)
{
    if (plain_len < 0) return -1;
    if (!__atomic_load_n(&g_initialized, __ATOMIC_ACQUIRE) || !plaintext || !out || !out_len) return -1;

    pthread_mutex_lock(&g_crypto_mtx);

    unsigned char iv[CRYPTO_IV_LEN];
    if (RAND_bytes(iv, CRYPTO_IV_LEN) != 1) {
        pthread_mutex_unlock(&g_crypto_mtx);
        return -1;
    }

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        pthread_mutex_unlock(&g_crypto_mtx);
        return -1;
    }

    int alloc_len = CRYPTO_IV_LEN + plain_len + CRYPTO_TAG_LEN;
    *out = malloc(alloc_len);
    if (!*out) {
        EVP_CIPHER_CTX_free(ctx);
        pthread_mutex_unlock(&g_crypto_mtx);
        return -1;
    }

    memcpy(*out, iv, CRYPTO_IV_LEN);

    int len = 0, total = 0;

    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1) goto err;
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, CRYPTO_IV_LEN, NULL) != 1) goto err;
    if (EVP_EncryptInit_ex(ctx, NULL, NULL, g_key, iv) != 1) goto err;

    if (EVP_EncryptUpdate(ctx, *out + CRYPTO_IV_LEN, &len, plaintext, plain_len) != 1) goto err;
    total = len;

    if (EVP_EncryptFinal_ex(ctx, *out + CRYPTO_IV_LEN + total, &len) != 1) goto err;
    total += len;

    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, CRYPTO_TAG_LEN,
                            *out + CRYPTO_IV_LEN + total) != 1) goto err;
    total += CRYPTO_TAG_LEN;

    *out_len = CRYPTO_IV_LEN + plain_len + CRYPTO_TAG_LEN;

    EVP_CIPHER_CTX_free(ctx);
    pthread_mutex_unlock(&g_crypto_mtx);
    return 0;

err:
    free(*out);
    *out = NULL;
    EVP_CIPHER_CTX_free(ctx);
    pthread_mutex_unlock(&g_crypto_mtx);
    return -1;
}

/*
 * AES-256-GCM decryption
 *
 * Input layout:
 *   IV(12 bytes) | ciphertext | TAG(16 bytes)
 *
 * OpenSSL requires the expected TAG to be set before EVP_DecryptFinal,
 * which then performs a constant-time comparison. Returns -1 on mismatch.
 */
int crypto_decrypt(const unsigned char *ciphertext, int cipher_len,
                   unsigned char **out, int *out_len)
{
    if (!__atomic_load_n(&g_initialized, __ATOMIC_ACQUIRE) || !ciphertext || !out || !out_len) return -1;
    if (cipher_len < CRYPTO_GCM_OVERHEAD) return -1;

    pthread_mutex_lock(&g_crypto_mtx);

    const unsigned char *iv  = ciphertext;
    const unsigned char *tag = ciphertext + cipher_len - CRYPTO_TAG_LEN;
    const unsigned char *enc = ciphertext + CRYPTO_IV_LEN;
    int enc_len = cipher_len - CRYPTO_IV_LEN - CRYPTO_TAG_LEN;

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        pthread_mutex_unlock(&g_crypto_mtx);
        return -1;
    }

    *out = malloc(enc_len > 0 ? enc_len : 1);
    if (!*out) {
        EVP_CIPHER_CTX_free(ctx);
        pthread_mutex_unlock(&g_crypto_mtx);
        return -1;
    }

    int len = 0, total = 0;

    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1) goto err;
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, CRYPTO_IV_LEN, NULL) != 1) goto err;
    if (EVP_DecryptInit_ex(ctx, NULL, NULL, g_key, iv) != 1) goto err;

    if (enc_len > 0) {
        if (EVP_DecryptUpdate(ctx, *out, &len, enc, enc_len) != 1) goto err;
        total = len;
    }

    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, CRYPTO_TAG_LEN,
                            (void *)tag) != 1) goto err;

    if (EVP_DecryptFinal_ex(ctx, *out + total, &len) != 1) {
        /* TAG verification failed: ciphertext tampered or wrong key */
        fprintf(stderr, "[CRYPTO] GCM TAG verification FAILED — data tampered or wrong key\n");
        goto err;
    }
    total += len;

    *out_len = total;
    EVP_CIPHER_CTX_free(ctx);
    pthread_mutex_unlock(&g_crypto_mtx);
    return 0;

err:
    free(*out);
    *out = NULL;
    EVP_CIPHER_CTX_free(ctx);
    pthread_mutex_unlock(&g_crypto_mtx);
    return -1;
}

void crypto_cleanup(void)
{
    pthread_mutex_lock(&g_crypto_mtx);
    memset(g_key, 0, CRYPTO_KEY_LEN);
    __atomic_store_n(&g_initialized, 0, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&g_crypto_mtx);
}
