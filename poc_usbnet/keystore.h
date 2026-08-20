// Multi-account Nostr keystore in NVS.
//
// Up to 8 keypairs, each with a short label. The single legacy keypair
// written by the mini_screen Signer app (namespace "nostr", keys
// "priv"/"pub_x") is migrated into slot 0 on first run.
//
// NVS layout (namespace "nostr"):
//   n        uint8   number of stored keys
//   act      uint8   index of the active key
//   k{i}p    32B     private key (BIP-340 canonical, even-Y)
//   k{i}x    32B     x-only public key
//   k{i}l    string  label
#pragma once
#include <Arduino.h>

namespace keystore {

constexpr int MAX_KEYS = 8;

// Load all keys from NVS (and migrate the legacy single key). Call in setup().
bool begin();

int  count();
int  activeIndex();
bool setActive(int idx);      // persists

String labelOf(int idx);
String pubHexOf(int idx);
String npubOf(int idx);       // bech32, for display
bool   privOf(int idx, uint8_t out[32]);
bool   pubOf(int idx, uint8_t out[32]);   // raw x-only pubkey (identicons etc.)

// Active-account conveniences.
String activePubHex();
bool   activePriv(uint8_t out[32]);

// Import a key given as nsec1... or 64-char hex. Canonicalizes to even-Y,
// derives the pubkey, rejects duplicates and a full store. Returns the new
// slot index, or a negative error:
//   -1 parse failure   -2 invalid key   -3 duplicate   -4 store full
int importKey(const String& raw, const String& label);

// Generate a brand new key from hardware entropy. Same return convention.
int generateKey(const String& label);

// Remove a key (compacts the list, fixes the active index).
bool removeKey(int idx);

} // namespace keystore
