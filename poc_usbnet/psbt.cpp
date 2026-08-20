#include "psbt.h"

#include <esp_random.h>
#include "mbedtls/ecp.h"
#include "mbedtls/bignum.h"
#include "mbedtls/sha256.h"
#include "mbedtls/base64.h"

#include "bip340.h"
#include "taproot.h"

namespace psbt {

// ---------------------------------------------------------------------------
// Byte reader
// ---------------------------------------------------------------------------
struct Reader {
  const uint8_t* p;
  size_t n;
  size_t i = 0;
  bool fail = false;

  uint8_t u8() {
    if (i + 1 > n) { fail = true; return 0; }
    return p[i++];
  }
  uint32_t u16() {
    if (i + 2 > n) { fail = true; return 0; }
    uint32_t v = p[i] | ((uint32_t)p[i + 1] << 8);
    i += 2;
    return v;
  }
  uint32_t u32() {
    if (i + 4 > n) { fail = true; return 0; }
    uint32_t v = p[i] | ((uint32_t)p[i + 1] << 8) | ((uint32_t)p[i + 2] << 16) |
                 ((uint32_t)p[i + 3] << 24);
    i += 4;
    return v;
  }
  uint64_t u64() {
    uint64_t lo = u32();
    uint64_t hi = u32();
    return lo | (hi << 32);
  }
  uint64_t varint() {
    uint8_t b = u8();
    if (fail) return 0;
    if (b < 0xfd) return b;
    if (b == 0xfd) return u16();
    if (b == 0xfe) return u32();
    return u64();
  }
  bool bytes(uint8_t* dst, size_t k) {
    if (i + k > n) { fail = true; return false; }
    memcpy(dst, p + i, k);
    i += k;
    return true;
  }
  bool skip(size_t k) {
    if (i + k > n) { fail = true; return false; }
    i += k;
    return true;
  }
};

// ---------------------------------------------------------------------------
// Parse
// ---------------------------------------------------------------------------
bool Tx::parse(const uint8_t* data, size_t len) {
  ins.clear();
  outs.clear();
  inMapEnd.clear();
  error = "";

  if (len < 10 || memcmp(data, "psbt\xff", 5) != 0) {
    error = "not a PSBT";
    return false;
  }
  raw.assign(data, data + len);
  Reader r{data, len};
  r.i = 5;

  // --- global map: pull out the unsigned tx (keytype 0x00) ---
  std::vector<uint8_t> rawtx;
  while (true) {
    uint64_t klen = r.varint();
    if (r.fail) { error = "truncated global map"; return false; }
    if (klen == 0) break;
    if (klen > 1000 || r.i + klen > len) { error = "bad global key"; return false; }
    uint8_t ktype = data[r.i];
    r.skip(klen);
    uint64_t vlen = r.varint();
    if (r.fail || r.i + vlen > len) { error = "bad global value"; return false; }
    if (ktype == 0x00 && klen == 1)
      rawtx.assign(data + r.i, data + r.i + vlen);
    r.skip(vlen);
  }
  if (rawtx.empty()) { error = "no unsigned tx"; return false; }

  // --- unsigned tx (legacy serialization, empty scriptSigs) ---
  Reader t{rawtx.data(), rawtx.size()};
  version = t.u32();
  uint64_t nin = t.varint();
  if (t.fail || nin == 0 || nin > 32) { error = "bad input count"; return false; }
  for (uint64_t k = 0; k < nin; ++k) {
    Input in;
    t.bytes(in.txid, 32);
    in.vout = t.u32();
    uint64_t sl = t.varint();
    if (t.fail || sl != 0) { error = "unexpected scriptSig"; return false; }
    in.sequence = t.u32();
    if (t.fail) { error = "truncated input"; return false; }
    ins.push_back(in);
  }
  uint64_t nout = t.varint();
  if (t.fail || nout == 0 || nout > 64) { error = "bad output count"; return false; }
  for (uint64_t k = 0; k < nout; ++k) {
    Output o;
    o.amount = t.u64();
    uint64_t sl = t.varint();
    if (t.fail || sl > 10000 || t.i + sl > rawtx.size()) {
      error = "bad output script";
      return false;
    }
    o.script.assign(rawtx.data() + t.i, rawtx.data() + t.i + sl);
    t.skip(sl);
    outs.push_back(o);
  }
  locktime = t.u32();
  if (t.fail) { error = "truncated tx"; return false; }

  // --- per-input maps: witness_utxo (0x01) required; tap leaf (0x15) for
  //     multisig script-path spends ---
  for (size_t k = 0; k < ins.size(); ++k) {
    while (true) {
      uint64_t klen = r.varint();
      if (r.fail) { error = "truncated input map"; return false; }
      if (klen == 0) { inMapEnd.push_back(r.i - 1); break; }
      if (klen > 1000 || r.i + klen > len) { error = "bad input key"; return false; }
      uint8_t ktype = data[r.i];
      r.skip(klen);
      uint64_t vlen = r.varint();
      if (r.fail || r.i + vlen > len) { error = "bad input value"; return false; }
      if (ktype == 0x01 && klen == 1) {
        Reader w{data + r.i, (size_t)vlen};
        uint64_t amt = w.u64();
        uint64_t sl = w.varint();
        if (!w.fail && sl <= 10000 && w.i + sl <= (size_t)vlen) {
          ins[k].amount = amt;
          ins[k].script.assign(data + r.i + w.i, data + r.i + w.i + sl);
        }
      }
      // PSBT_IN_TAP_LEAF_SCRIPT: key = 0x15 || control block,
      // value = script || leaf_version (last byte). NDTM has one leaf.
      if (ktype == 0x15 && vlen >= 2 && ins[k].leafScript.empty()) {
        ins[k].leafScript.assign(data + r.i, data + r.i + vlen - 1);
        ins[k].leafVer = data[r.i + vlen - 1];
      }
      r.skip(vlen);
    }
  }

  // --- per-output maps: nothing we need, but they must parse ---
  for (size_t k = 0; k < outs.size(); ++k) {
    while (true) {
      uint64_t klen = r.varint();
      if (r.fail) break;  // trailing output maps may be absent entirely
      if (klen == 0) break;
      if (klen > 1000 || r.i + klen > len) break;
      r.skip(klen);
      uint64_t vlen = r.varint();
      if (r.fail || r.i + vlen > len) break;
      r.skip(vlen);
    }
  }

  for (size_t k = 0; k < ins.size(); ++k) {
    if (ins[k].script.empty()) {
      error = "input " + String(k) + " missing witness_utxo";
      return false;
    }
  }
  if (totalIn() < totalOut()) { error = "outputs exceed inputs"; return false; }
  return true;
}

uint64_t Tx::totalIn() const {
  uint64_t s = 0;
  for (const auto& in : ins) s += in.amount;
  return s;
}
uint64_t Tx::totalOut() const {
  uint64_t s = 0;
  for (const auto& o : outs) s += o.amount;
  return s;
}

// ---------------------------------------------------------------------------
// Output rendering
// ---------------------------------------------------------------------------
bool Tx::isOpReturn(size_t i) const {
  return i < outs.size() && !outs[i].script.empty() && outs[i].script[0] == 0x6a;
}

String Tx::opReturnText(size_t i) const {
  if (!isOpReturn(i)) return "";
  const std::vector<uint8_t>& s = outs[i].script;
  size_t off = 1, dlen = 0;
  if (s.size() >= 2 && s[1] <= 75) { dlen = s[1]; off = 2; }
  else if (s.size() >= 3 && s[1] == 0x4c) { dlen = s[2]; off = 3; }
  else if (s.size() >= 4 && s[1] == 0x4d) { dlen = s[2] | ((size_t)s[3] << 8); off = 4; }
  if (off + dlen > s.size()) dlen = s.size() - off;

  bool printable = dlen > 0;
  for (size_t k = 0; k < dlen; ++k)
    if (s[off + k] < 0x20 || s[off + k] > 0x7e) { printable = false; break; }

  if (printable) {
    String out;
    out.reserve(dlen);
    for (size_t k = 0; k < dlen; ++k) out += (char)s[off + k];
    return out;
  }
  static const char* h = "0123456789abcdef";
  String out = "0x";
  for (size_t k = 0; k < dlen && k < 24; ++k) {
    out += h[s[off + k] >> 4];
    out += h[s[off + k] & 15];
  }
  if (dlen > 24) out += "...";
  return out;
}

String Tx::outAddress(size_t i) const {
  if (i >= outs.size()) return "";
  const std::vector<uint8_t>& s = outs[i].script;
  if (!s.empty() && s[0] == 0x6a) return "OP_RETURN";
  // segwit: OP_0/OP_1..OP_16 followed by a single push of 2..40 bytes
  if (s.size() >= 4 && s.size() == (size_t)s[1] + 2 && s[1] >= 2 && s[1] <= 40) {
    int ver = -1;
    if (s[0] == 0x00) ver = 0;
    else if (s[0] >= 0x51 && s[0] <= 0x60) ver = s[0] - 0x50;
    if (ver >= 0) return taproot::addressFromProgram(ver, s.data() + 2, s[1]);
  }
  return "nonstandard";
}

// ---------------------------------------------------------------------------
// Signing (BIP-341 key path, default sighash)
// ---------------------------------------------------------------------------
static int hw_rng(void*, unsigned char* out, size_t n) {
  esp_fill_random(out, n);
  return 0;
}

static void wr32(mbedtls_sha256_context* c, uint32_t v) {
  uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
  mbedtls_sha256_update(c, b, 4);
}
static void wr64(mbedtls_sha256_context* c, uint64_t v) {
  wr32(c, (uint32_t)v);
  wr32(c, (uint32_t)(v >> 32));
}
static void wrVarint(mbedtls_sha256_context* c, uint64_t v) {
  uint8_t b[9];
  size_t n;
  if (v < 0xfd) { b[0] = (uint8_t)v; n = 1; }
  else if (v <= 0xffff) { b[0] = 0xfd; b[1] = v; b[2] = v >> 8; n = 3; }
  else { b[0] = 0xfe; b[1] = v; b[2] = v >> 8; b[3] = v >> 16; b[4] = v >> 24; n = 5; }
  mbedtls_sha256_update(c, b, n);
}

// Tweaked private key: d' = (d + tagged_hash("TapTweak", pubx)) mod n,
// negated so the resulting output key has even Y (BIP-340 canonical form,
// which bip340::sign requires). Also returns the output key's x coordinate
// so callers can double-check which scripts are actually spendable.
static bool tweakPriv(const uint8_t priv[32], const uint8_t pubx[32],
                      uint8_t out_priv[32], uint8_t out_x[32]) {
  mbedtls_ecp_group grp;
  mbedtls_ecp_point Q;
  mbedtls_mpi d, t;
  mbedtls_ecp_group_init(&grp);
  mbedtls_ecp_point_init(&Q);
  mbedtls_mpi_init(&d);
  mbedtls_mpi_init(&t);

  bool ok = false;
  uint8_t tweak[32];

  if (mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256K1) != 0) goto done;
  if (mbedtls_mpi_read_binary(&d, priv, 32) != 0) goto done;
  bip340::tagged_hash("TapTweak", pubx, 32, tweak);
  if (mbedtls_mpi_read_binary(&t, tweak, 32) != 0) goto done;
  if (mbedtls_mpi_cmp_mpi(&t, &grp.N) >= 0) goto done;

  // d' = d + t mod n
  if (mbedtls_mpi_add_mpi(&d, &d, &t) != 0) goto done;
  if (mbedtls_mpi_mod_mpi(&d, &d, &grp.N) != 0) goto done;
  if (mbedtls_mpi_cmp_int(&d, 0) == 0) goto done;

  // Q = d'*G; canonicalize to even Y
  if (mbedtls_ecp_mul(&grp, &Q, &d, &grp.G, hw_rng, nullptr) != 0) goto done;
  if (mbedtls_mpi_write_binary(&Q.MBEDTLS_PRIVATE(X), out_x, 32) != 0) goto done;
  if (mbedtls_mpi_get_bit(&Q.MBEDTLS_PRIVATE(Y), 0) == 1) {
    if (mbedtls_mpi_sub_mpi(&d, &grp.N, &d) != 0) goto done;
  }
  if (mbedtls_mpi_write_binary(&d, out_priv, 32) != 0) goto done;
  ok = true;

done:
  mbedtls_mpi_free(&d);
  mbedtls_mpi_free(&t);
  mbedtls_ecp_point_free(&Q);
  mbedtls_ecp_group_free(&grp);
  memset(tweak, 0, sizeof(tweak));
  return ok;
}

// BIP-341 midstate hashes over all inputs/outputs (shared by key-path and
// script-path sighash).
static void midstates(const Tx& tx, uint8_t sha_prevouts[32],
                      uint8_t sha_amounts[32], uint8_t sha_scripts[32],
                      uint8_t sha_seqs[32], uint8_t sha_outs[32]) {
  mbedtls_sha256_context c;
  mbedtls_sha256_init(&c);

  mbedtls_sha256_starts(&c, 0);
  for (const auto& in : tx.ins) {
    mbedtls_sha256_update(&c, in.txid, 32);
    wr32(&c, in.vout);
  }
  mbedtls_sha256_finish(&c, sha_prevouts);

  mbedtls_sha256_starts(&c, 0);
  for (const auto& in : tx.ins) wr64(&c, in.amount);
  mbedtls_sha256_finish(&c, sha_amounts);

  mbedtls_sha256_starts(&c, 0);
  for (const auto& in : tx.ins) {
    wrVarint(&c, in.script.size());
    mbedtls_sha256_update(&c, in.script.data(), in.script.size());
  }
  mbedtls_sha256_finish(&c, sha_scripts);

  mbedtls_sha256_starts(&c, 0);
  for (const auto& in : tx.ins) wr32(&c, in.sequence);
  mbedtls_sha256_finish(&c, sha_seqs);

  mbedtls_sha256_starts(&c, 0);
  for (const auto& o : tx.outs) {
    wr64(&c, o.amount);
    wrVarint(&c, o.script.size());
    mbedtls_sha256_update(&c, o.script.data(), o.script.size());
  }
  mbedtls_sha256_finish(&c, sha_outs);
  mbedtls_sha256_free(&c);
}

// Common SigMsg header: epoch..spend_type..input_index. Returns bytes written.
static size_t sigMsgHeader(const Tx& tx, size_t k, uint8_t spend_type,
                           const uint8_t mid[5][32], uint8_t* msg) {
  size_t o = 0;
  msg[o++] = 0x00;  // epoch
  msg[o++] = 0x00;  // hash_type: SIGHASH_DEFAULT
  msg[o++] = (uint8_t)tx.version; msg[o++] = (uint8_t)(tx.version >> 8);
  msg[o++] = (uint8_t)(tx.version >> 16); msg[o++] = (uint8_t)(tx.version >> 24);
  msg[o++] = (uint8_t)tx.locktime; msg[o++] = (uint8_t)(tx.locktime >> 8);
  msg[o++] = (uint8_t)(tx.locktime >> 16); msg[o++] = (uint8_t)(tx.locktime >> 24);
  for (int m = 0; m < 5; ++m) { memcpy(msg + o, mid[m], 32); o += 32; }
  msg[o++] = spend_type;
  msg[o++] = (uint8_t)k; msg[o++] = (uint8_t)(k >> 8);
  msg[o++] = (uint8_t)(k >> 16); msg[o++] = (uint8_t)(k >> 24);
  return o;
}

bool Tx::signAll(const uint8_t priv[32], const uint8_t pubx[32]) {
  error = "";
  uint8_t tpriv[32], program[32];
  if (!tweakPriv(priv, pubx, tpriv, program)) {
    error = "key tweak failed";
    return false;
  }

  // Every input must pay to OUR tweaked key — refuse to touch anything else.
  for (size_t k = 0; k < ins.size(); ++k) {
    const std::vector<uint8_t>& s = ins[k].script;
    if (s.size() != 34 || s[0] != 0x51 || s[1] != 0x20 ||
        memcmp(s.data() + 2, program, 32) != 0) {
      memset(tpriv, 0, 32);
      error = "input " + String(k) + " is not this account's address";
      return false;
    }
  }

  uint8_t mid[5][32];
  midstates(*this, mid[0], mid[1], mid[2], mid[3], mid[4]);

  for (size_t k = 0; k < ins.size(); ++k) {
    uint8_t msg[175];
    size_t o = sigMsgHeader(*this, k, 0x00, mid, msg);  // key path, no annex

    uint8_t sighash[32], sig[64];
    bip340::tagged_hash("TapSighash", msg, o, sighash);
    if (!bip340::sign(tpriv, sighash, sig)) {
      memset(tpriv, 0, 32);
      error = "signing failed on input " + String(k);
      return false;
    }
    ins[k].sig.assign(sig, sig + 64);
  }
  memset(tpriv, 0, 32);
  return true;
}

// ---------------------------------------------------------------------------
// NDTM multisig: script-path partial signing
// ---------------------------------------------------------------------------
bool Tx::isMultisig() const {
  for (const auto& in : ins)
    if (!in.leafScript.empty()) return true;
  return false;
}

// Leaf format: <k1> CHECKSIG <k2> CHECKSIGADD ... <kn> CHECKSIGADD <m> NUMEQUAL
String Tx::multisigDesc() const {
  for (const auto& in : ins) {
    const std::vector<uint8_t>& s = in.leafScript;
    if (s.size() < 36) continue;
    int n = 0;
    size_t i = 0;
    while (i + 33 <= s.size() && s[i] == 0x20) {
      i += 33;
      if (i < s.size() && (s[i] == 0xac || s[i] == 0xba)) { n++; i++; }
      else break;
    }
    if (n >= 2 && i + 2 == s.size() && s[i] >= 0x51 && s[i] <= 0x60 &&
        s[i + 1] == 0x9c)
      return String(s[i] - 0x50) + "-of-" + String(n) + " multisig";
  }
  return "";
}

// d normalized so d*G has even Y (bip340::sign requires canonical keys).
static bool normalizePriv(const uint8_t priv[32], uint8_t out_priv[32]) {
  mbedtls_ecp_group grp;
  mbedtls_ecp_point P;
  mbedtls_mpi d;
  mbedtls_ecp_group_init(&grp);
  mbedtls_ecp_point_init(&P);
  mbedtls_mpi_init(&d);

  bool ok = false;
  if (mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256K1) != 0) goto done;
  if (mbedtls_mpi_read_binary(&d, priv, 32) != 0) goto done;
  if (mbedtls_mpi_cmp_int(&d, 0) == 0 ||
      mbedtls_mpi_cmp_mpi(&d, &grp.N) >= 0) goto done;
  if (mbedtls_ecp_mul(&grp, &P, &d, &grp.G, hw_rng, nullptr) != 0) goto done;
  if (mbedtls_mpi_get_bit(&P.MBEDTLS_PRIVATE(Y), 0) == 1) {
    if (mbedtls_mpi_sub_mpi(&d, &grp.N, &d) != 0) goto done;
  }
  if (mbedtls_mpi_write_binary(&d, out_priv, 32) != 0) goto done;
  ok = true;

done:
  mbedtls_mpi_free(&d);
  mbedtls_ecp_point_free(&P);
  mbedtls_ecp_group_free(&grp);
  return ok;
}

static bool scriptHasKey(const std::vector<uint8_t>& script,
                         const uint8_t pubx[32]) {
  if (script.size() < 33) return false;
  for (size_t i = 0; i + 33 <= script.size(); ++i)
    if (script[i] == 0x20 && memcmp(script.data() + i + 1, pubx, 32) == 0)
      return true;
  return false;
}

bool Tx::signPartial(const uint8_t priv[32], const uint8_t pubx[32]) {
  error = "";
  for (size_t k = 0; k < ins.size(); ++k) {
    if (ins[k].leafScript.empty()) {
      error = "input " + String(k) + " has no tapscript leaf";
      return false;
    }
    if (!scriptHasKey(ins[k].leafScript, pubx)) {
      error = "this key is not a co-signer on input " + String(k);
      return false;
    }
  }

  uint8_t npriv[32];
  if (!normalizePriv(priv, npriv)) {
    error = "bad private key";
    return false;
  }
  memcpy(signerPub, pubx, 32);

  uint8_t mid[5][32];
  midstates(*this, mid[0], mid[1], mid[2], mid[3], mid[4]);

  for (size_t k = 0; k < ins.size(); ++k) {
    // tapleaf_hash = tagged_hash("TapLeaf", ver || compact_size(script) || script)
    {
      const std::vector<uint8_t>& s = ins[k].leafScript;
      std::vector<uint8_t> buf;
      buf.reserve(s.size() + 4);
      buf.push_back(ins[k].leafVer);
      if (s.size() < 0xfd) buf.push_back((uint8_t)s.size());
      else {
        buf.push_back(0xfd);
        buf.push_back((uint8_t)s.size());
        buf.push_back((uint8_t)(s.size() >> 8));
      }
      buf.insert(buf.end(), s.begin(), s.end());
      bip340::tagged_hash("TapLeaf", buf.data(), buf.size(), ins[k].leafHash);
    }

    // SigMsg with spend_type = 2 (ext_flag=1: script path, no annex),
    // ext = tapleaf_hash || key_version(0x00) || codesep_pos(0xffffffff)
    uint8_t msg[212];
    size_t o = sigMsgHeader(*this, k, 0x02, mid, msg);
    memcpy(msg + o, ins[k].leafHash, 32); o += 32;
    msg[o++] = 0x00;
    msg[o++] = 0xff; msg[o++] = 0xff; msg[o++] = 0xff; msg[o++] = 0xff;

    uint8_t sighash[32], sig[64];
    bip340::tagged_hash("TapSighash", msg, o, sighash);
    if (!bip340::sign(npriv, sighash, sig)) {
      memset(npriv, 0, 32);
      error = "signing failed on input " + String(k);
      return false;
    }
    ins[k].sig.assign(sig, sig + 64);
  }
  memset(npriv, 0, 32);
  return true;
}

// Re-emit the original PSBT bytes with one PSBT_IN_TAP_SCRIPT_SIG entry
// (key = 0x14 || pubkey || leafhash, value = 64-byte sig) inserted at the
// end of each input map.
bool Tx::partialPsbt(std::vector<uint8_t>& out) const {
  if (raw.empty() || inMapEnd.size() != ins.size()) return false;
  for (const auto& in : ins)
    if (in.sig.size() != 64) return false;

  out.clear();
  out.reserve(raw.size() + ins.size() * 131);
  size_t pos = 0;
  for (size_t k = 0; k < ins.size(); ++k) {
    out.insert(out.end(), raw.begin() + pos, raw.begin() + inMapEnd[k]);
    out.push_back(0x41);  // key length: 1 + 32 + 32
    out.push_back(0x14);  // PSBT_IN_TAP_SCRIPT_SIG
    out.insert(out.end(), signerPub, signerPub + 32);
    out.insert(out.end(), ins[k].leafHash, ins[k].leafHash + 32);
    out.push_back(0x40);  // value length: 64
    out.insert(out.end(), ins[k].sig.begin(), ins[k].sig.end());
    pos = inMapEnd[k];
  }
  out.insert(out.end(), raw.begin() + pos, raw.end());
  return true;
}

String Tx::partialPsbtB64() const {
  std::vector<uint8_t> bin;
  if (!partialPsbt(bin)) return "";
  size_t need = 0;
  mbedtls_base64_encode(nullptr, 0, &need, bin.data(), bin.size());
  std::vector<unsigned char> b64(need + 1);
  size_t olen = 0;
  if (mbedtls_base64_encode(b64.data(), b64.size(), &olen, bin.data(),
                            bin.size()) != 0)
    return "";
  b64[olen] = 0;
  return String((const char*)b64.data());
}

// ---------------------------------------------------------------------------
// Final serialization
// ---------------------------------------------------------------------------
static void hexAppend(String& s, const uint8_t* b, size_t n) {
  static const char* h = "0123456789abcdef";
  for (size_t i = 0; i < n; ++i) {
    s += h[b[i] >> 4];
    s += h[b[i] & 15];
  }
}
static void hex32(String& s, uint32_t v) {
  uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
  hexAppend(s, b, 4);
}
static void hex64(String& s, uint64_t v) {
  hex32(s, (uint32_t)v);
  hex32(s, (uint32_t)(v >> 32));
}
static void hexVarint(String& s, uint64_t v) {
  if (v < 0xfd) { uint8_t b = (uint8_t)v; hexAppend(s, &b, 1); }
  else { uint8_t b[3] = {0xfd, (uint8_t)v, (uint8_t)(v >> 8)}; hexAppend(s, b, 3); }
}

String Tx::finalHex() const {
  for (const auto& in : ins)
    if (in.sig.size() != 64) return "";

  String s;
  s.reserve(300 + ins.size() * 250 + outs.size() * 90);
  hex32(s, version);
  s += "0001";  // segwit marker + flag
  hexVarint(s, ins.size());
  for (const auto& in : ins) {
    hexAppend(s, in.txid, 32);
    hex32(s, in.vout);
    s += "00";  // empty scriptSig
    hex32(s, in.sequence);
  }
  hexVarint(s, outs.size());
  for (const auto& o : outs) {
    hex64(s, o.amount);
    hexVarint(s, o.script.size());
    hexAppend(s, o.script.data(), o.script.size());
  }
  for (const auto& in : ins) {
    s += "01";  // one witness element
    s += "40";  // 64 bytes (SIGHASH_DEFAULT — no sighash byte appended)
    hexAppend(s, in.sig.data(), in.sig.size());
  }
  hex32(s, locktime);
  return s;
}

}  // namespace psbt
