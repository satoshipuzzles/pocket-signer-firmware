// Minimal PSBT (BIP-174 v0) engine for the airgapped signer.
//
// Two spend types are supported:
//
//  1. Single-key taproot key-path spends from addresses derived by
//     taproot::addressFromXonly (BIP-86-style tweak of the Nostr key).
//     signAll() produces a fully final network transaction.
//
//  2. NDTM multisig (Nostr-Derived Taproot Multisig, same scheme as the
//     nostr-onchain-signer extension): script-path spends of a single
//     OP_CHECKSIGADD tapscript leaf over sorted Nostr x-only keys.
//     signPartial() adds this device's BIP-340 signature as a
//     PSBT_IN_TAP_SCRIPT_SIG entry; the coordinator (NostrTX PWA) merges
//     partials from each co-signer and finalizes.
//
// Both use the BIP-341 default sighash (0x00).
#pragma once
#include <Arduino.h>
#include <vector>

namespace psbt {

struct Input {
  uint8_t txid[32];   // wire order (little-endian display of the id)
  uint32_t vout = 0;
  uint32_t sequence = 0xFFFFFFFF;
  uint64_t amount = 0;
  std::vector<uint8_t> script;  // witness_utxo scriptPubKey
  std::vector<uint8_t> sig;     // 64 bytes once signed
  // Script-path (multisig) data from PSBT_IN_TAP_LEAF_SCRIPT, if present.
  std::vector<uint8_t> leafScript;
  uint8_t leafVer = 0xc0;
  uint8_t leafHash[32];         // computed during signPartial
};

struct Output {
  uint64_t amount = 0;
  std::vector<uint8_t> script;
};

struct Tx {
  uint32_t version = 2;
  uint32_t locktime = 0;
  std::vector<Input> ins;
  std::vector<Output> outs;
  String error;  // set by parse()/signAll() on failure

  bool parse(const uint8_t* data, size_t len);

  uint64_t totalIn() const;
  uint64_t totalOut() const;
  uint64_t fee() const { return totalIn() - totalOut(); }

  bool isOpReturn(size_t i) const;
  // OP_RETURN payload rendered as text (printable) or hex.
  String opReturnText(size_t i) const;
  // Address for output i, or "OP_RETURN"/"nonstandard".
  String outAddress(size_t i) const;

  // Signs every input with the (untweaked, even-Y canonical) Nostr private
  // key whose x-only pubkey is pubx. Fails if any input does not pay to
  // the tweaked key. Zeroizes intermediates.
  bool signAll(const uint8_t priv[32], const uint8_t pubx[32]);

  // Final network serialization (after signAll), lowercase hex.
  String finalHex() const;

  // --- NDTM multisig (script path) ---
  // True if any input carries a tapscript leaf (PSBT_IN_TAP_LEAF_SCRIPT).
  bool isMultisig() const;
  // "2-of-3 multisig" parsed from the CHECKSIGADD leaf, "" if not multisig.
  String multisigDesc() const;
  // Adds this key's script-path signature to every input whose leaf script
  // contains pubx. Fails if any input lacks such a leaf. Zeroizes secrets.
  bool signPartial(const uint8_t priv[32], const uint8_t pubx[32]);
  // Original PSBT with this signer's PSBT_IN_TAP_SCRIPT_SIG entries inserted.
  bool partialPsbt(std::vector<uint8_t>& out) const;
  String partialPsbtB64() const;

  // Raw PSBT bytes + per-input-map terminator offsets (kept by parse so
  // partialPsbt can re-emit the document without losing any fields).
  std::vector<uint8_t> raw;
  std::vector<size_t> inMapEnd;
  uint8_t signerPub[32];
};

}  // namespace psbt
