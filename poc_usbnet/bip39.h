// BIP-39 mnemonic encoding (24 words / 256-bit entropy only).
//
// toMnemonic:   32-byte entropy -> 24 English words (8-bit checksum from
//               SHA-256(entropy), 264 bits split into 24 x 11-bit indices).
// fromMnemonic: inverse; validates every word and the checksum.
// mnemonicToSeed: PBKDF2-HMAC-SHA512(mnemonic, "mnemonic"+passphrase, 2048).
#pragma once
#include <Arduino.h>

namespace bip39 {

// Encode 32 bytes of entropy as a 24-word mnemonic.
String toMnemonic(const uint8_t entropy[32]);

// Decode a 24-word mnemonic back to 32 bytes. Returns false on unknown
// word or checksum mismatch. Accepts any whitespace separation,
// case-insensitive.
bool fromMnemonic(const String& words, uint8_t out[32]);

// BIP-39 seed: PBKDF2-HMAC-SHA512(mnemonic, "mnemonic"+passphrase, 2048)
// -> 64 bytes.
void mnemonicToSeed(const String& mnemonic, const String& passphrase,
                    uint8_t out[64]);

} // namespace bip39
