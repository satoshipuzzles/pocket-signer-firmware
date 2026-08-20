// BIP-340 Schnorr signatures on secp256k1, built on the mbedTLS primitives
// bundled with the ESP32 Arduino core (no extra dependencies).
//
// Implements the reference signing algorithm with hardware-entropy auxiliary
// randomness, and a full on-device verification pass so an invalid signature
// can never leave the device.
#pragma once
#include <Arduino.h>

namespace bip340 {

// sha256(sha256(tag) || sha256(tag) || data) — the BIP-340 tagged hash.
void tagged_hash(const char* tag, const uint8_t* data, size_t len, uint8_t out[32]);

// Plain sha256 helper (used for NIP-01 event ids).
void sha256(const uint8_t* data, size_t len, uint8_t out[32]);

// Sign a 32-byte message with a BIP-340-canonical (even-Y) private key.
// Writes 64 bytes (r || s) into sig. Returns false on any internal failure.
bool sign(const uint8_t priv[32], const uint8_t msg[32], uint8_t sig[64]);

// Verify sig over msg against an x-only public key. Used as a self-check
// after signing; also usable to sanity-check imported keys.
bool verify(const uint8_t pub_x[32], const uint8_t msg[32], const uint8_t sig[64]);

} // namespace bip340
