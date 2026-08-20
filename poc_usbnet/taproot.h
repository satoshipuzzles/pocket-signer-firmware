// Taproot (P2TR) address derivation from a Nostr x-only public key.
//
// Convention: BIP-86 key-path-only tweak with no script tree —
//   t = tagged_hash("TapTweak", x)
//   Q = lift_x(x) + t*G
//   address = bech32m("bc", witness v1, x(Q))
//
// This means every Nostr keypair *is* a Bitcoin wallet: the same secret that
// signs events can key-path-spend from this address (tweaked with t), which
// is the foundation for on-device PSBT signing.
//
// Algorithm cross-checked against the official BIP-86 test vector by
// tools/taproot_check.py.
#pragma once
#include <Arduino.h>

namespace taproot {

// Derives the mainnet P2TR address for an x-only pubkey. "" on failure
// (x not on curve). Result is ~62 chars, "bc1p...".
String addressFromXonly(const uint8_t xonly[32]);

// Encodes an arbitrary segwit program as a mainnet address: v0 -> bech32
// (bc1q...), v1+ -> bech32m (bc1p...). Used to render PSBT outputs. "" on
// bad witness version / length.
String addressFromProgram(int witver, const uint8_t* prog, size_t len);

// Taproot output key (the 32-byte witness program) for an x-only pubkey:
// x(lift_x(x) + tagged_hash("TapTweak", x) * G). Lets the PSBT reviewer
// recognize inputs/outputs that belong to a device account. False if x is
// not on the curve.
bool tweakedXOnly(const uint8_t xonly[32], uint8_t out_x[32]);

// Bech32 (BIP-173, checksum const 1) encoding of arbitrary bytes under `hrp`.
// Used for LNURL: lnurlFromLud16 below. "" on failure.
String bech32Encode(const char* hrp, const uint8_t* data, size_t len);

// Converts a lud16 lightning address ("user@domain") into the uppercase
// LNURL string wallets can scan: bech32("lnurl", utf8 of the lnurlp URL).
// Passes through strings already starting with "lnurl1" (lud06), uppercased.
// "" if the input is malformed.
String lnurlFromLud16(const String& lud16);

}  // namespace taproot
