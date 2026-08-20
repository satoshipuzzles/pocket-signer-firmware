#include "nostr_keys.h"
#include "bech32.h"

#include <Preferences.h>
#include <esp_system.h>
#include <esp_random.h>

#include "mbedtls/ecp.h"
#include "mbedtls/bignum.h"

namespace nostrocrypto {

// NVS keys.
static constexpr const char* kNs      = "nostr";
static constexpr const char* kKeyPriv = "priv";
static constexpr const char* kKeyPub  = "pub_x";

// ---- helpers ---------------------------------------------------------------

static bool mpi_is_zero_or_ge_n(const mbedtls_mpi* d, const mbedtls_mpi* n) {
  // Reject d == 0 (invalid) or d >= n (out of subgroup). d in [1, n-1].
  if (mbedtls_mpi_cmp_int(d, 0) == 0) return true;
  if (mbedtls_mpi_cmp_mpi(d, n) >= 0) return true;
  return false;
}

// Compute pub = d * G on secp256k1 and enforce BIP-340 (even-Y) by negating d
// if the derived point has odd Y. Writes pub.x into `pub_x_out` (32 bytes).
static bool derive_pubkey_bip340(mbedtls_mpi* d, uint8_t pub_x_out[32]) {
  mbedtls_ecp_group grp;
  mbedtls_ecp_point pub;
  mbedtls_ecp_group_init(&grp);
  mbedtls_ecp_point_init(&pub);

  bool ok = false;

  if (mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256K1) != 0) goto done;

  // mbedtls_ecp_mul insists on some form of RNG for blinding. Provide one that
  // wraps esp_fill_random so we get real hardware entropy for the countermeasure.
  {
    auto rng = [](void*, unsigned char* out, size_t n) -> int {
      esp_fill_random(out, n);
      return 0;
    };
    if (mbedtls_ecp_mul(&grp, &pub, d, &grp.G, rng, nullptr) != 0) goto done;
  }

  // If Y is odd, negate d so the equivalent point -P has even Y and same X.
  if (mbedtls_mpi_get_bit(&pub.MBEDTLS_PRIVATE(Y), 0) == 1) {
    mbedtls_mpi neg;
    mbedtls_mpi_init(&neg);
    if (mbedtls_mpi_sub_mpi(&neg, &grp.N, d) != 0) { mbedtls_mpi_free(&neg); goto done; }
    if (mbedtls_mpi_copy(d, &neg) != 0)            { mbedtls_mpi_free(&neg); goto done; }
    mbedtls_mpi_free(&neg);
  }

  if (mbedtls_mpi_write_binary(&pub.MBEDTLS_PRIVATE(X), pub_x_out, 32) != 0) goto done;
  ok = true;

done:
  mbedtls_ecp_point_free(&pub);
  mbedtls_ecp_group_free(&grp);
  return ok;
}

// ---- public API ------------------------------------------------------------

bool generate_and_store(NostrKeys& out) {
  mbedtls_ecp_group grp;
  mbedtls_mpi d;
  mbedtls_ecp_group_init(&grp);
  mbedtls_mpi_init(&d);

  bool ok = false;

  if (mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256K1) != 0) goto done;

  // Rejection-sample a valid private key from hardware entropy.
  for (int attempts = 0; attempts < 32; ++attempts) {
    esp_fill_random(out.priv, 32);
    if (mbedtls_mpi_read_binary(&d, out.priv, 32) != 0) continue;
    if (!mpi_is_zero_or_ge_n(&d, &grp.N)) break;   // in [1, n-1] → accept
    if (attempts == 31) goto done;
  }

  if (!derive_pubkey_bip340(&d, out.pub_x)) goto done;

  // Serialize the (possibly negated) canonical private key back to bytes.
  if (mbedtls_mpi_write_binary(&d, out.priv, 32) != 0) goto done;

  {
    Preferences p;
    if (!p.begin(kNs, /*ro=*/false)) goto done;
    p.putBytes(kKeyPriv, out.priv, 32);
    p.putBytes(kKeyPub,  out.pub_x, 32);
    p.end();
  }
  out.loaded = true;
  ok = true;

done:
  mbedtls_mpi_free(&d);
  mbedtls_ecp_group_free(&grp);
  return ok;
}

bool import_and_store(const uint8_t raw_priv[32], NostrKeys& out) {
  mbedtls_ecp_group grp;
  mbedtls_mpi d;
  mbedtls_ecp_group_init(&grp);
  mbedtls_mpi_init(&d);

  bool ok = false;

  if (mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256K1) != 0) goto done;
  if (mbedtls_mpi_read_binary(&d, raw_priv, 32) != 0)             goto done;
  // Reject d == 0 or d >= n (invalid).
  if (mpi_is_zero_or_ge_n(&d, &grp.N))                            goto done;

  memcpy(out.priv, raw_priv, 32);
  if (!derive_pubkey_bip340(&d, out.pub_x))                       goto done;
  // Write canonicalized private back
  if (mbedtls_mpi_write_binary(&d, out.priv, 32) != 0)            goto done;

  {
    Preferences p;
    if (!p.begin(kNs, /*ro=*/false)) goto done;
    p.putBytes(kKeyPriv, out.priv, 32);
    p.putBytes(kKeyPub,  out.pub_x, 32);
    p.end();
  }
  out.loaded = true;
  ok = true;

done:
  mbedtls_mpi_free(&d);
  mbedtls_ecp_group_free(&grp);
  return ok;
}

bool load(NostrKeys& out) {
  Serial.println("[nostr_keys] load: begin");
  Preferences p;
  // Open RW: on some ESP32-Arduino versions read-only mode on a missing
  // namespace can log noisy errors or fail unpredictably.
  if (!p.begin(kNs, /*ro=*/false)) {
    Serial.println("[nostr_keys] load: prefs.begin failed");
    return false;
  }
  size_t got_priv = p.getBytesLength(kKeyPriv);
  size_t got_pub  = p.getBytesLength(kKeyPub);
  Serial.printf("[nostr_keys] load: sizes priv=%u pub=%u\n",
                (unsigned)got_priv, (unsigned)got_pub);
  if (got_priv != 32 || got_pub != 32) { p.end(); return false; }
  p.getBytes(kKeyPriv, out.priv,  32);
  p.getBytes(kKeyPub,  out.pub_x, 32);
  p.end();
  out.loaded = true;
  Serial.println("[nostr_keys] load: ok");
  return true;
}

void wipe(NostrKeys& inout) {
  Preferences p;
  if (p.begin(kNs, /*ro=*/false)) {
    p.clear();
    p.end();
  }
  memset(inout.priv,  0, 32);
  memset(inout.pub_x, 0, 32);
  inout.loaded = false;
}

String to_npub(const NostrKeys& k) {
  return bech32_encode_32("npub", k.pub_x);
}

String to_nsec(const NostrKeys& k) {
  return bech32_encode_32("nsec", k.priv);
}

String to_hex32(const uint8_t b[32]) {
  static const char* hex = "0123456789abcdef";
  String s; s.reserve(64);
  for (int i = 0; i < 32; ++i) {
    s += hex[b[i] >> 4];
    s += hex[b[i] & 0xf];
  }
  return s;
}

} // namespace nostrocrypto
