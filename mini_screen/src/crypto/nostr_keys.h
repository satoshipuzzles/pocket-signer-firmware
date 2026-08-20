// Real Nostr keypair generation on ESP32-S3.
//
// The private key is 32 bytes of hardware entropy (esp_fill_random, which
// backs onto the ESP32-S3 hardware TRNG when RF is active — see
// docs/esp_random.html). The public key is derived on the secp256k1 curve
// using mbedTLS (already bundled in the ESP32 Arduino core, no extra deps).
//
// BIP-340 convention: the x-only public key requires the point's Y coordinate
// to be even. If Y is odd, we negate the private key (d' = n - d), which
// flips Y's parity while leaving X unchanged. From that moment on, `priv`
// stores the *canonical* BIP-340 private key, ready for schnorr signing.
//
// The keypair is persisted in NVS under the "nostr" namespace, so it survives
// reboots. Call `wipe()` to zeroize + erase (danger zone).
#pragma once
#include <Arduino.h>

namespace nostrocrypto {

struct NostrKeys {
  uint8_t priv[32];   // BIP-340-canonical private key
  uint8_t pub_x[32];  // x-only public key (32 bytes)
  bool    loaded = false;
};

// Generate a fresh keypair from hardware entropy and persist it. Overwrites
// any existing stored keys. Returns true on success.
bool generate_and_store(NostrKeys& out);

// Import an externally-supplied 32-byte private key: enforces BIP-340 (even-Y
// canonicalization), derives the x-only public key, and persists both to NVS
// as the active identity. Overwrites any existing stored keys.
// Returns true on success. `raw_priv` may be in canonical or non-canonical
// form; we'll flip its parity if needed and write the canonical form.
bool import_and_store(const uint8_t raw_priv[32], NostrKeys& out);

// Load an existing keypair from NVS. Returns false if none is stored.
bool load(NostrKeys& out);

// Zeroize and erase the stored keypair from NVS.
void wipe(NostrKeys& inout);

// Convenience: format bech32 npub / nsec strings.
String to_npub(const NostrKeys& k);
String to_nsec(const NostrKeys& k);

// Utility: 32-byte -> 64-char lowercase hex.
String to_hex32(const uint8_t b[32]);

} // namespace nostrocrypto
