// PIN-based key encryption. See keywrap.h.
#include "keywrap.h"

#include <string.h>
#include "esp_random.h"
#include "mbedtls/sha256.h"
#include "mbedtls/md.h"
#include "mbedtls/pkcs5.h"
#include "mbedtls/gcm.h"
#include "mbedtls/platform_util.h"

namespace keywrap {

void deriveKey(const String& pin, const uint8_t salt[16], uint8_t out[32]) {
  mbedtls_pkcs5_pbkdf2_hmac_ext(
      MBEDTLS_MD_SHA256,
      (const unsigned char*)pin.c_str(), pin.length(),
      salt, 16,
      20000, 32, out);
}

void wrap(const uint8_t key[32], const uint8_t plain[32], uint8_t out[60]) {
  uint8_t* iv  = out;        // 12 bytes
  uint8_t* ct  = out + 12;   // 32 bytes
  uint8_t* tag = out + 44;   // 16 bytes
  esp_fill_random(iv, 12);

  mbedtls_gcm_context gcm;
  mbedtls_gcm_init(&gcm);
  mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256);
  mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, 32,
                            iv, 12, nullptr, 0,
                            plain, ct, 16, tag);
  mbedtls_gcm_free(&gcm);
}

bool unwrap(const uint8_t key[32], const uint8_t blob[60], uint8_t out[32]) {
  const uint8_t* iv  = blob;
  const uint8_t* ct  = blob + 12;
  const uint8_t* tag = blob + 44;

  mbedtls_gcm_context gcm;
  mbedtls_gcm_init(&gcm);
  mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256);
  int rc = mbedtls_gcm_auth_decrypt(&gcm, 32,
                                    iv, 12, nullptr, 0,
                                    tag, 16, ct, out);
  mbedtls_gcm_free(&gcm);
  if (rc != 0) {
    mbedtls_platform_zeroize(out, 32);
    return false;
  }
  return true;
}

void verifier(const uint8_t key[32], const uint8_t salt[16], uint8_t out[32]) {
  uint8_t buf[48];
  memcpy(buf, key, 32);
  memcpy(buf + 32, salt, 16);
  mbedtls_sha256(buf, sizeof(buf), out, 0);
  mbedtls_platform_zeroize(buf, sizeof(buf));
}

} // namespace keywrap
