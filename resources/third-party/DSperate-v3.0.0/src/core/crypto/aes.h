#ifndef AES_H
#define AES_H

/* This file is sourced from https://github.com/kokke/tiny-AES-c, with unused code excised.
 * This code is licensed under the Unlicense license, effectively public domain.
 * https://github.com/kokke/tiny-AES-c/blob/f06ac37/unlicense.txt
 */

#include <stdint.h>
#include <stddef.h>

#define AES_BLOCKLEN 16 /* Block length in bytes - AES is 128b block only */
#define AES_KEYLEN 16   /* Key length in bytes */
#define AES_keyExpSize 176

struct AES_ctx
{
  uint8_t RoundKey[AES_keyExpSize];
  uint8_t Iv[AES_BLOCKLEN];
};

#ifdef __cplusplus
extern "C" {
#endif

void AES_init_ctx(struct AES_ctx* ctx, const uint8_t key[AES_KEYLEN]);
void AES_init_ctx_iv(struct AES_ctx* ctx, const uint8_t key[AES_KEYLEN], const uint8_t iv[AES_BLOCKLEN]);
void AES_ctx_set_iv(struct AES_ctx* ctx, const uint8_t iv[AES_BLOCKLEN]);

/* buffer size MUST be mutile of AES_BLOCKLEN;
 * Suggest https://en.wikipedia.org/wiki/Padding_(cryptography)#PKCS7 for padding scheme
 * NOTES: you need to set IV in ctx via AES_init_ctx_iv() or AES_ctx_set_iv()
 * no IV should ever be reused with the same key
 */
void AES_CBC_decrypt_buffer(struct AES_ctx* ctx, uint8_t* buf, size_t length);

/* Same function for encrypting as for decrypting.
 * IV is incremented for every block, and used after encryption as XOR-compliment for output
 * Suggesting https://en.wikipedia.org/wiki/Padding_(cryptography)#PKCS7 for padding scheme
 * NOTES: you need to set IV in ctx with AES_init_ctx_iv() or AES_ctx_set_iv()
 *        no IV should ever be reused with the same key
 */
void AES_CTR_xcrypt_buffer(struct AES_ctx* ctx, uint8_t* buf, size_t length);

/* DSperate: the next CTR keystream block (what AES_CTR_xcrypt_buffer XORs
 * into a 16-byte buffer), with the counter advanced past it. */
void AES_CTR_next_keystream(struct AES_ctx* ctx, uint8_t ks[AES_BLOCKLEN]);

/* One 16-byte block in place (the CCM MAC step). */
void AES_ECB_encrypt(const struct AES_ctx* ctx, uint8_t* buf);

/* DSperate: the cipher uses the CPU's AES instructions when present; a
 * nonzero `soft` pins the portable C instead (tests compare the two). */
void AES_force_software(int soft);

#ifdef __cplusplus
}
#endif

#endif /* AES_H */
