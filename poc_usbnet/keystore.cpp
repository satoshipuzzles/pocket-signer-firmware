#include "keystore.h"
#include "bech32.h"

#include <Preferences.h>
#include <esp_random.h>
#include "mbedtls/ecp.h"
#include "mbedtls/bignum.h"

namespace keystore {

static constexpr const char* kNs = "nostr";

struct Slot {
  uint8_t priv[32];
  uint8_t pub_x[32];
  String  label;
};

static Slot s_keys[MAX_KEYS];
static int s_count = 0;
static int s_active = 0;

// ---------------------------------------------------------------------------
// secp256k1 helpers (same conventions as the Signer app's nostr_keys.cpp)
// ---------------------------------------------------------------------------
static int hw_rng(void*, unsigned char* out, size_t n) {
  esp_fill_random(out, n);
  return 0;
}

// Canonicalize d to even-Y (negating if needed) and derive the x-only pubkey.
// priv is updated in place to the canonical form. Returns false if d invalid.
static bool canonicalize_and_derive(uint8_t priv[32], uint8_t pub_x[32]) {
  mbedtls_ecp_group grp;
  mbedtls_ecp_point pub;
  mbedtls_mpi d;
  mbedtls_ecp_group_init(&grp);
  mbedtls_ecp_point_init(&pub);
  mbedtls_mpi_init(&d);

  bool ok = false;
  if (mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256K1) != 0) goto done;
  if (mbedtls_mpi_read_binary(&d, priv, 32) != 0) goto done;
  if (mbedtls_mpi_cmp_int(&d, 0) == 0 || mbedtls_mpi_cmp_mpi(&d, &grp.N) >= 0) goto done;
  if (mbedtls_ecp_mul(&grp, &pub, &d, &grp.G, hw_rng, nullptr) != 0) goto done;

  if (mbedtls_mpi_get_bit(&pub.MBEDTLS_PRIVATE(Y), 0) == 1) {
    mbedtls_mpi neg;
    mbedtls_mpi_init(&neg);
    int r = mbedtls_mpi_sub_mpi(&neg, &grp.N, &d);
    if (r == 0) r = mbedtls_mpi_copy(&d, &neg);
    mbedtls_mpi_free(&neg);
    if (r != 0) goto done;
  }

  if (mbedtls_mpi_write_binary(&pub.MBEDTLS_PRIVATE(X), pub_x, 32) != 0) goto done;
  if (mbedtls_mpi_write_binary(&d, priv, 32) != 0) goto done;
  ok = true;

done:
  mbedtls_mpi_free(&d);
  mbedtls_ecp_point_free(&pub);
  mbedtls_ecp_group_free(&grp);
  return ok;
}

// ---------------------------------------------------------------------------
// NVS persistence
// ---------------------------------------------------------------------------
static void slotKeys(int i, char p[8], char x[8], char l[8]) {
  snprintf(p, 8, "k%dp", i);
  snprintf(x, 8, "k%dx", i);
  snprintf(l, 8, "k%dl", i);
}

static bool persistAll() {
  Preferences pr;
  if (!pr.begin(kNs, false)) return false;
  pr.putUChar("n", (uint8_t)s_count);
  pr.putUChar("act", (uint8_t)s_active);
  for (int i = 0; i < s_count; ++i) {
    char kp[8], kx[8], kl[8];
    slotKeys(i, kp, kx, kl);
    pr.putBytes(kp, s_keys[i].priv, 32);
    pr.putBytes(kx, s_keys[i].pub_x, 32);
    pr.putString(kl, s_keys[i].label);
  }
  // clear any stale higher slots
  for (int i = s_count; i < MAX_KEYS; ++i) {
    char kp[8], kx[8], kl[8];
    slotKeys(i, kp, kx, kl);
    pr.remove(kp); pr.remove(kx); pr.remove(kl);
  }
  pr.end();
  return true;
}

bool begin() {
  Preferences pr;
  if (!pr.begin(kNs, false)) return false;

  s_count = pr.getUChar("n", 0);
  s_active = pr.getUChar("act", 0);
  if (s_count > MAX_KEYS) s_count = MAX_KEYS;

  for (int i = 0; i < s_count; ++i) {
    char kp[8], kx[8], kl[8];
    slotKeys(i, kp, kx, kl);
    if (pr.getBytesLength(kp) != 32 || pr.getBytesLength(kx) != 32) {
      s_count = i;
      break;
    }
    pr.getBytes(kp, s_keys[i].priv, 32);
    pr.getBytes(kx, s_keys[i].pub_x, 32);
    s_keys[i].label = pr.getString(kl, "key " + String(i + 1));
  }

  // Migrate the legacy single keypair from the Signer app into slot 0.
  if (s_count == 0 && pr.getBytesLength("priv") == 32 && pr.getBytesLength("pub_x") == 32) {
    pr.getBytes("priv", s_keys[0].priv, 32);
    pr.getBytes("pub_x", s_keys[0].pub_x, 32);
    s_keys[0].label = "device key";
    s_count = 1;
    s_active = 0;
    pr.end();
    persistAll();
    Serial.println("[keystore] migrated legacy keypair to slot 0");
    return true;
  }

  if (s_active >= s_count) s_active = 0;
  pr.end();
  Serial.printf("[keystore] loaded %d key(s), active=%d\n", s_count, s_active);
  return true;
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------
int count() { return s_count; }
int activeIndex() { return s_active; }

bool setActive(int idx) {
  if (idx < 0 || idx >= s_count) return false;
  s_active = idx;
  Preferences pr;
  if (pr.begin(kNs, false)) {
    pr.putUChar("act", (uint8_t)s_active);
    pr.end();
  }
  return true;
}

String labelOf(int idx) {
  return (idx >= 0 && idx < s_count) ? s_keys[idx].label : String();
}

static String hex32(const uint8_t b[32]) {
  static const char* h = "0123456789abcdef";
  String s;
  s.reserve(64);
  for (int i = 0; i < 32; ++i) { s += h[b[i] >> 4]; s += h[b[i] & 0xF]; }
  return s;
}

String pubHexOf(int idx) {
  return (idx >= 0 && idx < s_count) ? hex32(s_keys[idx].pub_x) : String();
}

String npubOf(int idx) {
  return (idx >= 0 && idx < s_count)
             ? nostrocrypto::bech32_encode_32("npub", s_keys[idx].pub_x)
             : String();
}

bool privOf(int idx, uint8_t out[32]) {
  if (idx < 0 || idx >= s_count) return false;
  memcpy(out, s_keys[idx].priv, 32);
  return true;
}

bool pubOf(int idx, uint8_t out[32]) {
  if (idx < 0 || idx >= s_count) return false;
  memcpy(out, s_keys[idx].pub_x, 32);
  return true;
}

String activePubHex() { return pubHexOf(s_active); }
bool activePriv(uint8_t out[32]) { return privOf(s_active, out); }

// ---------------------------------------------------------------------------
// Mutations
// ---------------------------------------------------------------------------
static int addSlot(const uint8_t priv[32], const String& label) {
  uint8_t p[32], x[32];
  memcpy(p, priv, 32);
  if (!canonicalize_and_derive(p, x)) {
    Serial.println("[keystore] addSlot: derive failed");
    return -2;
  }

  for (int i = 0; i < s_count; ++i) {
    if (memcmp(s_keys[i].pub_x, x, 32) == 0) {
      Serial.printf("[keystore] addSlot: duplicate of slot %d\n", i);
      return -3;
    }
  }
  if (s_count >= MAX_KEYS) {
    Serial.println("[keystore] addSlot: store full");
    return -4;
  }

  int idx = s_count++;
  memcpy(s_keys[idx].priv, p, 32);
  memcpy(s_keys[idx].pub_x, x, 32);
  s_keys[idx].label = label.length() ? label : ("key " + String(idx + 1));
  if (!persistAll()) Serial.println("[keystore] addSlot: persist FAILED");
  memset(p, 0, 32);
  Serial.printf("[keystore] addSlot: ok, slot %d (%d total)\n", idx, s_count);
  return idx;
}

int importKey(const String& raw, const String& label) {
  uint8_t priv[32];
  String trimmed = raw;
  trimmed.trim();
  if (nostrocrypto::parse_private_key(trimmed, priv) != 0) return -1;
  int r = addSlot(priv, label);
  memset(priv, 0, 32);
  return r;
}

int generateKey(const String& label) {
  uint8_t priv[32];
  esp_fill_random(priv, 32);
  int r = addSlot(priv, label);
  memset(priv, 0, 32);
  return r;
}

bool removeKey(int idx) {
  if (idx < 0 || idx >= s_count) return false;
  for (int i = idx; i < s_count - 1; ++i) s_keys[i] = s_keys[i + 1];
  memset(&s_keys[s_count - 1], 0, sizeof(Slot::priv) + sizeof(Slot::pub_x));
  s_keys[s_count - 1].label = "";
  s_count--;
  if (s_active >= s_count) s_active = s_count > 0 ? s_count - 1 : 0;
  return persistAll();
}

} // namespace keystore
