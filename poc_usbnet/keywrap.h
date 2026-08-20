// PIN-based key encryption (AES-256-GCM, key from PBKDF2-HMAC-SHA256).
//
// Blob layout produced by wrap(): 12-byte IV || 32-byte ciphertext ||
// 16-byte GCM tag = 60 bytes total.
#pragma once
#include <Arduino.h>

namespace keywrap {

constexpr size_t BLOB_LEN = 60;  // 12 (iv) + 32 (ct) + 16 (tag)

// Derive a 32-byte wrapping key from a numeric PIN string and 16-byte salt:
// PBKDF2-HMAC-SHA256, 20000 iterations.
void deriveKey(const String& pin, const uint8_t salt[16], uint8_t out[32]);

// AES-256-GCM. Encrypts a 32-byte plaintext into a 60-byte blob
// (12 iv + 32 ct + 16 tag). IV from hardware RNG.
void wrap(const uint8_t key[32], const uint8_t plain[32], uint8_t out[60]);

// Returns false on auth failure (wrong PIN / tampered blob).
bool unwrap(const uint8_t key[32], const uint8_t blob[60], uint8_t out[32]);

// SHA-256(key || salt), stored as a 32-byte verifier so we can distinguish
// "wrong PIN" from "corrupt data" without storing anything cheaply
// derivable back to the PIN.
void verifier(const uint8_t key[32], const uint8_t salt[16], uint8_t out[32]);

} // namespace keywrap
