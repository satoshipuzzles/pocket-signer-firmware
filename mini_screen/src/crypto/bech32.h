// Minimal BIP-173 bech32 encoder used for Nostr npub/nsec (32-byte payload).
// This is *encode-only* and only covers the exact shape Nostr uses:
// HRP + '1' + 52 data chars + 6 checksum chars.
//
// Reference: https://github.com/nostr-protocol/nips/blob/master/19.md
#pragma once
#include <Arduino.h>

namespace nostrocrypto {

// Encodes a 32-byte payload under `hrp` (e.g. "npub", "nsec") into a bech32
// string. Returns "" on any parameter error. Output length is always 63 chars
// for a 32-byte payload.
String bech32_encode_32(const char* hrp, const uint8_t payload[32]);

// Decodes a bech32 string that carries a 32-byte payload. On success writes
// the 32 bytes into `out` and, if `hrp_out` is non-null, copies the HRP into
// it (max 8 chars + terminator). Returns true on success, false on:
//   - malformed structure (missing '1', wrong lengths, bad chars)
//   - failing checksum
//   - payload not exactly 32 bytes
bool bech32_decode_32(const String& s, uint8_t out[32], char hrp_out[16] = nullptr);

// Convenience: try to parse either bech32 (nsec1...) or 64-char lowercase hex
// into a 32-byte key. Returns 0 on success, or a negative error code:
//   -1 = empty/too short   -2 = bech32 parse fail   -3 = hex parse fail
int parse_private_key(const String& raw, uint8_t out[32], char hrp_out[16] = nullptr);

} // namespace nostrocrypto
