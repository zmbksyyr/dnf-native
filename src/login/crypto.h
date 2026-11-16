#ifndef DNF_CRYPTO_H
#define DNF_CRYPTO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- hashing ---------- */
/* out must be 16 bytes. */
void md5_hash(const uint8_t *data, size_t len, uint8_t out[16]);
/* Produce lowercase 32-char hex string (plus NUL) of MD5(data). */
void md5_hex(const uint8_t *data, size_t len, char out_hex[33]);

/* out must be 20 bytes. */
void sha1_hash(const uint8_t *data, size_t len, uint8_t out[20]);

/* ---------- base64 ---------- */
/* Returns number of bytes written to out (including trailing NUL).
   out must be at least 4*((len+2)/3)+1 bytes. */
size_t base64_encode(const uint8_t *in, size_t len, char *out);

/* Decode base64 in (len bytes, may contain CR/LF/space). Returns number of
   bytes written to out, or (size_t)-1 on invalid input. */
size_t base64_decode(const char *in, size_t len, uint8_t *out);

/* ---------- RSA private key ---------- */
typedef struct {
    int      k;                 /* modulus size in bytes */
    int      limbs;             /* modulus size in 32-bit limbs */
    uint32_t n[80];             /* modulus, little-endian limbs */
    uint32_t d[80];             /* private exponent, little-endian limbs */
    uint32_t n0inv;             /* -n[0]^-1 mod 2^32 (Montgomery) */
    uint32_t r2[80];            /* R^2 mod n (Montgomery), little-endian limbs */
} RsaKey;

/* Parse a PKCS#1 "RSA PRIVATE KEY" PEM (may be a full PEM block with header /
   footer and whitespace). Returns 0 on success, -1 on failure. */
int rsa_key_from_pem(const char *pem, size_t len, RsaKey *key);

/* Validate a PKCS#1 private key with Windows CryptoAPI and export its actual
   modulus/exponent as a SPKI PUBLIC KEY PEM. Returns 0 or -1. */
int rsa_public_key_pem(const char *pem, size_t len, char *out, size_t cap);

/* PKCS#1 v1.5 private-key operation, matching Go's PrivateEncrypt from the
   original program: em = 0x00 01 FF..FF 00 data, m = em^d mod n, output is the
   minimal big-endian byte representation of m (leading zero bytes removed).
   out capacity must be at least key->k bytes. Returns output length, or -1. */
int rsa_private_encrypt(const RsaKey *key, const uint8_t *data, size_t data_len,
                        uint8_t *out);

#ifdef __cplusplus
}
#endif

#endif /* DNF_CRYPTO_H */
