#include "bip340.h"

#include <esp_random.h>
#include "mbedtls/ecp.h"
#include "mbedtls/bignum.h"
#include "mbedtls/sha256.h"

namespace bip340 {

// ---------------------------------------------------------------------------
// Hashing
// ---------------------------------------------------------------------------
void sha256(const uint8_t* data, size_t len, uint8_t out[32]) {
  mbedtls_sha256(data, len, out, /*is224=*/0);
}

void tagged_hash(const char* tag, const uint8_t* data, size_t len, uint8_t out[32]) {
  uint8_t tag_hash[32];
  sha256((const uint8_t*)tag, strlen(tag), tag_hash);

  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  mbedtls_sha256_update(&ctx, tag_hash, 32);
  mbedtls_sha256_update(&ctx, tag_hash, 32);
  mbedtls_sha256_update(&ctx, data, len);
  mbedtls_sha256_finish(&ctx, out);
  mbedtls_sha256_free(&ctx);
}

// mbedTLS wants an RNG callback for scalar-mul blinding.
static int hw_rng(void*, unsigned char* out, size_t n) {
  esp_fill_random(out, n);
  return 0;
}

// ---------------------------------------------------------------------------
// lift_x: recover the even-Y point for an x-only public key.
// secp256k1: p % 4 == 3, so sqrt(a) = a^((p+1)/4) mod p.
// ---------------------------------------------------------------------------
static bool lift_x(mbedtls_ecp_group* grp, const uint8_t x_bytes[32],
                   mbedtls_ecp_point* P) {
  mbedtls_mpi x, y2, y, e, seven;
  mbedtls_mpi_init(&x); mbedtls_mpi_init(&y2); mbedtls_mpi_init(&y);
  mbedtls_mpi_init(&e); mbedtls_mpi_init(&seven);

  bool ok = false;
  if (mbedtls_mpi_read_binary(&x, x_bytes, 32) != 0) goto done;
  if (mbedtls_mpi_cmp_mpi(&x, &grp->P) >= 0) goto done;

  // y^2 = x^3 + 7 mod p
  if (mbedtls_mpi_lset(&seven, 7) != 0) goto done;
  if (mbedtls_mpi_mul_mpi(&y2, &x, &x) != 0) goto done;
  if (mbedtls_mpi_mod_mpi(&y2, &y2, &grp->P) != 0) goto done;
  if (mbedtls_mpi_mul_mpi(&y2, &y2, &x) != 0) goto done;
  if (mbedtls_mpi_add_mpi(&y2, &y2, &seven) != 0) goto done;
  if (mbedtls_mpi_mod_mpi(&y2, &y2, &grp->P) != 0) goto done;

  // e = (p+1)/4 ; y = y2^e mod p
  if (mbedtls_mpi_add_int(&e, &grp->P, 1) != 0) goto done;
  if (mbedtls_mpi_shift_r(&e, 2) != 0) goto done;
  if (mbedtls_mpi_exp_mod(&y, &y2, &e, &grp->P, nullptr) != 0) goto done;

  // check y^2 == y2 (x must actually be on the curve)
  {
    mbedtls_mpi chk;
    mbedtls_mpi_init(&chk);
    int r = mbedtls_mpi_mul_mpi(&chk, &y, &y);
    if (r == 0) r = mbedtls_mpi_mod_mpi(&chk, &chk, &grp->P);
    bool on_curve = (r == 0) && (mbedtls_mpi_cmp_mpi(&chk, &y2) == 0);
    mbedtls_mpi_free(&chk);
    if (!on_curve) goto done;
  }

  // choose even Y
  if (mbedtls_mpi_get_bit(&y, 0) == 1) {
    if (mbedtls_mpi_sub_mpi(&y, &grp->P, &y) != 0) goto done;
  }

  if (mbedtls_mpi_copy(&P->MBEDTLS_PRIVATE(X), &x) != 0) goto done;
  if (mbedtls_mpi_copy(&P->MBEDTLS_PRIVATE(Y), &y) != 0) goto done;
  if (mbedtls_mpi_lset(&P->MBEDTLS_PRIVATE(Z), 1) != 0) goto done;
  ok = true;

done:
  mbedtls_mpi_free(&x); mbedtls_mpi_free(&y2); mbedtls_mpi_free(&y);
  mbedtls_mpi_free(&e); mbedtls_mpi_free(&seven);
  return ok;
}

// ---------------------------------------------------------------------------
// Sign (BIP-340 reference algorithm, aux randomness from the hardware TRNG)
// ---------------------------------------------------------------------------
bool sign(const uint8_t priv[32], const uint8_t msg[32], uint8_t sig[64]) {
  mbedtls_ecp_group grp;
  mbedtls_ecp_point R, P;
  mbedtls_mpi d, k, e, s, tmp;
  mbedtls_ecp_group_init(&grp);
  mbedtls_ecp_point_init(&R);
  mbedtls_ecp_point_init(&P);
  mbedtls_mpi_init(&d); mbedtls_mpi_init(&k); mbedtls_mpi_init(&e);
  mbedtls_mpi_init(&s); mbedtls_mpi_init(&tmp);

  bool ok = false;
  uint8_t pub_x[32], aux[32], t[32], nonce_input[96], rand32[32];
  uint8_t challenge_input[96];

  if (mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256K1) != 0) goto done;

  // d, and P = d*G (caller guarantees canonical even-Y d, but recompute
  // pub_x from scratch so the challenge always matches the real key).
  if (mbedtls_mpi_read_binary(&d, priv, 32) != 0) goto done;
  if (mbedtls_mpi_cmp_int(&d, 0) == 0 || mbedtls_mpi_cmp_mpi(&d, &grp.N) >= 0) goto done;
  if (mbedtls_ecp_mul(&grp, &P, &d, &grp.G, hw_rng, nullptr) != 0) goto done;
  if (mbedtls_mpi_get_bit(&P.MBEDTLS_PRIVATE(Y), 0) == 1) goto done;  // not canonical
  if (mbedtls_mpi_write_binary(&P.MBEDTLS_PRIVATE(X), pub_x, 32) != 0) goto done;

  // t = d XOR tagged_hash("BIP0340/aux", a)
  esp_fill_random(aux, 32);
  tagged_hash("BIP0340/aux", aux, 32, t);
  for (int i = 0; i < 32; ++i) t[i] ^= priv[i];

  // rand = tagged_hash("BIP0340/nonce", t || pub_x || msg)
  memcpy(nonce_input, t, 32);
  memcpy(nonce_input + 32, pub_x, 32);
  memcpy(nonce_input + 64, msg, 32);
  tagged_hash("BIP0340/nonce", nonce_input, 96, rand32);

  // k0 = rand mod n, must be nonzero
  if (mbedtls_mpi_read_binary(&k, rand32, 32) != 0) goto done;
  if (mbedtls_mpi_mod_mpi(&k, &k, &grp.N) != 0) goto done;
  if (mbedtls_mpi_cmp_int(&k, 0) == 0) goto done;

  // R = k*G ; if R.y odd, k = n - k
  if (mbedtls_ecp_mul(&grp, &R, &k, &grp.G, hw_rng, nullptr) != 0) goto done;
  if (mbedtls_mpi_get_bit(&R.MBEDTLS_PRIVATE(Y), 0) == 1) {
    if (mbedtls_mpi_sub_mpi(&tmp, &grp.N, &k) != 0) goto done;
    if (mbedtls_mpi_copy(&k, &tmp) != 0) goto done;
  }
  if (mbedtls_mpi_write_binary(&R.MBEDTLS_PRIVATE(X), sig, 32) != 0) goto done;  // r

  // e = tagged_hash("BIP0340/challenge", r || pub_x || msg) mod n
  memcpy(challenge_input, sig, 32);
  memcpy(challenge_input + 32, pub_x, 32);
  memcpy(challenge_input + 64, msg, 32);
  {
    uint8_t e_bytes[32];
    tagged_hash("BIP0340/challenge", challenge_input, 96, e_bytes);
    if (mbedtls_mpi_read_binary(&e, e_bytes, 32) != 0) goto done;
    if (mbedtls_mpi_mod_mpi(&e, &e, &grp.N) != 0) goto done;
  }

  // s = (k + e*d) mod n
  if (mbedtls_mpi_mul_mpi(&s, &e, &d) != 0) goto done;
  if (mbedtls_mpi_add_mpi(&s, &s, &k) != 0) goto done;
  if (mbedtls_mpi_mod_mpi(&s, &s, &grp.N) != 0) goto done;
  if (mbedtls_mpi_write_binary(&s, sig + 32, 32) != 0) goto done;

  // Never let a bad signature leave the device.
  ok = verify(pub_x, msg, sig);

done:
  // Zeroize nonce material.
  mbedtls_mpi_free(&d); mbedtls_mpi_free(&k); mbedtls_mpi_free(&e);
  mbedtls_mpi_free(&s); mbedtls_mpi_free(&tmp);
  mbedtls_ecp_point_free(&R);
  mbedtls_ecp_point_free(&P);
  mbedtls_ecp_group_free(&grp);
  memset(aux, 0, sizeof(aux));
  memset(t, 0, sizeof(t));
  memset(nonce_input, 0, sizeof(nonce_input));
  memset(rand32, 0, sizeof(rand32));
  return ok;
}

// ---------------------------------------------------------------------------
// Verify: R' = s*G + (n - e)*P must have even Y and X == r.
// ---------------------------------------------------------------------------
bool verify(const uint8_t pub_x[32], const uint8_t msg[32], const uint8_t sig[64]) {
  mbedtls_ecp_group grp;
  mbedtls_ecp_point P, Rp;
  mbedtls_mpi r, s, e, ne;
  mbedtls_ecp_group_init(&grp);
  mbedtls_ecp_point_init(&P);
  mbedtls_ecp_point_init(&Rp);
  mbedtls_mpi_init(&r); mbedtls_mpi_init(&s);
  mbedtls_mpi_init(&e); mbedtls_mpi_init(&ne);

  bool ok = false;
  uint8_t challenge_input[96], e_bytes[32], rx[32];

  if (mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256K1) != 0) goto done;
  if (!lift_x(&grp, pub_x, &P)) goto done;

  if (mbedtls_mpi_read_binary(&r, sig, 32) != 0) goto done;
  if (mbedtls_mpi_cmp_mpi(&r, &grp.P) >= 0) goto done;
  if (mbedtls_mpi_read_binary(&s, sig + 32, 32) != 0) goto done;
  if (mbedtls_mpi_cmp_mpi(&s, &grp.N) >= 0) goto done;

  memcpy(challenge_input, sig, 32);
  memcpy(challenge_input + 32, pub_x, 32);
  memcpy(challenge_input + 64, msg, 32);
  tagged_hash("BIP0340/challenge", challenge_input, 96, e_bytes);
  if (mbedtls_mpi_read_binary(&e, e_bytes, 32) != 0) goto done;
  if (mbedtls_mpi_mod_mpi(&e, &e, &grp.N) != 0) goto done;

  // ne = n - e ; R' = s*G + ne*P
  if (mbedtls_mpi_sub_mpi(&ne, &grp.N, &e) != 0) goto done;
  if (mbedtls_ecp_muladd(&grp, &Rp, &s, &grp.G, &ne, &P) != 0) goto done;

  // R' must not be infinity, must have even Y, and X must equal r.
  if (mbedtls_mpi_cmp_int(&Rp.MBEDTLS_PRIVATE(Z), 0) == 0) goto done;
  if (mbedtls_mpi_get_bit(&Rp.MBEDTLS_PRIVATE(Y), 0) == 1) goto done;
  if (mbedtls_mpi_write_binary(&Rp.MBEDTLS_PRIVATE(X), rx, 32) != 0) goto done;
  if (memcmp(rx, sig, 32) != 0) goto done;
  ok = true;

done:
  mbedtls_mpi_free(&r); mbedtls_mpi_free(&s);
  mbedtls_mpi_free(&e); mbedtls_mpi_free(&ne);
  mbedtls_ecp_point_free(&P);
  mbedtls_ecp_point_free(&Rp);
  mbedtls_ecp_group_free(&grp);
  return ok;
}

} // namespace bip340
